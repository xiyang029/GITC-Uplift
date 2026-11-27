#include "ngx_hooks/hook_installer.hpp"

#include "nr/keys.hpp"

#include <intrin.h>
#include <psapi.h>
#include <winternl.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <mutex>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "ngx_hooks/minhook_once.hpp"
#include "nvsdk_ngx_defs_dlssg.h"
#include "nr/d3d11_handles.hpp"
#include "nr/keys.hpp"
#include "nr/log.hpp"
#include "nr/vk_handles.hpp"

namespace uplift::ngx_hooks {
namespace {

constexpr size_t INITIAL_MODULE_CAPACITY = 256u;
constexpr char HUDLESS_KEY[] = NVSDK_NGX_DLSSG_Parameter_HUDLess;
constexpr uint8_t HUDLESS_LEAVE = 0u;
constexpr uint8_t HUDLESS_HIDE = 1u;
constexpr size_t MODULE_PATH_CHARS = 32768u;
constexpr size_t ENTRY_POINT_COUNT = 20u;
constexpr size_t FIRST_D3D11 = 6u;  // The D3D11 entries precede Vulkan's entries.
constexpr size_t FIRST_VULKAN = 13u;

struct ModuleExports {
  void* create = nullptr;
  void* evaluate = nullptr;
  void* evaluate_c = nullptr;
  void* release = nullptr;
  void* shutdown1 = nullptr;
  void* cap_params = nullptr;
  void* d3d11_create = nullptr;
  void* d3d11_evaluate = nullptr;
  void* d3d11_evaluate_c = nullptr;
  void* d3d11_release = nullptr;
  void* d3d11_shutdown1 = nullptr;
  void* d3d11_shutdown = nullptr;
  void* d3d11_cap_params = nullptr;
  void* vk_create = nullptr;
  void* vk_create1 = nullptr;
  void* vk_evaluate = nullptr;
  void* vk_evaluate_c = nullptr;
  void* vk_release = nullptr;
  void* vk_shutdown1 = nullptr;
  void* vk_cap_params = nullptr;
};

using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                               NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                 const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using EvaluateCFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                  const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
// The core's Vulkan entry points (nvsdk_ngx_vk.h). ReleaseFeature has the Direct3D 12 signature.
using VkCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VkCreate1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VkEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                   PFN_NVSDK_NGX_ProgressCallback);
using VkEvaluateCFn = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                    PFN_NVSDK_NGX_ProgressCallback_C);
// The core's NVSDK_NGX_VULKAN_Shutdown1 is NOT the SDK header's one-argument function. Read from the driver's own code
// (_nvngx.dll 32.0.16.1714, the export at RVA 0xCFD0): it keeps RCX (the VkDevice) and RDX, and tail-jumps to the shared shutdown routine
// (RVA 0x44DC0) with RDX as that routine's fourth argument, a pointer it writes a 32-bit count through (`mov dword ptr [r13], eax` at
// +0x44E54, the crash of a one-argument call). It reads neither R8, R9 nor any stack argument. Streamline 2.12's NGX library (sl.common.dll)
// calls it so: `mov rcx, device; lea rdx, [remaining]; call`, then checks the count. The detour takes four pointer-sized parameters (x64: RCX,
// RDX, R8, R9) and forwards them unchanged, so whatever the caller passed reaches the original as it was.
using VkShutdown1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(void*, void*, void*, void*);
// NVSDK_NGX_D3D12_Shutdown1 has the same shape (the same core, the export at RVA 0x682B0): it keeps RCX (the ID3D12Device) and RDX, calls the
// device's GetAdapterLuid (vtable +0x158) for its LUID, and calls the shared shutdown routine (RVA 0x44DC0) with the device, the LUID and RDX as the count
// pointer; it reads neither R8, R9 nor any stack argument. Detoured with the same four pointer-sized parameters, forwarded unchanged.
using D3D12Shutdown1Fn = VkShutdown1Fn;
// The core's Direct3D 11 entry points (nvsdk_ngx.h, the non-snippet declarations). ReleaseFeature has the Direct3D 12 signature.
using D3D11CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using D3D11EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                      PFN_NVSDK_NGX_ProgressCallback);
using D3D11EvaluateCFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                                       PFN_NVSDK_NGX_ProgressCallback_C);
// NVSDK_NGX_D3D11_Shutdown1 has the Direct3D 12 one's shape (the same core, the export at RVA 0x659E0): it keeps RCX (the ID3D11Device) and RDX,
// reads the device's LUID (a call at RVA 0x64F30), and tail-jumps to the shared shutdown routine (RVA 0x44DC0) with RDX as the count pointer; it reads neither
// R8, R9 nor any stack argument. Detoured with the same four pointer-sized parameters, forwarded unchanged.
using D3D11Shutdown1Fn = VkShutdown1Fn;
// The device-less NVSDK_NGX_D3D11_Shutdown (RVA 0x65970) takes no argument: it calls the shared routine with a null device and a count of its own.
// Games on an SDK older than Shutdown1 (Final Fantasy XV's DLSS 1) call it.
// The process-wide DLSS capability block (device-less, cached by the core): the render scale override
// (addon/dlss_ratio.hpp) plants its OptimalSettings callback into it.
using CapParamsFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using D3D11ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)();

// The trampolines of one hooked module copy; stored before its detours are enabled. `poisoned`
// latches when MinHook fails to disable or remove one of this slot's hooks: the hook may still be
// live, so its slot is never reused and its originals are never nulled (a live hook must never call
// a null original).
struct SlotOriginals {
  std::atomic<HMODULE> core{nullptr};  // the copy's module, for the unhooked exports the swap needs (AllocateParameters)
  std::atomic<CreateFn> create{nullptr};
  std::atomic<EvaluateFn> evaluate{nullptr};
  std::atomic<EvaluateCFn> evaluate_c{nullptr};
  std::atomic<ReleaseFn> release{nullptr};
  std::atomic<D3D12Shutdown1Fn> shutdown1{nullptr};
  std::atomic<CapParamsFn> cap_params{nullptr};  // GetCapabilityParameters (the render scale override's plant point)
  std::atomic<D3D11CreateFn> d3d11_create{nullptr};
  std::atomic<D3D11EvaluateFn> d3d11_evaluate{nullptr};
  std::atomic<D3D11EvaluateCFn> d3d11_evaluate_c{nullptr};
  std::atomic<ReleaseFn> d3d11_release{nullptr};
  std::atomic<D3D11Shutdown1Fn> d3d11_shutdown1{nullptr};
  std::atomic<CapParamsFn> d3d11_cap_params{nullptr};
  std::atomic<D3D11ShutdownFn> d3d11_shutdown{nullptr};
  std::atomic<VkCreateFn> vk_create{nullptr};
  std::atomic<VkCreate1Fn> vk_create1{nullptr};
  std::atomic<VkEvaluateFn> vk_evaluate{nullptr};
  std::atomic<VkEvaluateCFn> vk_evaluate_c{nullptr};
  std::atomic<ReleaseFn> vk_release{nullptr};
  std::atomic<VkShutdown1Fn> vk_shutdown1{nullptr};
  std::atomic<CapParamsFn> vk_cap_params{nullptr};
  std::atomic<bool> poisoned{false};

  void Clear() {
    core.store(nullptr, std::memory_order_release);
    create.store(nullptr, std::memory_order_release);
    evaluate.store(nullptr, std::memory_order_release);
    evaluate_c.store(nullptr, std::memory_order_release);
    release.store(nullptr, std::memory_order_release);
    shutdown1.store(nullptr, std::memory_order_release);
    cap_params.store(nullptr, std::memory_order_release);
    d3d11_create.store(nullptr, std::memory_order_release);
    d3d11_evaluate.store(nullptr, std::memory_order_release);
    d3d11_evaluate_c.store(nullptr, std::memory_order_release);
    d3d11_release.store(nullptr, std::memory_order_release);
    d3d11_shutdown1.store(nullptr, std::memory_order_release);
    d3d11_shutdown.store(nullptr, std::memory_order_release);
    d3d11_cap_params.store(nullptr, std::memory_order_release);
    vk_create.store(nullptr, std::memory_order_release);
    vk_create1.store(nullptr, std::memory_order_release);
    vk_evaluate.store(nullptr, std::memory_order_release);
    vk_evaluate_c.store(nullptr, std::memory_order_release);
    vk_release.store(nullptr, std::memory_order_release);
    vk_shutdown1.store(nullptr, std::memory_order_release);
    vk_cap_params.store(nullptr, std::memory_order_release);
  }
};

std::array<SlotOriginals, HookInstaller::MAX_MODULES> g_originals;
std::atomic<NgxObserver*> g_observer{nullptr};
std::atomic<bool> g_installer_running{false};
thread_local uint32_t t_depth = 0u;
thread_local bool t_vulkan_evaluate = false;  // inside the original of an outermost Vulkan evaluate (InsideVulkanEvaluate)
// The slot of the outermost hooked call on this thread (CurrentNgxCore, OwnCreate, OwnRelease): the
// observer's own NGX calls go to the same module copy's originals. Restored by SlotGuard.
thread_local size_t t_call_slot = SIZE_MAX;

// Sets t_call_slot for the outermost call's observer callbacks, restoring the previous one after.
class SlotGuard {
 public:
  explicit SlotGuard(size_t slot) : previous_(std::exchange(t_call_slot, slot)) {}
  ~SlotGuard() { t_call_slot = previous_; }
  SlotGuard(const SlotGuard&) = delete;
  SlotGuard& operator=(const SlotGuard&) = delete;

 private:
  size_t previous_;
};

// Counts hooked calls on this thread, so one NGX copy forwarding to another is seen once.
class DepthGuard {
 public:
  DepthGuard() { ++t_depth; }
  ~DepthGuard() { --t_depth; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;
  [[nodiscard]] bool Nested() const { return t_depth > 1u; }
};

// Which hooked call an observer exception came from; each gets its own "logged once" latch, so a
// failure in one never silences the (independent) first report for the others.
enum class ObserverCall : uint8_t {
  CREATE,
  EVALUATE,
  RELEASE,
  SHUTDOWN,
};

// `what` is the caught exception's `.what()`, or null when it was not a std::exception. Logs at
// most once per `kind` for the life of the process: an observer that keeps throwing must never
// flood the log.
void LogObserverFailure(ObserverCall kind, const char* what) noexcept {
  static std::array<std::atomic<bool>, 4> logged{false, false, false, false};
  const size_t index = static_cast<size_t>(kind);
  if (logged[index].exchange(true)) return;
  constexpr std::array<const char*, 4> NAMES = {"create", "evaluate", "release", "shutdown"};
  try {
    if (what != nullptr) {
      nr::Logf(nr::LogLevel::ERR, "the NGX {} observer threw ({}); the call went to NGX unchanged", NAMES[index], what);
    } else {
      nr::Logf(nr::LogLevel::ERR, "the NGX {} observer threw; the call went to NGX unchanged", NAMES[index]);
    }
  } catch (...) {
    // No-op: logging here is best effort; the NGX call itself is never affected.
  }
}

// Every create detour's body (shared by the supported APIs). `call` is the call as the observer sees it;
// `original()` runs the hooked copy's own entry point with the game's arguments.
template <class Original>
NVSDK_NGX_Result RunCreate(size_t slot, CreateCall call, NVSDK_NGX_Handle** out_handle, Original&& original) {
  const DepthGuard depth;
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (depth.Nested() || observer == nullptr) return original();
  const SlotGuard guard(slot);
  NVSDK_NGX_Parameter* const parameters = call.parameters;
  try {
    observer->BeforeCreate(&call);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::CREATE, e.what());
  } catch (...) {
    LogObserverFailure(ObserverCall::CREATE, nullptr);
  }
  const auto create = [&] {
    call.submitted.reset();
    try {
      if (parameters != nullptr && KindOf(call.feature) == FeatureKind::UPSCALER) {
        call.submitted = ReadCreateSnapshot(*parameters);
      }
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::CREATE, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::CREATE, nullptr);
    }
    return original();
  };
  const NVSDK_NGX_Result result = create();
  try {
    observer->AfterCreate(call, ((out_handle != nullptr && NVSDK_NGX_SUCCEED(result)) ? *out_handle : nullptr), result);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::CREATE, e.what());
  } catch (...) {
    LogObserverFailure(ObserverCall::CREATE, nullptr);
  }
  return result;
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV CreateDetour(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                                         NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** out_handle) {
  const CreateFn original = g_originals[SLOT].create.load(std::memory_order_acquire);
  return RunCreate(SLOT,
                   {.list = list, .feature = feature, .parameters = parameters, .caller = _ReturnAddress()}, out_handle,
                   [&] { return original(list, feature, parameters, out_handle); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkCreateDetour(VkCommandBuffer buffer, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters,
                                           NVSDK_NGX_Handle** out_handle) {
  const VkCreateFn original = g_originals[SLOT].vk_create.load(std::memory_order_acquire);
  return RunCreate(SLOT,
                   {.api = NgxApi::VULKAN, .list = nr::AsList(buffer), .feature = feature, .parameters = parameters, .caller = _ReturnAddress()},
                   out_handle, [&] { return original(buffer, feature, parameters, out_handle); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkCreate1Detour(VkDevice device, VkCommandBuffer buffer, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters,
                                            NVSDK_NGX_Handle** out_handle) {
  const VkCreate1Fn original = g_originals[SLOT].vk_create1.load(std::memory_order_acquire);
  return RunCreate(
      SLOT,
      {.api = NgxApi::VULKAN, .list = nr::AsList(buffer), .vk_device = device, .feature = feature, .parameters = parameters, .caller = _ReturnAddress()},
      out_handle, [&] { return original(device, buffer, feature, parameters, out_handle); });
}

void NotifyEvaluate(NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                    NVSDK_NGX_Result result) noexcept {
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (observer == nullptr) return;
  try {
    observer->AfterEvaluate(api, list, handle, parameters, result);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::EVALUATE, e.what());
  } catch (...) {
    LogObserverFailure(ObserverCall::EVALUATE, nullptr);
  }
}

void NotifyOriginalEvaluate(EvaluateCall* call, NVSDK_NGX_Result result) noexcept {
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (observer == nullptr) return;
  try {
    observer->AfterOriginalEvaluate(call, result);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::EVALUATE, e.what());
  } catch (...) {
    LogObserverFailure(ObserverCall::EVALUATE, nullptr);
  }
}

// Changes one HUD-less resource parameter temporarily, restoring it with the type used by NGX.
class ResourceSwap {
 public:
  ResourceSwap() = default;
  ResourceSwap(const ResourceSwap&) = delete;
  ResourceSwap& operator=(const ResourceSwap&) = delete;
  void Apply(NgxApi api, NVSDK_NGX_Parameter* block, const char* key, bool hide, void* value) {
    void* const wanted = (hide ? nullptr : value);
    if (api == NgxApi::D3D12 && NVSDK_NGX_SUCCEED(block->Get(key, &game_resource_))) {
      if (hide && game_resource_ == nullptr) return;
      kind_ = Kind::D3D12;
      block->Set(key, static_cast<ID3D12Resource*>(wanted));
    } else if (NVSDK_NGX_SUCCEED(block->Get(key, &game_pointer_))) {
      if (hide && game_pointer_ == nullptr) return;
      kind_ = Kind::POINTER;
      block->Set(key, wanted);
    } else {
      return;
    }
    block_ = block;
    key_ = key;
  }
  ~ResourceSwap() { Restore(); }
  void Restore() noexcept {
    if (block_ == nullptr) return;
    if (kind_ == Kind::D3D12) {
      block_->Set(key_, game_resource_);
    } else {
      block_->Set(key_, game_pointer_);
    }
    block_ = nullptr;
    key_ = nullptr;
  }

 private:
  enum class Kind : uint8_t { D3D12, POINTER };
  NVSDK_NGX_Parameter* block_ = nullptr;
  const char* key_ = nullptr;
  Kind kind_ = Kind::D3D12;
  ID3D12Resource* game_resource_ = nullptr;
  void* game_pointer_ = nullptr;
};

// Before-upscaling Color and frame generation HUD-less swaps for one evaluate. The observer decides before the original
// runs; the destructor puts the game's own Color back on every path, so the game's block never keeps a
// texture of Uplift's past this call. It writes back with the type the game used. A Vulkan block
// holds its resources as void* (NGX_VULKAN_EVALUATE_DLSS_EXT), so only that type is read and written there. A Direct3D 11 block holds
// them as ID3D11Resource* (NGX_D3D11_EVALUATE_DLSS_EXT), read and written with that overload (a void* one, as an engine may set, takes the pointer branch).
class EvaluateSwaps {
 public:
  // `call` carries the game's handle and parameters; BeforeEvaluate decides both resource swaps
  // and may substitute `call->eval_handle`. OnColorSwapRejected still reports the game's handle.
  EvaluateSwaps(EvaluateCall* call, const NVSDK_NGX_Handle* game_handle) noexcept {
    NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
    if (observer == nullptr || call->parameters == nullptr) return;
    try {
      observer->BeforeEvaluate(call);
      // NGX takes the block as const, but it is the game's own mutable object, set again every frame.
      auto* const block = const_cast<NVSDK_NGX_Parameter*>(call->parameters);
      if (call->api == NgxApi::D3D12 && call->native_nr_controls) {
        native_block_ = block;
        ApplyNativeNrControls(block, *call->native_nr_controls);
      }
      if (call->api == NgxApi::D3D12 && call->native_nr_handle) {
        native_handle_block_ = block;
        ApplyNativeNrHandle(block, *call->native_nr_handle);
      }
      if (call->hudless_action != HUDLESS_LEAVE && (call->api == NgxApi::D3D12 || call->api == NgxApi::VULKAN)) {
        hudless_owner_ = std::move(call->hudless_owner);
        hudless_.Apply(call->api, block, HUDLESS_KEY, call->hudless_action == HUDLESS_HIDE, call->hudless);
      }
      if (call->color == nullptr) return;
      if (call->api == NgxApi::D3D11) {
        // Plan 18: `call.color` is an ID3D11Resource*, punned (nr/d3d11_handles.hpp). Fix round 1 (M-3): held for the original call, so a present on another
        // thread that releases the bridge's texture cannot free it under NGX. Final review: taken before the branch is chosen, so the ID3D11Resource* branch and
        // the void* one (an engine that sets Color as a pointer) both hold it, and the destructor releases it once, on every path (a rejected swap too).
        swapped11_ = nr::D3D11ResourceOf(call->color);
        swapped11_->AddRef();
      }
      if (call->api == NgxApi::D3D12 && NVSDK_NGX_SUCCEED(block->Get(NVSDK_NGX_Parameter_Color, &game_color_))) {
        kind_ = Kind::D3D12;
        block->Set(NVSDK_NGX_Parameter_Color, call->color);
      } else if (call->api == NgxApi::D3D11 && NVSDK_NGX_SUCCEED(block->Get(NVSDK_NGX_Parameter_Color, &game_color11_))) {
        // NGX_D3D11_EVALUATE_DLSS_EXT sets Color as an ID3D11Resource*.
        kind_ = Kind::D3D11;
        block->Set(NVSDK_NGX_Parameter_Color, swapped11_);
      } else if (NVSDK_NGX_SUCCEED(block->Get(NVSDK_NGX_Parameter_Color, &game_pointer_))) {
        kind_ = Kind::POINTER;
        block->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(call->color));
      } else {
        // Before-upscaling already applied NR into call.color, on the assumption the swap below would
        // put it where DLSS reads it. It cannot: the game set no Color for this evaluate at all, so
        // there is nothing to swap into, and the caller's status must not say NR applied.
        observer->OnColorSwapRejected(call->api, call->list, game_handle);
        return;
      }
      block_ = block;
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::EVALUATE, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::EVALUATE, nullptr);
    }
  }
  ~EvaluateSwaps() {
    RestoreNativeNrHandle();
    RestoreNativeNrControls();
    if (block_ != nullptr) {
      switch (kind_) {
        case Kind::D3D12:   block_->Set(NVSDK_NGX_Parameter_Color, game_color_); break;
        case Kind::D3D11:   block_->Set(NVSDK_NGX_Parameter_Color, game_color11_); break;
        case Kind::POINTER: block_->Set(NVSDK_NGX_Parameter_Color, game_pointer_); break;
      }
    }
    if (swapped11_ != nullptr) {
      swapped11_->Release();  // after the game's own Color is back
    }
  }
  EvaluateSwaps(const EvaluateSwaps&) = delete;
  EvaluateSwaps& operator=(const EvaluateSwaps&) = delete;

 private:
  enum class NativeValueKind : uint8_t { FLOAT, UINT, INT, UINT64 };
  struct NativeRestore {
    const char* key = nullptr;
    NativeValueKind kind = NativeValueKind::FLOAT;
    float f = 0.f;
    uint32_t u = 0u;
    int32_t i = 0;
    unsigned long long u64 = 0u;
  };
  void ApplyNativeNrControls(NVSDK_NGX_Parameter* block, const nr::Controls& controls) {
    const auto swap_float = [&](const char* key, float value) {
      float old = 0.f;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old))) {
        block->Set(key, value);
        native_control_restore_[native_control_restore_count_++] = {.key = key, .kind = NativeValueKind::FLOAT, .f = old};
      }
    };
    const auto swap_uint = [&](const char* key, uint32_t value) {
      uint32_t old = 0u;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old))) {
        block->Set(key, value);
        native_control_restore_[native_control_restore_count_++] = {.key = key, .kind = NativeValueKind::UINT, .u = old};
      }
    };
    const auto swap_int = [&](const char* key, int32_t value) {
      int32_t old = 0;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old))) {
        block->Set(key, value);
        native_control_restore_[native_control_restore_count_++] = {.key = key, .kind = NativeValueKind::INT, .i = old};
      }
    };
    swap_float(nr::keys::INTENSITY, controls.intensity);
    swap_float(nr::keys::LOCAL_TONE, controls.local_tone);
    swap_float(nr::keys::LOCAL_STRUCTURE, controls.local_structure);
    swap_float(nr::keys::SKIN_STRUCTURE, controls.skin_structure);
    swap_uint(nr::keys::STYLE, controls.style);
    // NGX games use both signed and unsigned integer representations for boolean DLSS-NR flags.
    int32_t auto_mask = 0;
    if (NVSDK_NGX_SUCCEED(block->Get(nr::keys::USE_AUTO_MASK, &auto_mask))) swap_int(nr::keys::USE_AUTO_MASK, controls.auto_mask ? 1 : 0);
    else swap_uint(nr::keys::USE_AUTO_MASK, controls.auto_mask ? 1u : 0u);
  }
  void ApplyNativeNrHandle(NVSDK_NGX_Parameter* block, const EvaluateCall::NativeNrHandleOverrides& work) {
    const std::array<const nr::keys::Resource*, 8> keys = {&nr::keys::COLOR, &nr::keys::OUTPUT, &nr::keys::DEPTH,
        &nr::keys::MVEC, &nr::keys::CONTROL_MASK, &nr::keys::BACKBUFFER, &nr::keys::UI, &nr::keys::UI_ALPHA};
    const auto swap_float = [&](const char* key, float value) {
      float old = 0.f;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old))) {
        block->Set(key, value);
        native_handle_restore_[native_handle_restore_count_++] = {.key = key, .kind = NativeValueKind::FLOAT, .f = old};
      }
    };
    const auto swap_uint = [&](const char* key, uint32_t value) {
      uint32_t old = 0u;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old))) {
        block->Set(key, value);
        native_handle_restore_[native_handle_restore_count_++] = {.key = key, .kind = NativeValueKind::UINT, .u = old};
      }
    };
    const auto swap_int = [&](const char* key, int32_t value) {
      int32_t old = 0;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old))) {
        block->Set(key, value);
        native_handle_restore_[native_handle_restore_count_++] = {.key = key, .kind = NativeValueKind::INT, .i = old};
      } else {
        swap_uint(key, static_cast<uint32_t>(value));
      }
    };
    const auto swap_dimension = [&](const char* key, uint32_t value) {
      int32_t old_int = 0;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old_int))) {
        block->Set(key, static_cast<int32_t>(value));
        native_handle_restore_[native_handle_restore_count_++] = {.key = key, .kind = NativeValueKind::INT, .i = old_int};
        return;
      }
      uint32_t old_uint = 0u;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old_uint))) {
        block->Set(key, value);
        native_handle_restore_[native_handle_restore_count_++] = {.key = key, .kind = NativeValueKind::UINT, .u = old_uint};
        return;
      }
      unsigned long long old_u64 = 0u;
      if (NVSDK_NGX_SUCCEED(block->Get(key, &old_u64))) {
        block->Set(key, static_cast<unsigned long long>(value));
        native_handle_restore_[native_handle_restore_count_++] = {.key = key, .kind = NativeValueKind::UINT64, .u64 = old_u64};
      }
    };
    for (size_t i = 0u; i < keys.size(); ++i) {
      native_handle_resources_[i].Apply(NgxApi::D3D12, block, keys[i]->resource,
          work.resources[i] == nullptr, work.resources[i]);
      swap_dimension(keys[i]->subrect.base_x, 0u);
      swap_dimension(keys[i]->subrect.base_y, 0u);
      swap_dimension(keys[i]->subrect.width, work.width);
      swap_dimension(keys[i]->subrect.height, work.height);
    }
    swap_dimension(nr::keys::WIDTH, work.width);
    swap_dimension(nr::keys::HEIGHT, work.height);
    if (work.motion_rescaled) {
      swap_float(nr::keys::MVEC_SCALE_X, 1.f);
      swap_float(nr::keys::MVEC_SCALE_Y, 1.f);
    }
    swap_int(nr::keys::UI_CORRECTION, work.ui_correction ? 1 : 0);
  }
  void RestoreNativeNrControls() noexcept {
    if (native_block_ == nullptr) return;
    while (native_control_restore_count_ > 0u) {
      const NativeRestore& restore = native_control_restore_[--native_control_restore_count_];
      switch (restore.kind) {
        case NativeValueKind::FLOAT: native_block_->Set(restore.key, restore.f); break;
        case NativeValueKind::UINT: native_block_->Set(restore.key, restore.u); break;
        case NativeValueKind::INT: native_block_->Set(restore.key, restore.i); break;
        case NativeValueKind::UINT64: native_block_->Set(restore.key, restore.u64); break;
      }
    }
  }
  void RestoreNativeNrHandle() noexcept {
    if (native_handle_block_ == nullptr) return;
    while (native_handle_restore_count_ > 0u) {
      const NativeRestore& restore = native_handle_restore_[--native_handle_restore_count_];
      switch (restore.kind) {
        case NativeValueKind::FLOAT: native_handle_block_->Set(restore.key, restore.f); break;
        case NativeValueKind::UINT: native_handle_block_->Set(restore.key, restore.u); break;
        case NativeValueKind::INT: native_handle_block_->Set(restore.key, restore.i); break;
        case NativeValueKind::UINT64: native_handle_block_->Set(restore.key, restore.u64); break;
      }
    }
    for (auto& resource : native_handle_resources_) resource.Restore();
    native_handle_block_ = nullptr;
  }
  enum class Kind : uint8_t { D3D12, D3D11, POINTER };  // the type the game's Color was read with, and is put back with
  NVSDK_NGX_Parameter* block_ = nullptr;
  Kind kind_ = Kind::D3D12;
  ID3D12Resource* game_color_ = nullptr;
  ID3D11Resource* game_color11_ = nullptr;
  ID3D11Resource* swapped11_ = nullptr;     // the bridge's texture DLSS reads, referenced until the swap ends (either branch)
  void* game_pointer_ = nullptr;
  NVSDK_NGX_Parameter* native_block_ = nullptr;
  NVSDK_NGX_Parameter* native_handle_block_ = nullptr;
  std::array<ResourceSwap, 8> native_handle_resources_{};
  std::array<NativeRestore, 8> native_control_restore_{};
  size_t native_control_restore_count_ = 0u;
  std::array<NativeRestore, 48> native_handle_restore_{};
  size_t native_handle_restore_count_ = 0u;
  // Members are destroyed in reverse order: restore the game's image before releasing Uplift's.
  std::shared_ptr<const void> hudless_owner_;
  ResourceSwap hudless_;
};

// Every evaluate detour's body (Plan 13: shared by both APIs and both callback kinds). `original(handle)`
// runs the hooked copy's own entry point with the game's arguments, or with the handle BeforeEvaluate's
// preset swap substituted; AfterEvaluate still sees the game's handle.
template <class Original>
NVSDK_NGX_Result RunEvaluate(size_t slot, NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                             const NVSDK_NGX_Parameter* parameters, Original&& original) {
  const DepthGuard depth;
  if (depth.Nested()) return original(handle);
  const SlotGuard guard(slot);
  EvaluateCall call = {.api = api, .list = list, .handle = handle, .parameters = parameters};
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  {
    const EvaluateSwaps swap(&call, handle);
    if (call.skip_original) {
      // The XMX mode generated the frame itself, into the same buffers the core would have written:
      // running the core's generation now would overwrite it (and, hooked, it produces nothing).
      result = NVSDK_NGX_Result_Success;
    } else {
      const NVSDK_NGX_Handle* const use = (call.eval_handle != nullptr ? call.eval_handle : handle);
      t_vulkan_evaluate = (api == NgxApi::VULKAN);
      result = original(use);
      t_vulkan_evaluate = false;
    }
  }  // the game's Color and HUD-less image are back before AfterEvaluate reads the block
  NotifyOriginalEvaluate(&call, result);
  NotifyEvaluate(api, list, handle, parameters, result);
  return result;
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV EvaluateDetour(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                           const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const EvaluateFn original = g_originals[SLOT].evaluate.load(std::memory_order_acquire);
  return RunEvaluate(SLOT, NgxApi::D3D12, list, handle, parameters,
                     [&](const NVSDK_NGX_Handle* use) { return original(list, use, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV EvaluateCDetour(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                            const NVSDK_NGX_Parameter* parameters,
                                            PFN_NVSDK_NGX_ProgressCallback_C callback) {
  const EvaluateCFn original = g_originals[SLOT].evaluate_c.load(std::memory_order_acquire);
  return RunEvaluate(SLOT, NgxApi::D3D12, list, handle, parameters,
                     [&](const NVSDK_NGX_Handle* use) { return original(list, use, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkEvaluateDetour(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                                             PFN_NVSDK_NGX_ProgressCallback callback) {
  const VkEvaluateFn original = g_originals[SLOT].vk_evaluate.load(std::memory_order_acquire);
  return RunEvaluate(SLOT, NgxApi::VULKAN, nr::AsList(buffer), handle, parameters,
                     [&](const NVSDK_NGX_Handle* use) { return original(buffer, use, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkEvaluateCDetour(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                                              PFN_NVSDK_NGX_ProgressCallback_C callback) {
  const VkEvaluateCFn original = g_originals[SLOT].vk_evaluate_c.load(std::memory_order_acquire);
  return RunEvaluate(SLOT, NgxApi::VULKAN, nr::AsList(buffer), handle, parameters,
                     [&](const NVSDK_NGX_Handle* use) { return original(buffer, use, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV D3D11EvaluateDetour(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                                                PFN_NVSDK_NGX_ProgressCallback callback) {
  const D3D11EvaluateFn original = g_originals[SLOT].d3d11_evaluate.load(std::memory_order_acquire);
  return RunEvaluate(SLOT, NgxApi::D3D11, nr::AsList(context), handle, parameters,
                     [&](const NVSDK_NGX_Handle* use) { return original(context, use, parameters, callback); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV D3D11EvaluateCDetour(ID3D11DeviceContext* context, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
                                                 PFN_NVSDK_NGX_ProgressCallback_C callback) {
  const D3D11EvaluateCFn original = g_originals[SLOT].d3d11_evaluate_c.load(std::memory_order_acquire);
  return RunEvaluate(SLOT, NgxApi::D3D11, nr::AsList(context), handle, parameters,
                     [&](const NVSDK_NGX_Handle* use) { return original(context, use, parameters, callback); });
}

// Both APIs' ReleaseFeature: BeforeRelease, then the hooked copy's own entry point.
NVSDK_NGX_Result RunRelease(size_t slot, NVSDK_NGX_Handle* handle, ReleaseFn original) {
  const DepthGuard depth;
  const SlotGuard guard(slot);
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (!depth.Nested() && observer != nullptr) {
    try {
      observer->BeforeRelease(handle);
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::RELEASE, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::RELEASE, nullptr);
    }
  }
  return original(handle);
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV ReleaseDetour(NVSDK_NGX_Handle* handle) {
  return RunRelease(SLOT, handle, g_originals[SLOT].release.load(std::memory_order_acquire));
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkReleaseDetour(NVSDK_NGX_Handle* handle) {
  return RunRelease(SLOT, handle, g_originals[SLOT].vk_release.load(std::memory_order_acquire));
}

// BeforeCoreShutdown on an outermost call, then the core's own Shutdown1 with the caller's four registers (VkShutdown1Fn).
// Nested calls (one copy forwarding to another, or the NR runtime's teardown reaching the core inside BeforeCoreShutdown) go straight through.
// The APIs' Shutdown1 variants (D3D12Shutdown1Fn is the same shape); `caller` is the detour's own return address.
NVSDK_NGX_Result RunShutdown1(NgxApi api, VkShutdown1Fn original, const void* caller, void* device, void* second, void* third, void* fourth) {
  const DepthGuard depth;
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (!depth.Nested() && observer != nullptr) {
    try {
      observer->BeforeCoreShutdown(api, device, caller);
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::SHUTDOWN, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::SHUTDOWN, nullptr);
    }
  }
  return original(device, second, third, fourth);
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV Shutdown1Detour(void* device, void* second, void* third, void* fourth) {
  return RunShutdown1(NgxApi::D3D12, g_originals[SLOT].shutdown1.load(std::memory_order_acquire), _ReturnAddress(), device, second, third, fourth);
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkShutdown1Detour(void* device, void* second, void* third, void* fourth) {
  return RunShutdown1(NgxApi::VULKAN, g_originals[SLOT].vk_shutdown1.load(std::memory_order_acquire), _ReturnAddress(), device, second, third, fourth);
}

// Plan 18 (design §2): the Direct3D 11 detours mirror the Direct3D 12 ones (their evaluate twins sit with the others above); the evaluate's
// ID3D11DeviceContext rides as the command list (nr::AsList).
template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV D3D11CreateDetour(ID3D11DeviceContext* context, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters,
                                              NVSDK_NGX_Handle** out_handle) {
  const D3D11CreateFn original = g_originals[SLOT].d3d11_create.load(std::memory_order_acquire);
  return RunCreate(SLOT,
                   {.api = NgxApi::D3D11, .list = nr::AsList(context), .feature = feature, .parameters = parameters, .caller = _ReturnAddress()},
                   out_handle, [&] { return original(context, feature, parameters, out_handle); });
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV D3D11ReleaseDetour(NVSDK_NGX_Handle* handle) {
  return RunRelease(SLOT, handle, g_originals[SLOT].d3d11_release.load(std::memory_order_acquire));
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV D3D11Shutdown1Detour(void* device, void* second, void* third, void* fourth) {
  return RunShutdown1(NgxApi::D3D11, g_originals[SLOT].d3d11_shutdown1.load(std::memory_order_acquire), _ReturnAddress(), device, second, third, fourth);
}

// The device-less NVSDK_NGX_D3D11_Shutdown: BeforeCoreShutdown with a null device on an outermost call, then the core's own.
NVSDK_NGX_Result RunShutdownAll(D3D11ShutdownFn original, const void* caller) {
  const DepthGuard depth;
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (!depth.Nested() && observer != nullptr) {
    try {
      observer->BeforeCoreShutdown(NgxApi::D3D11, nullptr, caller);
    } catch (const std::exception& e) {
      LogObserverFailure(ObserverCall::SHUTDOWN, e.what());
    } catch (...) {
      LogObserverFailure(ObserverCall::SHUTDOWN, nullptr);
    }
  }
  return original();
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV D3D11ShutdownDetour() {
  return RunShutdownAll(g_originals[SLOT].d3d11_shutdown.load(std::memory_order_acquire), _ReturnAddress());
}

// The process-wide capability block, after the original returned it: the one render-scale-override
// plant point. The original's result is forwarded either way; a null block or a nested call (one copy
// forwarding to another) goes to the core's own.
NVSDK_NGX_Result RunCapabilityParameters(NgxApi api, NVSDK_NGX_Parameter** out_parameters, CapParamsFn original) {
  const DepthGuard depth;
  const NVSDK_NGX_Result result = original(out_parameters);
  if (depth.Nested() || NVSDK_NGX_FAILED(result) || out_parameters == nullptr || *out_parameters == nullptr) return result;
  NgxObserver* const observer = g_observer.load(std::memory_order_acquire);
  if (observer == nullptr) return result;
  try {
    observer->AfterCapabilityParameters(api, *out_parameters);
  } catch (const std::exception& e) {
    LogObserverFailure(ObserverCall::CREATE, e.what());
  } catch (...) {
    LogObserverFailure(ObserverCall::CREATE, nullptr);
  }
  return result;
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV CapParamsDetour(NVSDK_NGX_Parameter** out_parameters) {
  return RunCapabilityParameters(NgxApi::D3D12, out_parameters, g_originals[SLOT].cap_params.load(std::memory_order_acquire));
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV D3D11CapParamsDetour(NVSDK_NGX_Parameter** out_parameters) {
  return RunCapabilityParameters(NgxApi::D3D11, out_parameters, g_originals[SLOT].d3d11_cap_params.load(std::memory_order_acquire));
}

template <size_t SLOT>
NVSDK_NGX_Result NVSDK_CONV VkCapParamsDetour(NVSDK_NGX_Parameter** out_parameters) {
  return RunCapabilityParameters(NgxApi::VULKAN, out_parameters, g_originals[SLOT].vk_cap_params.load(std::memory_order_acquire));
}

struct DetourSet {
  CreateFn create;
  EvaluateFn evaluate;
  EvaluateCFn evaluate_c;
  ReleaseFn release;
  D3D12Shutdown1Fn shutdown1;
  CapParamsFn cap_params;
  D3D11CreateFn d3d11_create;
  D3D11EvaluateFn d3d11_evaluate;
  D3D11EvaluateCFn d3d11_evaluate_c;
  ReleaseFn d3d11_release;
  D3D11Shutdown1Fn d3d11_shutdown1;
  D3D11ShutdownFn d3d11_shutdown;
  CapParamsFn d3d11_cap_params;
  VkCreateFn vk_create;
  VkCreate1Fn vk_create1;
  VkEvaluateFn vk_evaluate;
  VkEvaluateCFn vk_evaluate_c;
  ReleaseFn vk_release;
  VkShutdown1Fn vk_shutdown1;
  CapParamsFn vk_cap_params;
};

// One detour set per module copy: each calls its own copy's trampolines.
constexpr std::array<DetourSet, HookInstaller::MAX_MODULES> DETOURS =
    []<size_t... SLOTS>(std::index_sequence<SLOTS...> /*slots*/) {
      return std::array<DetourSet, sizeof...(SLOTS)>{
          DetourSet{
              .create = &CreateDetour<SLOTS>,
              .evaluate = &EvaluateDetour<SLOTS>,
              .evaluate_c = &EvaluateCDetour<SLOTS>,
              .release = &ReleaseDetour<SLOTS>,
              .shutdown1 = &Shutdown1Detour<SLOTS>,
              .cap_params = &CapParamsDetour<SLOTS>,
              .d3d11_create = &D3D11CreateDetour<SLOTS>,
              .d3d11_evaluate = &D3D11EvaluateDetour<SLOTS>,
              .d3d11_evaluate_c = &D3D11EvaluateCDetour<SLOTS>,
              .d3d11_release = &D3D11ReleaseDetour<SLOTS>,
              .d3d11_shutdown1 = &D3D11Shutdown1Detour<SLOTS>,
              .d3d11_shutdown = &D3D11ShutdownDetour<SLOTS>,
              .d3d11_cap_params = &D3D11CapParamsDetour<SLOTS>,
              .vk_create = &VkCreateDetour<SLOTS>,
              .vk_create1 = &VkCreate1Detour<SLOTS>,
              .vk_evaluate = &VkEvaluateDetour<SLOTS>,
              .vk_evaluate_c = &VkEvaluateCDetour<SLOTS>,
              .vk_release = &VkReleaseDetour<SLOTS>,
              .vk_shutdown1 = &VkShutdown1Detour<SLOTS>,
              .vk_cap_params = &VkCapParamsDetour<SLOTS>,
          }...,
      };
    }(std::make_index_sequence<HookInstaller::MAX_MODULES>());

// LDR_DLL_NOTIFICATION_DATA: its Loaded and Unloaded members share this layout (MSDN,
// "LdrRegisterDllNotification"). The Windows SDK headers do not declare these types.
struct DllNotificationData {
  ULONG flags;
  const UNICODE_STRING* full_dll_name;
  const UNICODE_STRING* base_dll_name;
  void* dll_base;
  ULONG size_of_image;
};
using DllNotificationFn = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, void* context);
using RegisterDllNotificationFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn callback, void* context, void** cookie);
using UnregisterDllNotificationFn = LONG(NTAPI*)(void* cookie);
constexpr ULONG DLL_NOTIFICATION_LOADED = 1u;

// Runs under the loader lock: it only flags the load; Poll() hooks the module later, outside it.
VOID CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, void* context) {
  if (reason != DLL_NOTIFICATION_LOADED || data == nullptr || data->base_dll_name == nullptr || context == nullptr) return;
  const UNICODE_STRING& name = *data->base_dll_name;
  if (name.Buffer == nullptr) return;
  if (IsNgxCoreFileName(std::wstring_view(name.Buffer, name.Length / sizeof(wchar_t)))) {
    static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
  }
}

FARPROC NtdllExport(const char* name) {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return (ntdll == nullptr ? nullptr : GetProcAddress(ntdll, name));
}

std::vector<std::pair<HMODULE, std::wstring>> FindCores() {
  std::vector<HMODULE> loaded_modules(INITIAL_MODULE_CAPACITY);
  while (true) {
    DWORD needed_bytes = 0u;
    if (K32EnumProcessModules(GetCurrentProcess(), loaded_modules.data(),
                              static_cast<DWORD>(loaded_modules.size() * sizeof(HMODULE)), &needed_bytes)
        == FALSE) {
      loaded_modules.clear();
      break;
    }
    const size_t count = needed_bytes / sizeof(HMODULE);
    const bool complete = (count <= loaded_modules.size());
    loaded_modules.resize(count);
    if (complete) break;
  }
  std::vector<std::pair<HMODULE, std::wstring>> cores;
  std::wstring path(MODULE_PATH_CHARS, L'\0');
  for (const HMODULE module : loaded_modules) {
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0u || length >= path.size()) continue;
    const std::wstring_view full_path(path.data(), length);
    const size_t separator = full_path.find_last_of(L"\\/");
    const std::wstring_view file_name =
        (separator == std::wstring_view::npos ? full_path : full_path.substr(separator + 1u));
    if (IsNgxCoreFileName(file_name)) {
      cores.emplace_back(module, std::wstring(file_name));
    }
  }

  return cores;
}

ModuleExports ResolveExports(HMODULE module) {
  const auto export_address = [module](const char* export_name) {
    return reinterpret_cast<void*>(GetProcAddress(module, export_name));
  };
  return {
      .create = export_address("NVSDK_NGX_D3D12_CreateFeature"),
      .evaluate = export_address("NVSDK_NGX_D3D12_EvaluateFeature"),
      .evaluate_c = export_address("NVSDK_NGX_D3D12_EvaluateFeature_C"),
      .release = export_address("NVSDK_NGX_D3D12_ReleaseFeature"),
      .shutdown1 = export_address("NVSDK_NGX_D3D12_Shutdown1"),
      .cap_params = export_address("NVSDK_NGX_D3D12_GetCapabilityParameters"),
      .d3d11_create = export_address("NVSDK_NGX_D3D11_CreateFeature"),
      .d3d11_evaluate = export_address("NVSDK_NGX_D3D11_EvaluateFeature"),
      .d3d11_evaluate_c = export_address("NVSDK_NGX_D3D11_EvaluateFeature_C"),
      .d3d11_release = export_address("NVSDK_NGX_D3D11_ReleaseFeature"),
      .d3d11_shutdown1 = export_address("NVSDK_NGX_D3D11_Shutdown1"),
      .d3d11_shutdown = export_address("NVSDK_NGX_D3D11_Shutdown"),
      .d3d11_cap_params = export_address("NVSDK_NGX_D3D11_GetCapabilityParameters"),
      .vk_create = export_address("NVSDK_NGX_VULKAN_CreateFeature"),
      .vk_create1 = export_address("NVSDK_NGX_VULKAN_CreateFeature1"),
      .vk_evaluate = export_address("NVSDK_NGX_VULKAN_EvaluateFeature"),
      .vk_evaluate_c = export_address("NVSDK_NGX_VULKAN_EvaluateFeature_C"),
      .vk_release = export_address("NVSDK_NGX_VULKAN_ReleaseFeature"),
      .vk_shutdown1 = export_address("NVSDK_NGX_VULKAN_Shutdown1"),
      .vk_cap_params = export_address("NVSDK_NGX_VULKAN_GetCapabilityParameters"),
  };
}

// Every function `module` exports itself. A forwarder's function is another module's, which has it among its own.
std::vector<void*> ExportedFunctions(HMODULE module) {
  std::vector<void*> functions;
  const auto* const base = reinterpret_cast<const uint8_t*>(module);
  const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return functions;
  const auto* const headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (headers->Signature != IMAGE_NT_SIGNATURE) return functions;
  const IMAGE_DATA_DIRECTORY& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (directory.VirtualAddress == 0u || directory.Size == 0u) return functions;
  const auto* const exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
  const auto* const addresses = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
  for (DWORD index = 0u; index < exports->NumberOfFunctions; ++index) {
    const DWORD address = addresses[index];
    const bool forwarder = (address >= directory.VirtualAddress && address < directory.VirtualAddress + directory.Size);
    if (address != 0u && !forwarder) {
      functions.push_back(const_cast<uint8_t*>(base + address));
    }
  }
  return functions;
}

// The places named in a patch's way, without the nulls past the last.
std::vector<void*> WayOf(const hooks::ToolPatch& patch) {
  return {patch.way.begin(), std::ranges::find(patch.way, nullptr)};
}

void Rollback(size_t slot, std::span<void* const> created, std::string_view name) {
  bool poisoned = false;
  for (void* const target : created) {
    if (const MH_STATUS status = MH_DisableHook(target); status != MH_OK) {
      nr::Logf(nr::LogLevel::WARN, "MinHook could not disable a hook rolling back {}: {}", name,
               MH_StatusToString(status));
      poisoned = true;
    }
    if (const MH_STATUS status = MH_RemoveHook(target); status != MH_OK) {
      nr::Logf(nr::LogLevel::WARN, "MinHook could not remove a hook rolling back {}: {}", name,
               MH_StatusToString(status));
      poisoned = true;
    }
  }
  if (poisoned) {
    // The hook may still be live: never null its originals, and never hand this slot to another
    // module.
    g_originals[slot].poisoned.store(true, std::memory_order_release);
  } else {
    g_originals[slot].Clear();
  }
  nr::Logf(nr::LogLevel::WARN, "NGX hooks on {} failed; the DLSS placements cannot use it", name);
}

}  // namespace

bool InsideVulkanEvaluate() {
  return t_vulkan_evaluate;
}

HMODULE CurrentNgxCore() {
  const size_t slot = t_call_slot;
  if (slot >= HookInstaller::MAX_MODULES) return nullptr;
  return g_originals[slot].core.load(std::memory_order_acquire);
}

// The original entry point `api`'s own create, or Fail (a copy that exports only Vulkan's CreateFeature1
// cannot serve the swap: CreateFeature names the device through the command buffer, CreateFeature1 needs
// the VkDevice, which an evaluate does not carry).
NVSDK_NGX_Result OwnCreate(NgxApi api, ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
                           NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** out_handle) {
  const size_t slot = t_call_slot;
  if (slot >= HookInstaller::MAX_MODULES) return NVSDK_NGX_Result_Fail;
  const SlotOriginals& originals = g_originals[slot];
  switch (api) {
    case NgxApi::D3D12: {
      const CreateFn create = originals.create.load(std::memory_order_acquire);
      return (create != nullptr ? create(list, feature, parameters, out_handle) : NVSDK_NGX_Result_Fail);
    }
    case NgxApi::D3D11: {
      const D3D11CreateFn create = originals.d3d11_create.load(std::memory_order_acquire);
      return (create != nullptr ? create(reinterpret_cast<ID3D11DeviceContext*>(list), feature, parameters, out_handle)
                                : NVSDK_NGX_Result_Fail);
    }
    case NgxApi::VULKAN: {
      const VkCreateFn create = originals.vk_create.load(std::memory_order_acquire);
      return (create != nullptr ? create(reinterpret_cast<VkCommandBuffer>(list), feature, parameters, out_handle)
                                : NVSDK_NGX_Result_Fail);
    }
  }
  return NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_Result OwnRelease(NgxApi api, NVSDK_NGX_Handle* handle) {
  const size_t slot = t_call_slot;
  if (slot >= HookInstaller::MAX_MODULES) return NVSDK_NGX_Result_Fail;
  const SlotOriginals& originals = g_originals[slot];
  switch (api) {
    case NgxApi::D3D12: {
      const ReleaseFn release = originals.release.load(std::memory_order_acquire);
      return (release != nullptr ? release(handle) : NVSDK_NGX_Result_Fail);
    }
    case NgxApi::D3D11: {
      const ReleaseFn release = originals.d3d11_release.load(std::memory_order_acquire);
      return (release != nullptr ? release(handle) : NVSDK_NGX_Result_Fail);
    }
    case NgxApi::VULKAN: {
      const ReleaseFn release = originals.vk_release.load(std::memory_order_acquire);
      return (release != nullptr ? release(handle) : NVSDK_NGX_Result_Fail);
    }
  }
  return NVSDK_NGX_Result_Fail;
}

bool IsNgxCoreFileName(std::wstring_view file_name) {
  if (file_name.empty()) return false;
  const auto equals = [file_name](const wchar_t* candidate) {
    return CompareStringOrdinal(file_name.data(), static_cast<int>(file_name.size()), candidate, -1, TRUE) == CSTR_EQUAL;
  };
  return equals(L"_nvngx.dll") || equals(L"nvngx.dll");
}

std::string HookedApis(const HookedModule& module) {
  std::vector<std::string_view> names;
  if (module.d3d12) names.push_back("Direct3D 12");
  if (module.d3d11) names.push_back("Direct3D 11");
  if (module.vulkan) names.push_back("Vulkan");
  std::string text;
  for (size_t index = 0u; index < names.size(); ++index) {
    text += (index == 0u ? "" : (index + 1u == names.size() ? " and " : ", "));
    text += names[index];
  }
  return text;
}

HookInstaller::~HookInstaller() {
  Shutdown();
}

bool HookInstaller::Start(NgxObserver* observer, std::string* error) {
  const std::unique_lock lock(mutex_);
  if (started_) return true;
  if (g_installer_running.exchange(true)) {
    *error = "another NGX hook installer is already running";
    return false;
  }
  // One MH_Initialize per process, shared with the Vulkan device hook (which may have started it already).
  if (const MH_STATUS status = hooks::EnsureMinHook(); status != MH_OK) {
    g_installer_running.store(false);
    *error = std::format("MinHook failed to start: {}", MH_StatusToString(status));
    return false;
  }
  // The detours and the DLL-load notification live in this module: it must outlive them.
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         reinterpret_cast<LPCWSTR>(&OnDllNotification), &self)
      == FALSE) {
    nr::Logf(nr::LogLevel::WARN, "could not pin Uplift's module ({})", GetLastError());
  }
  g_observer.store(observer, std::memory_order_release);
  const auto register_notification =
      reinterpret_cast<RegisterDllNotificationFn>(NtdllExport("LdrRegisterDllNotification"));
  if (register_notification == nullptr
      || register_notification(0u, &OnDllNotification, &pending_, &notification_cookie_) < 0) {
    notification_cookie_ = nullptr;
    nr::Log(nr::LogLevel::WARN, "no DLL-load notification: an NGX core loaded later is hooked only at an explicit Scan");
  }
  started_ = true;
  ScanLocked();
  return true;
}

void HookInstaller::Poll() {
  if (!pending_.load(std::memory_order_relaxed)) return;
  if (!pending_.exchange(false, std::memory_order_acq_rel)) return;
  Scan();
}

size_t HookInstaller::Scan() {
  const std::unique_lock lock(mutex_);
  return ScanLocked();
}

bool HookInstaller::InstallSlot(HMODULE module, const std::wstring& file_name) {
  const std::string name = [&file_name] {
    if (file_name.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0u, file_name.data(), static_cast<int>(file_name.size()), nullptr,
                                           0, nullptr, nullptr);
    if (length <= 0) return std::string("<unnamed module>");
    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0u, file_name.data(), static_cast<int>(file_name.size()), utf8.data(), length, nullptr,
                        nullptr);
    return utf8;
  }();
  const auto hooked_already = [this](void* address) {
    return std::ranges::any_of(installed_, [address](const Installed& entry) {
      return std::ranges::find(entry.exports, address) != entry.exports.end();
    });
  };
  // Each API needs create (either variant on Vulkan), evaluate and release exports; forwarding copies use an existing hook.
  const ModuleExports exports = ResolveExports(module);
  const bool d3d12_exported = (exports.create != nullptr && exports.evaluate != nullptr && exports.release != nullptr);
  const bool d3d11_exported = (exports.d3d11_create != nullptr && exports.d3d11_evaluate != nullptr && exports.d3d11_release != nullptr);
  const bool vulkan_exported = ((exports.vk_create != nullptr || exports.vk_create1 != nullptr) && exports.vk_evaluate != nullptr && exports.vk_release != nullptr);
  if (!d3d12_exported && !d3d11_exported && !vulkan_exported) {
    nr::Logf(nr::LogLevel::INFO, "{} exports no D3D12, D3D11 or Vulkan feature entry points; not hooked", name);
    rejected_.push_back(module);
    return false;
  }
  const bool hook_d3d12 =
      (d3d12_exported && !hooked_already(exports.create) && !hooked_already(exports.evaluate) && !hooked_already(exports.release));
  const bool hook_d3d11 =
      (d3d11_exported && !hooked_already(exports.d3d11_create) && !hooked_already(exports.d3d11_evaluate) && !hooked_already(exports.d3d11_release));
  const bool hook_vulkan = (vulkan_exported && !hooked_already(exports.vk_create) && !hooked_already(exports.vk_create1) && !hooked_already(exports.vk_evaluate)
                            && !hooked_already(exports.vk_release));
  if (!hook_d3d12 && !hook_d3d11 && !hook_vulkan) {
    nr::Logf(nr::LogLevel::INFO, "{} forwards to an NGX core that is hooked already", name);
    rejected_.push_back(module);
    return false;
  }

  size_t slot = MAX_MODULES;
  for (size_t candidate = 0u; candidate < MAX_MODULES; ++candidate) {
    if (g_originals[candidate].poisoned.load(std::memory_order_acquire)) continue;
    const bool taken =
        std::ranges::any_of(installed_, [candidate](const Installed& entry) { return entry.slot == candidate; });
    if (!taken) {
      slot = candidate;
      break;
    }
  }
  if (slot == MAX_MODULES) {
    nr::Logf(nr::LogLevel::WARN, "no free NGX hook slot for {} (at most {} module copies are hooked at once)", name,
             MAX_MODULES);
    rejected_.push_back(module);
    return false;
  }

  // Entries follow SlotOriginals; absent exports and APIs not selected have null targets. Each exported create,
  // evaluate and release is required, except EvaluateFeature_C. An exported Vulkan Shutdown1 is required too,
  // unless another installed target already covers it: its observer releases or abandons native NR before the
  // core retires the device's NGX state. A failed required hook removes that module's whole Vulkan set before
  // enabling any of it. D3D12 and D3D11 keep their existing best-effort compatibility policy: an absent or
  // unhookable optional shutdown export leaves their placements available, without that guard, and is reported below.
  const DetourSet& detours = DETOURS[slot];
  struct EntryPoint {
    void* target = nullptr;
    void* detour = nullptr;
    bool required = false;
    void* original = nullptr;  // MinHook's trampoline, once hooked
    void* behind = nullptr;    // what was hooked instead of `target`: the code behind another tool's patch on it, or the patch's own jump stub
    bool stub = false;         // `behind` is that stub
    bool covered = false;      // a hook of Uplift's on `behind` is there already, and takes this entry's calls too
  };
  std::array<EntryPoint, ENTRY_POINT_COUNT> entry_points = {{
      // [0..5] Direct3D 12
      {(hook_d3d12 ? exports.create : nullptr), reinterpret_cast<void*>(detours.create), true},
      {(hook_d3d12 ? exports.evaluate : nullptr), reinterpret_cast<void*>(detours.evaluate), true},
      {(hook_d3d12 ? exports.evaluate_c : nullptr), reinterpret_cast<void*>(detours.evaluate_c), false},
      {(hook_d3d12 ? exports.release : nullptr), reinterpret_cast<void*>(detours.release), true},
      {(hook_d3d12 ? exports.shutdown1 : nullptr), reinterpret_cast<void*>(detours.shutdown1), false},
      {(hook_d3d12 ? exports.cap_params : nullptr), reinterpret_cast<void*>(detours.cap_params), false},
      // [6..12] Direct3D 11
      {(hook_d3d11 ? exports.d3d11_create : nullptr), reinterpret_cast<void*>(detours.d3d11_create), true},
      {(hook_d3d11 ? exports.d3d11_evaluate : nullptr), reinterpret_cast<void*>(detours.d3d11_evaluate), true},
      {(hook_d3d11 ? exports.d3d11_evaluate_c : nullptr), reinterpret_cast<void*>(detours.d3d11_evaluate_c), false},
      {(hook_d3d11 ? exports.d3d11_release : nullptr), reinterpret_cast<void*>(detours.d3d11_release), true},
      {(hook_d3d11 ? exports.d3d11_shutdown1 : nullptr), reinterpret_cast<void*>(detours.d3d11_shutdown1), false},
      {(hook_d3d11 ? exports.d3d11_shutdown : nullptr), reinterpret_cast<void*>(detours.d3d11_shutdown), false},
      {(hook_d3d11 ? exports.d3d11_cap_params : nullptr), reinterpret_cast<void*>(detours.d3d11_cap_params), false},
      // [13..19] Vulkan
      {(hook_vulkan ? exports.vk_create : nullptr), reinterpret_cast<void*>(detours.vk_create), true},
      {(hook_vulkan ? exports.vk_create1 : nullptr), reinterpret_cast<void*>(detours.vk_create1), true},
      {(hook_vulkan ? exports.vk_evaluate : nullptr), reinterpret_cast<void*>(detours.vk_evaluate), true},
      {(hook_vulkan ? exports.vk_evaluate_c : nullptr), reinterpret_cast<void*>(detours.vk_evaluate_c), false},
      {(hook_vulkan ? exports.vk_release : nullptr), reinterpret_cast<void*>(detours.vk_release), true},
      {(hook_vulkan && !hooked_already(exports.vk_shutdown1) ? exports.vk_shutdown1 : nullptr), reinterpret_cast<void*>(detours.vk_shutdown1), true},
      {(hook_vulkan ? exports.vk_cap_params : nullptr), reinterpret_cast<void*>(detours.vk_cap_params), false},
  }};

  // A game's NGX shutdown may FreeLibrary the core: pinned code can never be unmapped under a
  // detour. hook_installer.hpp promises every hooked module is pinned, so a failed pin rejects it.
  // Pin the named copy itself as well: the preset swap looks up AllocateParameters on it, even for forwarded exports.
  HMODULE pinned_core = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         reinterpret_cast<LPCWSTR>(module), &pinned_core) == FALSE) {
    nr::Logf(nr::LogLevel::WARN, "could not pin {} ({}); not hooked", name, GetLastError());
    rejected_.push_back(module);
    return false;
  }
  bool pinned_all = true;
  for (const EntryPoint& entry_point : entry_points) {
    if (entry_point.target == nullptr) continue;  // not exported, or its API is not taken
    HMODULE pinned_module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           static_cast<LPCWSTR>(entry_point.target), &pinned_module)
        == FALSE) {
      nr::Logf(nr::LogLevel::WARN, "could not pin {} ({}); not hooked", name, GetLastError());
      pinned_all = false;
      break;
    }
  }
  if (!pinned_all) {
    rejected_.push_back(module);
    return false;
  }

  std::vector<void*> created;
  bool ok = true;
  bool d3d11_hooked = hook_d3d11;
  bool vulkan_hooked = hook_vulkan;
  // A Direct3D 11 or Vulkan entry point that cannot be hooked costs that API's placements only, never the Direct3D 12 ones. That API's
  // hooks made so far go now: none was enabled yet, so none can be live.
  const auto drop_api = [&entry_points, &created](size_t first, size_t end) {
    for (size_t made = first; made < end; ++made) {
      entry_points[made].covered = false;
      if (entry_points[made].original == nullptr) continue;
      void* const hooked = (entry_points[made].behind != nullptr ? entry_points[made].behind : entry_points[made].target);
      MH_RemoveHook(hooked);
      std::erase(created, hooked);
      entry_points[made].original = nullptr;
    }
  };
  // The ways another tool's patches lead, on each entry and on every other function the module exports. A place on the way of another function
  // serves this entry only when that is the same entry of another core copy, and hooked: any other function's calls would pass the hook with this
  // entry's detour (a release detour passes on one argument). Such a place ends this entry's way, and it takes the relay or the entry instead. The
  // same entry may pass two hooks of Uplift's: the inner one passes a call it meets inside the outer straight on (DepthGuard).
  std::array<hooks::ToolPatch, ENTRY_POINT_COUNT> patches = {};
  for (size_t index = 0u; index < entry_points.size(); ++index) {
    if (entry_points[index].target != nullptr) {
      patches[index] = hooks::BehindPatch(entry_points[index].target);
    }
  }
  std::vector<std::vector<void*>> other_ways;
  for (void* const function : ExportedFunctions(module)) {
    if (std::ranges::any_of(entry_points, [function](const EntryPoint& entry_point) { return entry_point.target == function; })) continue;
    if (std::vector<void*> way = WayOf(hooks::BehindPatch(function)); !way.empty()) {
      other_ways.push_back(std::move(way));
    }
  }
  const auto serves_another = [this, &patches, &other_ways](size_t index, const void* address) {
    const auto on = [address](const std::vector<void*>& way) { return std::ranges::find(way, address) != way.end(); };
    for (size_t other = 0u; other < patches.size(); ++other) {
      if (other != index && std::ranges::find(patches[other].way, address) != patches[other].way.end()) return true;
    }
    if (std::ranges::any_of(other_ways, on) || std::ranges::any_of(rejected_ways_, on)) return true;
    for (const Installed& installed : installed_) {
      if (std::ranges::any_of(installed.other_ways, on)) return true;
      for (size_t other = 0u; other < installed.ways.size(); ++other) {
        const bool same_entry_hooked = (other == index && installed.hooked[other] != nullptr);
        if (!same_entry_hooked && on(installed.ways[other])) return true;
      }
    }
    return false;
  };
  // Where the calls of a hook at `hooked` go on: past the first hook of another core copy's further on its way, with that hook's own way on. The way
  // up to there is all jumps, so every argument reaches it, and no other detour takes the call. Null when no such hook is there.
  const auto past_hooks_further_on = [this](const hooks::ToolPatch& patch, const void* hooked) -> void* {
    auto place = std::ranges::find(patch.way, hooked);
    place = (place == patch.way.end() ? patch.way.begin() : std::next(place));  // a hook at the entry is before the whole way
    for (; place != patch.way.end() && *place != nullptr; ++place) {
      for (const Installed& installed : installed_) {
        for (size_t other = 0u; other < installed.hooked.size(); ++other) {
          if (installed.hooked[other] == *place && installed.originals[other] != nullptr) return installed.originals[other];
        }
      }
    }
    return nullptr;
  };
  for (size_t index = 0u; index < entry_points.size(); ++index) {
    EntryPoint& entry_point = entry_points[index];
    const bool d3d11_entry = (index >= FIRST_D3D11 && index < FIRST_VULKAN);
    const bool vulkan_entry = (index >= FIRST_VULKAN);
    // An optional entry point that another copy's hook already covers (a forwarding stub) is left to that hook.
    if (!ok || entry_point.target == nullptr || (d3d11_entry && !d3d11_hooked) || (vulkan_entry && !vulkan_hooked)
        || (!entry_point.required && hooked_already(entry_point.target))) {
      continue;
    }
    MH_STATUS status = MH_CreateHook(entry_point.target, entry_point.detour, &entry_point.original);
    // Another tool may have patched this entry first (MFG Unlocker does): hook behind its patch, so both see the call and no byte of its patch changes.
    // The entry's own hook is made first, and removed while nothing is enabled, because MinHook's "already created" is the one sign that a jump at the
    // entry is Uplift's. A hook-behind of another core copy's same entry takes this entry's calls too. A place that serves another function (above),
    // or holds any other hook of Uplift's, ends the way: all that follows it is that function's way too, or Uplift's own jump. When nothing behind
    // can be hooked the entry's hook stays, and an entry MinHook cannot hook gets the patch's own stub: a tool that checks its stub may then refuse
    // the calls, which the log says.
    if (status == MH_OK || status == MH_ERROR_UNSUPPORTED_FUNCTION) {
      const hooks::ToolPatch& patch = patches[index];
      enum class Rung : uint8_t { HOOKED, NEXT, END };
      const auto hook_behind = [this, index, &entry_point, &serves_another](void* address) {
        if (serves_another(index, address)) return Rung::END;
        void* original = nullptr;
        const MH_STATUS behind_status = MH_CreateHook(address, entry_point.detour, &original);
        const bool already = (behind_status == MH_ERROR_ALREADY_CREATED);
        const bool covered = already && std::ranges::any_of(installed_, [index, address](const Installed& other) { return other.hooked[index] == address; });
        if (behind_status != MH_OK && !covered) return (already ? Rung::END : Rung::NEXT);
        entry_point.behind = address;
        entry_point.covered = covered;
        entry_point.original = original;
        return Rung::HOOKED;
      };
      const bool entry_hooked = (status == MH_OK);
      for (void* const address : patch.code) {
        if (address == nullptr || hook_behind(address) != Rung::NEXT) break;
      }
      if (entry_point.behind == nullptr && !entry_hooked && patch.relay != nullptr) {
        entry_point.stub = (hook_behind(patch.relay) == Rung::HOOKED);
      }
      if (entry_point.behind != nullptr) {
        if (entry_hooked) {
          MH_RemoveHook(entry_point.target);
        }
        status = MH_OK;
        if (entry_point.covered) continue;
      }
    }
    if (status != MH_OK) {
      nr::Logf(nr::LogLevel::WARN, "MinHook could not hook {}: {}", name, MH_StatusToString(status));
      entry_point.original = nullptr;
      if (!entry_point.required) continue;
      if (d3d11_entry) {
        d3d11_hooked = false;
        drop_api(FIRST_D3D11, FIRST_VULKAN);
      } else if (vulkan_entry) {
        vulkan_hooked = false;
        drop_api(FIRST_VULKAN, entry_points.size());
      } else {
        ok = false;
      }
      continue;
    }
    void* const hooked = (entry_point.behind != nullptr ? entry_point.behind : entry_point.target);
    if (void* const past = past_hooks_further_on(patches[index], hooked)) {
      entry_point.original = past;
    }
    created.push_back(hooked);
  }
  ok = ok && (hook_d3d12 || d3d11_hooked || vulkan_hooked);  // something is left to hook
  const bool with_evaluate_c = (entry_points[2].original != nullptr);
  if (ok) {
    SlotOriginals& originals = g_originals[slot];
    originals.core.store(module, std::memory_order_release);
    originals.create.store(reinterpret_cast<CreateFn>(entry_points[0].original), std::memory_order_release);
    originals.evaluate.store(reinterpret_cast<EvaluateFn>(entry_points[1].original), std::memory_order_release);
    originals.evaluate_c.store(reinterpret_cast<EvaluateCFn>(entry_points[2].original), std::memory_order_release);
    originals.release.store(reinterpret_cast<ReleaseFn>(entry_points[3].original), std::memory_order_release);
    originals.shutdown1.store(reinterpret_cast<D3D12Shutdown1Fn>(entry_points[4].original), std::memory_order_release);
    originals.cap_params.store(reinterpret_cast<CapParamsFn>(entry_points[5].original), std::memory_order_release);
    originals.d3d11_create.store(reinterpret_cast<D3D11CreateFn>(entry_points[6].original), std::memory_order_release);
    originals.d3d11_evaluate.store(reinterpret_cast<D3D11EvaluateFn>(entry_points[7].original), std::memory_order_release);
    originals.d3d11_evaluate_c.store(reinterpret_cast<D3D11EvaluateCFn>(entry_points[8].original), std::memory_order_release);
    originals.d3d11_release.store(reinterpret_cast<ReleaseFn>(entry_points[9].original), std::memory_order_release);
    originals.d3d11_shutdown1.store(reinterpret_cast<D3D11Shutdown1Fn>(entry_points[10].original), std::memory_order_release);
    originals.d3d11_shutdown.store(reinterpret_cast<D3D11ShutdownFn>(entry_points[11].original), std::memory_order_release);
    originals.d3d11_cap_params.store(reinterpret_cast<CapParamsFn>(entry_points[12].original), std::memory_order_release);
    originals.vk_create.store(reinterpret_cast<VkCreateFn>(entry_points[13].original), std::memory_order_release);
    originals.vk_create1.store(reinterpret_cast<VkCreate1Fn>(entry_points[14].original), std::memory_order_release);
    originals.vk_evaluate.store(reinterpret_cast<VkEvaluateFn>(entry_points[15].original), std::memory_order_release);
    originals.vk_evaluate_c.store(reinterpret_cast<VkEvaluateCFn>(entry_points[16].original), std::memory_order_release);
    originals.vk_release.store(reinterpret_cast<ReleaseFn>(entry_points[17].original), std::memory_order_release);
    originals.vk_shutdown1.store(reinterpret_cast<VkShutdown1Fn>(entry_points[18].original), std::memory_order_release);
    originals.vk_cap_params.store(reinterpret_cast<CapParamsFn>(entry_points[19].original), std::memory_order_release);
    for (void* const target : created) {
      if (const MH_STATUS status = MH_QueueEnableHook(target); status != MH_OK) {
        nr::Logf(nr::LogLevel::WARN, "MinHook could not queue {} for enabling: {}", name, MH_StatusToString(status));
        ok = false;
      }
    }
    if (ok) {
      if (const MH_STATUS status = MH_ApplyQueued(); status != MH_OK) {
        nr::Logf(nr::LogLevel::WARN, "MinHook could not apply the queued hooks on {}: {}", name, MH_StatusToString(status));
        ok = false;
      }
    }
  }
  if (!ok) {
    Rollback(slot, created, name);
    // Its calls still go the ways the tools' patches lead: a hook of a later copy there would take them.
    for (const hooks::ToolPatch& patch : patches) {
      if (std::vector<void*> way = WayOf(patch); !way.empty()) {
        other_ways.push_back(std::move(way));
      }
    }
    std::ranges::move(other_ways, std::back_inserter(rejected_ways_));
    rejected_.push_back(module);
    return false;
  }
  const bool with_d3d12_shutdown = (hook_d3d12 && entry_points[4].original != nullptr);
  const bool with_vulkan_shutdown = (vulkan_hooked && entry_points[18].original != nullptr);
  const bool with_d3d11_shutdown = (d3d11_hooked && entry_points[10].original != nullptr);
  const bool with_d3d11_shutdown_all = (d3d11_hooked && entry_points[11].original != nullptr);
  std::vector<void*> hooked_exports;
  std::vector<void*> hooked(ENTRY_POINT_COUNT, nullptr);
  std::vector<void*> originals(ENTRY_POINT_COUNT, nullptr);
  std::vector<std::vector<void*>> ways(ENTRY_POINT_COUNT);
  size_t behind_patches = 0u;
  for (size_t index = 0u; index < entry_points.size(); ++index) {
    ways[index] = WayOf(patches[index]);
    const EntryPoint& entry_point = entry_points[index];
    if (entry_point.original == nullptr && !entry_point.covered) continue;
    hooked_exports.push_back(entry_point.target);
    hooked[index] = (entry_point.behind != nullptr ? entry_point.behind : entry_point.target);
    if (!entry_point.covered) {
      originals[index] = entry_point.original;
    }
    if (entry_point.stub) {
      nr::Logf(nr::LogLevel::WARN, "{}: another tool's jump stub is hooked itself, because nothing behind it can be hooked: that tool may refuse DLSS calls", name);
    } else if (entry_point.behind != nullptr) {
      ++behind_patches;
    }
  }
  installed_.push_back({.module = {.file_name = file_name,
                                   .module = module,
                                   .evaluate_c = with_evaluate_c,
                                   .d3d12 = hook_d3d12,
                                   .vulkan = vulkan_hooked,
                                   .vulkan_shutdown = with_vulkan_shutdown,
                                   .d3d12_shutdown = with_d3d12_shutdown,
                                   .d3d11 = d3d11_hooked,
                                   .d3d11_shutdown = with_d3d11_shutdown,
                                   .d3d11_shutdown_all = with_d3d11_shutdown_all},
                        .exports = std::move(hooked_exports),
                        .targets = std::move(created),
                        .hooked = std::move(hooked),
                        .originals = std::move(originals),
                        .ways = std::move(ways),
                        .other_ways = std::move(other_ways),
                        .slot = slot});
  if (behind_patches != 0u) {
    nr::Logf(nr::LogLevel::INFO, "{}: entry points hooked behind another tool's jump: {}", name, behind_patches);
  }
  nr::Logf(nr::LogLevel::INFO, "NGX hooks installed on {} ({} entry points; EvaluateFeature_C {}{}{}{})", name, HookedApis(installed_.back().module),
           (with_evaluate_c ? "hooked" : (exports.evaluate_c == nullptr ? "not exported" : "not hooked")),
           (!hook_d3d12 ? "" : (with_d3d12_shutdown ? "; D3D12_Shutdown1 hooked" : "; D3D12_Shutdown1 not hooked")),
           (!d3d11_hooked ? std::string()
                          : std::format("; D3D11_Shutdown1 {}; D3D11_Shutdown {}", (with_d3d11_shutdown ? "hooked" : "not hooked"),
                                        (with_d3d11_shutdown_all ? "hooked" : "not hooked"))),
           (!vulkan_hooked ? "" : (with_vulkan_shutdown ? "; VULKAN_Shutdown1 hooked" : "; VULKAN_Shutdown1 not hooked")));
  return true;
}

size_t HookInstaller::ScanLocked() {
  if (!started_) return 0u;
  const auto cores = FindCores();
  size_t hooked = 0u;
  for (const auto& [module, file_name] : cores) {
    const bool known =
        std::ranges::any_of(installed_, [module](const Installed& entry) { return entry.module.module == module; })
        || std::ranges::find(rejected_, module) != rejected_.end();
    if (known) continue;
    if (InstallSlot(module, file_name)) ++hooked;
  }
  return hooked;
}

void HookInstaller::Shutdown() {
  const std::unique_lock lock(mutex_);
  if (!started_) return;
  g_observer.store(nullptr, std::memory_order_release);
  if (notification_cookie_ != nullptr) {
    const auto unregister_notification =
        reinterpret_cast<UnregisterDllNotificationFn>(NtdllExport("LdrUnregisterDllNotification"));
    if (unregister_notification != nullptr) {
      unregister_notification(notification_cookie_);
    }
    notification_cookie_ = nullptr;
  }
  // The latest module first: its calls may go on through an earlier module's trampoline, which must outlive its hooks.
  for (const Installed& entry : std::ranges::reverse_view(installed_)) {
    bool poisoned = false;
    for (void* const target : entry.targets) {
      if (const MH_STATUS status = MH_DisableHook(target); status != MH_OK) {
        nr::Logf(nr::LogLevel::WARN, "MinHook could not disable a hook during shutdown (slot {}): {}", entry.slot,
                 MH_StatusToString(status));
        poisoned = true;
      }
      if (const MH_STATUS status = MH_RemoveHook(target); status != MH_OK) {
        nr::Logf(nr::LogLevel::WARN, "MinHook could not remove a hook during shutdown (slot {}): {}", entry.slot,
                 MH_StatusToString(status));
        poisoned = true;
      }
    }
    if (poisoned) {
      // The hook may still be live: never null its originals, and never hand this slot to another
      // module, even to a HookInstaller started later in this process.
      g_originals[entry.slot].poisoned.store(true, std::memory_order_release);
    } else {
      g_originals[entry.slot].Clear();
    }
  }
  installed_.clear();
  rejected_.clear();
  rejected_ways_.clear();
  // MinHook stays initialised while the Vulkan device hook (which lives for the process) holds it.
  if (const MH_STATUS status = hooks::ReleaseMinHook(); status != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "MinHook failed to uninitialize: {}", MH_StatusToString(status));
  }
  pending_.store(false, std::memory_order_release);
  started_ = false;
  g_installer_running.store(false);
}

bool HookInstaller::Started() const {
  const std::shared_lock lock(mutex_);
  return started_;
}

std::vector<HookedModule> HookInstaller::Modules() const {
  const std::shared_lock lock(mutex_);
  std::vector<HookedModule> modules;
  modules.reserve(installed_.size());
  for (const Installed& entry : installed_) {
    modules.push_back(entry.module);
  }
  return modules;
}

}  // namespace uplift::ngx_hooks
