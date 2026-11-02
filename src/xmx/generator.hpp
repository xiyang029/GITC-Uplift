// SPDX-License-Identifier: MIT
#pragma once
// The XMX mode's generator: XeFG's own context replaces NVIDIA's generation inside
// NVSDK_NGX_D3D12_EvaluateFeature, while NGX keeps owning presentation. Off unless
// MFGAdaUnlock.ini has [DLSSG] XmxGenerator=1. Ported from MFGAdaUnlock-RenoDx 1.8
// (docs/research/xemfg-modo-xmx-2026-11-01.md); offsets are libxess_fg.dll 1.3.1.78 and any
// other build is refused by Preflight before a byte is written.
#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "nr/ngx.hpp"
#include "xmx/host.hpp"
#include "xmx/model.hpp"
#include "xmx/vendor/cuda/cuda_route.h"
#include "xmx/vendor/runtime_layout.h"
#include "embedded_xefg_cuda.generated.h"

namespace uplift::xmx::generator {
using Microsoft::WRL::ComPtr;
constexpr DWORD kKnownTimestamp = 0x69CB0F4D;
constexpr uintptr_t kFactoryRva = 0x1853B0;
constexpr uintptr_t kContextVtableRva = 0xF45D48;
constexpr uintptr_t kInitRva = 0x1871C0;
constexpr uintptr_t kExecuteRva = 0x185800;
constexpr auto kBackbufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kInputState = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr auto kOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto kXefgInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

#pragma pack(push, 8)
struct InitParams {
  uint32_t width, height, format, flags, maxFrames, creationMask, visibleMask, reserved;
  void* tempBufferHeap;
  uint64_t bufferOffset;
  void* tempTextureHeap;
  uint64_t textureOffset;
  void* pipelineLibrary;
  uint32_t uiMode, reserved2;
};
struct Input {
  ID3D12Resource* resource;
  uint32_t baseX, baseY, width, height;
};
struct FrameData {
  Input backbuffer, depth, motion, ui, hudless;
  float view[16], projection[16], resolution[2], jitter[2], motionScale[2];
  uint32_t reset;
  float renderTimeMs;
};
struct ExecuteParams {
  FrameData previous, next;
  ID3D12Resource* const* outputs;
  const uint32_t* outputBases;
  uint8_t outputCount;
  uint8_t reserved[3];
  uint32_t networkScales;  // 0: the default
  void* descriptorHeap;    // null: XeFG's own
  uint32_t heapOffset, uiComposition;
  // The per-pair work runs in the call starting at 0; later calls reuse it.
  uint32_t firstFrame, frameCount;
};
#pragma pack(pop)
static_assert(sizeof(InitParams) == 80 && sizeof(Input) == 24 && sizeof(FrameData) == 0x118);
static_assert(offsetof(FrameData, view) == 0x78 && offsetof(FrameData, reset) == 0x110);
static_assert(offsetof(ExecuteParams, next) == 0x118 && offsetof(ExecuteParams, outputs) == 0x230);
static_assert(offsetof(ExecuteParams, outputCount) == 0x240 && offsetof(ExecuteParams, descriptorHeap) == 0x248);
static_assert(offsetof(ExecuteParams, firstFrame) == 0x258 && sizeof(ExecuteParams) == 0x260);
// One constant for the ceiling: the generator's arrays are sized by it and the model publishes it,
// so the two can never disagree. See xmx/model.hpp.
constexpr uint32_t kMaxFrames = model::kGeneratedFrames;

inline bool Enabled() { return uplift::xmx::Enabled(); }

struct Copies {
  ComPtr<ID3D12Resource> backbuffer, depth, motion, hudless;
  FrameData data{};
  bool valid{};
};
struct State {
  ComPtr<ID3D12Device> device;
  void* context{};  // the C functions' handle: { 'DX12', XefgContext* }
  HMODULE fg{};
  uint32_t width{}, height{}, flags{};
  DXGI_FORMAT format{};
  std::array<Copies, 2> frames;
  uint32_t current{};
  std::array<ComPtr<ID3D12Resource>, kMaxFrames> outputs;
  // Resources of an earlier context. They are kept only until the new context has generated a frame,
  // which is the point where the old submissions have long since been queued behind a completed one;
  // holding them for the process's life leaked ~309 MB per resolution change (a window resize, an
  // Alt-Tab out of fullscreen, a quality change), and those are not rare.
  std::vector<ComPtr<ID3D12Resource>> retired;
  uint32_t maxFrames{1};
  D3D12_RESOURCE_DESC outputDesc{};
  bool published{};
  bool failed{};
  uint32_t logs{};
};
inline State& Get() {
  static auto* state = new State;
  return *state;
}
inline void Log(const std::string& text) {
  auto& state = Get();
  if (state.logs++ < 48) xess_mod::WriteLog("generator", text);
}
// For the panel: nobody may take NVIDIA's generator for XeFG's, or the reverse.
inline std::atomic<uint64_t> g_last_generated_tick{0};
inline std::mutex g_failure_lock;
inline std::string g_failure;
inline void Fail(const std::string& reason) {
  { std::lock_guard guard(g_failure_lock); g_failure = reason; }
  Log("failed: " + reason);
}
inline std::atomic_bool g_preflight_passed{false};
// How many evaluates XeFG refused its arguments for (-4) over the process's life. Not an error state:
// a scene change or a mode switch refuses one evaluate and the next succeeds. Cumulative, so the
// panel can show whether refusals happen at all without the log narrating each one.
inline std::atomic<uint32_t> g_refused{0};
// hooked: Preflight passed, so the mode can serve this game's DLSS-G (no hook is installed by this
// route; the name is the panel's, from the module-hook design this replaced).
struct Report { bool generating{}; bool hooked{}; std::string failure; };
inline Report Snapshot() {
  const auto tick = g_last_generated_tick.load(std::memory_order_relaxed);
  std::lock_guard guard(g_failure_lock);
  return {tick != 0 && GetTickCount64() - tick < 2000, g_preflight_passed.load(), g_failure};
}
inline DWORD FileTimestamp(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS nt{};
  if (!file.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || !file.seekg(dos.e_lfanew) ||
      !file.read(reinterpret_cast<char*>(&nt), sizeof(nt)))
    return 0;
  return nt.FileHeader.TimeDateStamp;
}
// Before the mode serves a frame: XeFG's offsets are for one exact Intel build, so a mode that cannot
// run must refuse here and let the core generate instead. Reads the files without loading them, which
// also makes it safe under the loader lock. Passed or failed once per process (the files cannot change
// under a running game), so every later call is a load of a const bool.
inline bool Preflight() {
  static const bool passed = [] {
    const auto directory = AddonDirectory();
    std::error_code error;
    if (!std::filesystem::exists(directory / L"libxess_fg.dll", error)) {
      Fail("libxess_fg.dll not found");
      return false;
    }
    if (FileTimestamp(directory / L"libxess_fg.dll") != kKnownTimestamp) {
      Fail("libxess_fg.dll is not the supported 1.3.1.78");
      return false;
    }
    if (!std::filesystem::exists(directory / L"libxell.dll", error)) {
      Fail("libxell.dll not found");
      return false;
    }
    g_preflight_passed = true;
    return true;
  }();
  return passed;
}

inline int Guarded(void* function, void* context, void* a, void* b) noexcept {
  using Call = int(__cdecl*)(void*, void*, void*);
  __try {
    return reinterpret_cast<Call>(function)(context, a, b);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -100000;
  }
}

// The file beside this add-on first, so a game that ships its own (and loads it first) cannot
// substitute a build this port does not know: Preflight checked that exact file. Only when the
// add-on's folder has none does the already-loaded copy serve.
inline HMODULE OpenRuntime(const wchar_t* name) {
  const auto local = AddonDirectory() / name;
  std::error_code error;
  if (std::filesystem::exists(local, error)) {
    if (HMODULE module = LoadLibraryExW(local.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return module;
  }
  return GetModuleHandleW(name);
}

inline bool LoadContext(ID3D12Device* device) {
  auto& state = Get();
  state.fg = OpenRuntime(L"libxess_fg.dll");
  if (state.fg == nullptr) {
    Fail("libxess_fg.dll could not be loaded");
    return false;
  }
  auto* base = reinterpret_cast<unsigned char*>(state.fg);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != kKnownTimestamp) {
    Fail("unknown libxess_fg.dll build");
    return false;
  }
  if (!state.published) {
    state.published = true;
    const auto ll = OpenRuntime(L"libxell.dll");
    std::string error;
    const HMODULE fg = state.fg;
    if (ll != nullptr && model::Publish(fg, ll, error)) state.maxFrames = kMaxFrames;
    else Log("multi-frame model refused, 2x only: " + (ll == nullptr ? std::string("libxell.dll not found") : error));
    // The tensor-core route (src/xmx/vendor/cuda/): each recognized int8 conv layer program becomes
    // an NvAPI_D3D12_LaunchCubinShader launch in the provider's own command list, with no CUDA
    // context and no second queue. Must be in before the provider creates its pipeline states, i.e.
    // before the Init below. A refusal is not fatal: the DXIL port runs instead, as it did before.
    // Its own diagnostics go through SetLog; without it every cuda_* line the route emits is lost,
    // and "active" would be the only thing this build ever says about the tensor cores.
    xefg_cuda::SetLog([](const char* stage, const std::string& fields) noexcept { xess_mod::WriteLog(stage, fields); });
    if (xefg_cuda::Install(device, fg, xess_payload::Read(xess_mod::Self(), xefg_cuda::embedded::fatbin), error)) {
      const auto route = xefg_cuda::Snapshot();
      Log("tensor-core route active, compute capability " + std::to_string(route.major) + "." + std::to_string(route.minor) +
          "; " + std::to_string(route.layers) + " layer(s) recognized");
    } else {
      Log("tensor-core route refused (" + error + "); the DXIL port runs");
    }
  }
  void** wrapper = nullptr;
  const auto created = Guarded(base + kFactoryRva, device, &wrapper, nullptr);
  if (created != 0 || wrapper == nullptr) {
    Fail("factory returned " + std::to_string(created));
    return false;
  }
  // { 'DX12' magic, XefgContext* }
  if (wrapper[1] == nullptr ||
      reinterpret_cast<uintptr_t>(*static_cast<void***>(wrapper[1])) - reinterpret_cast<uintptr_t>(base) != kContextVtableRva) {
    Fail("unexpected context layout");
    return false;
  }
  state.context = wrapper;
  return true;
}
inline ComPtr<ID3D12Resource> Texture(ID3D12Device* device, D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES initial,
                                      D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  desc.Flags = flags;
  desc.MipLevels = 1;
  desc.DepthOrArraySize = 1;
  desc.SampleDesc = {1, 0};
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Alignment = 0;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&resource))))
    return nullptr;
  return resource;
}
inline void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
  if (from == to) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  list->ResourceBarrier(1, &barrier);
}
// Copies between resources left in their original states.
inline void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState,
                 ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState) {
  Barrier(list, target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
  Barrier(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(target, source);
  Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
  Barrier(list, target, D3D12_RESOURCE_STATE_COPY_DEST, targetState);
}
// The frame's copies as one batch. The shapes above cost four ResourceBarrier calls per copy; a
// frame makes six, so twenty-four calls into the driver, each with its own validation and
// submission overhead. Every transition is known before the first copy is recorded, so the batch
// goes out as two barrier calls around the copies. A resource that would appear twice keeps the
// old one-at-a-time path, so a shape this cannot express is never mis-recorded.
class CopyBatch {
 public:
  explicit CopyBatch(ID3D12GraphicsCommandList* list) : list_(list) {}
  CopyBatch(const CopyBatch&) = delete;
  CopyBatch& operator=(const CopyBatch&) = delete;
  ~CopyBatch() { Flush(); }

  void Add(ID3D12Resource* target, D3D12_RESOURCE_STATES targetState, ID3D12Resource* source,
           D3D12_RESOURCE_STATES sourceState) {
    if (target == nullptr || source == nullptr) return;
    // A duplicate would need two destinations for one resource, and the barrier array is sized for
    // kMaxBatch pairs: both fall back to the one-at-a-time path, which is correct either way.
    if (Duplicated(target) || Duplicated(source) || targets_.size() >= kMaxBatch) {
      Copy(list_, target, targetState, source, sourceState);
      return;
    }
    targets_.push_back({target, targetState});
    sources_.push_back({source, sourceState});
    copies_.push_back({target, source});
  }

  void Flush() {
    if (copies_.empty()) return;
    D3D12_RESOURCE_BARRIER barriers[kMaxBatch * 2]{};
    UINT count = 0;
    const auto push = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
      if (from == to) return;
      auto& barrier = barriers[count++];
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = resource;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = from;
      barrier.Transition.StateAfter = to;
    };
    for (const auto& entry : targets_) push(entry.resource, entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& entry : sources_) push(entry.resource, entry.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(count, barriers);
    for (const auto& copy : copies_) list_->CopyResource(copy.target, copy.source);
    count = 0;
    for (const auto& entry : sources_) push(entry.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, entry.state);
    for (const auto& entry : targets_) push(entry.resource, D3D12_RESOURCE_STATE_COPY_DEST, entry.state);
    list_->ResourceBarrier(count, barriers);
    targets_.clear();
    sources_.clear();
    copies_.clear();
  }

 private:
  static constexpr size_t kMaxBatch = 8;
  struct Entry { ID3D12Resource* resource; D3D12_RESOURCE_STATES state; };
  struct Pair { ID3D12Resource* target; ID3D12Resource* source; };
  bool Duplicated(ID3D12Resource* resource) const {
    for (const auto& entry : targets_) if (entry.resource == resource) return true;
    for (const auto& entry : sources_) if (entry.resource == resource) return true;
    return false;
  }
  ID3D12GraphicsCommandList* list_;
  std::vector<Entry> targets_, sources_;
  std::vector<Pair> copies_;
};
// The one rule an output resource must satisfy before XeFG may dispatch into it: this module created
// it, it started life in UNORDERED_ACCESS, and it carries the UAV flag. Nothing that DLSS-G hands over
// can qualify -- its current state is not knowable from here, and a barrier written on a guess is a
// device removal. That is why the frames are generated into textures of ours and copied back, and why
// writing straight into the game's buffer was removed rather than fixed.
constexpr bool XefgOutputEligible(D3D12_RESOURCE_STATES created_state, D3D12_RESOURCE_FLAGS flags) {
  return created_state == kOutputState && (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
}
// Every output texture is created exactly this way; changing either argument breaks the build rather
// than producing frames the GPU cannot see.
static_assert(XefgOutputEligible(kOutputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              "an output texture must start in UNORDERED_ACCESS and carry the UAV flag");

inline ID3D12Resource* Resource(const NVSDK_NGX_Parameter* params, const char* name) {
  ID3D12Resource* value = nullptr;
  return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : nullptr;
}
template <class T>
inline T Value(const NVSDK_NGX_Parameter* params, const char* name, T fallback) {
  T value{};
  return params->Get(name, &value) == NVSDK_NGX_Result_Success ? value : fallback;
}
// Each copy has a shape to follow; the old one is released when it changes. The copy is recorded
// into `batch`, which the caller flushes once for the frame.
inline bool Capture(ID3D12Device* device, CopyBatch& batch, ComPtr<ID3D12Resource>& copy,
                    ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState) {
  if (source == nullptr) {
    copy.Reset();
    return true;
  }
  const auto desc = source->GetDesc();
  // One GetDesc on the copy, not three: it is a virtual call into the driver, and at 6x this runs
  // four times per displayed frame.
  if (copy) {
    const auto existing = copy->GetDesc();
    if (existing.Width == desc.Width && existing.Height == desc.Height && existing.Format == desc.Format) {
      batch.Add(copy.Get(), kXefgInputState, source, sourceState);
      return true;
    }
  }
  copy = Texture(device, desc, kXefgInputState, D3D12_RESOURCE_FLAG_NONE);
  if (!copy) return false;
  batch.Add(copy.Get(), kXefgInputState, source, sourceState);
  return true;
}
inline Input Describe(ID3D12Resource* copy) {
  Input input{};
  input.resource = copy;
  if (copy != nullptr) {
    const auto desc = copy->GetDesc();
    input.width = static_cast<uint32_t>(desc.Width);
    input.height = desc.Height;
  }
  return input;
}
inline bool Prepare(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params, const D3D12_RESOURCE_DESC& output) {
  auto& state = Get();
  uint32_t flags = 0;
  if (Value<int>(params, "DLSSG.DepthInverted", 0) != 0) flags |= 1u << 0;
  if (Value<int>(params, "DLSSG.MvecJittered", 0) != 0) flags |= 1u << 4;
  const auto width = static_cast<uint32_t>(output.Width);
  if (state.context != nullptr && state.width == width && state.height == output.Height &&
      state.format == output.Format && state.flags == flags)
    return true;
  if (state.context != nullptr) {
    const auto retire = [&](ComPtr<ID3D12Resource>& resource) {
      if (resource) state.retired.push_back(std::move(resource));
      resource.Reset();
    };
    for (auto& frame : state.frames) {
      retire(frame.backbuffer);
      retire(frame.depth);
      retire(frame.motion);
      retire(frame.hudless);
      frame = {};
    }
    for (auto& texture : state.outputs) retire(texture);
    state.context = nullptr;  // its commands may be in flight, so it stays alive
    state.device.Reset();
  }
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&state.device)))) return false;
  if (!LoadContext(state.device.Get())) return false;
  InitParams init{};
  init.width = width;
  init.height = output.Height;
  init.format = output.Format;
  init.flags = flags;
  init.maxFrames = state.maxFrames;
  init.creationMask = 1;
  init.visibleMask = 1;
  const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kInitRva, state.context, &init, nullptr);
  Log("init " + std::to_string(width) + "x" + std::to_string(output.Height) + " format " +
      std::to_string(static_cast<int>(output.Format)) + " flags " + std::to_string(flags) + " result " + std::to_string(result));
  if (result != 0) {
    Fail("XeFG initialization returned " + std::to_string(result));
    return false;
  }
  state.outputDesc = output;
  state.width = width;
  state.height = output.Height;
  state.format = output.Format;
  state.flags = flags;
  return true;
}
// True when this call produced what NGX expects.
inline bool Generate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) {
  auto& state = Get();
  if (state.failed) return false;
  auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
  auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
  auto* real = Resource(params, "DLSSG.OutputReal");
  auto* depth = Resource(params, "DLSSG.Depth");
  auto* motion = Resource(params, "DLSSG.MVecs");
  if (backbuffer == nullptr || interpolated == nullptr || depth == nullptr || motion == nullptr) return false;
  const auto frameCount = (std::max)(Value<unsigned int>(params, "DLSSG.MultiFrameCount", 1), 1u);
  const auto frameIndex = std::clamp(Value<unsigned int>(params, "DLSSG.MultiFrameIndex", 1), 1u, frameCount);
  if (!Prepare(list, params, interpolated->GetDesc())) {
    state.failed = true;
    return false;
  }
  if (frameCount > state.maxFrames) {
    Fail(std::to_string(frameCount) + " generated frames, this context has " + std::to_string(state.maxFrames));
    state.failed = true;
    return false;
  }
  // One batch for every copy this evaluate makes. It is flushed before Execute, which reads these
  // inputs; the output copy below is recorded after that and goes out with the batch's own flush.
  CopyBatch batch(list);
  // The real frame's output only changes once per pair: frameIndex 1 is the pair's first evaluate, and
  // the four that follow (5x/6x) would copy the same backbuffer into it four more times. Copying it
  // on the pair's first evaluate only is what "the real frame is captured once per pair" already
  // means for the inputs below.
  if (real != nullptr && frameIndex == 1) batch.Add(real, kOutputState, backbuffer, kBackbufferState);
  auto* device = state.device.Get();
  // The real frame is captured once per pair; each generated frame only reads it.
  if (frameIndex == 1) {
  state.current ^= 1;
  auto& next = state.frames[state.current];
  if (!Capture(device, batch, next.backbuffer, backbuffer, kBackbufferState) ||
      !Capture(device, batch, next.depth, depth, kInputState) || !Capture(device, batch, next.motion, motion, kInputState) ||
      !Capture(device, batch, next.hudless, Resource(params, "DLSSG.HUDLess"), kInputState)) {
    Fail("input copies");
    state.failed = true;
    return false;
  }
  auto& data = next.data;
  data = {};
  data.backbuffer = Describe(next.backbuffer.Get());
  data.depth = Describe(next.depth.Get());
  data.motion = Describe(next.motion.Get());
  data.hudless = Describe(next.hudless.Get());
  void* projection = nullptr;
  if (params->Get("DLSSG.CameraViewToClip", &projection) == NVSDK_NGX_Result_Success && projection != nullptr)
    std::memcpy(data.projection, projection, sizeof(data.projection));
  data.resolution[0] = static_cast<float>(state.width);
  data.resolution[1] = static_cast<float>(state.height);
  data.jitter[0] = Value<float>(params, "DLSSG.JitterOffsetX", 0.f);
  data.jitter[1] = Value<float>(params, "DLSSG.JitterOffsetY", 0.f);
  data.motionScale[0] = Value<float>(params, "DLSSG.MvecScaleX", 1.f);
  data.motionScale[1] = Value<float>(params, "DLSSG.MvecScaleY", 1.f);
  const bool reset = Value<unsigned int>(params, "DLSSG.Reset", 0) != 0;
  data.reset = reset ? 1 : 0;
  next.valid = true;
  }
  auto& next = state.frames[state.current];
  auto& previous = state.frames[state.current ^ 1];
  // Without a pair the real frame is shown again.
  bool generated = false;
  if (previous.valid && next.data.reset == 0 && (next.hudless != nullptr) == (previous.hudless != nullptr)) {
    ExecuteParams execute{};
    execute.previous = previous.data;
    execute.next = next.data;
    ID3D12Resource* outputs[kMaxFrames]{};
    // Every slot is a texture of ours, created in UNORDERED_ACCESS: XeFG dispatches into it by UAV,
    // and a buffer NGX owns cannot be given that state from here (its current state is not knowable,
    // and a wrong barrier is a device removal). Writing straight into DLSS-G's own buffer was tried
    // and reverted: the frames arrived with the buffer in the wrong state, so the picture stuttered
    // while the counters looked healthy.
    for (uint32_t i = 0; i < frameCount; ++i) {
      if (!state.outputs[i])
        state.outputs[i] = Texture(device, state.outputDesc, kOutputState, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      if (!state.outputs[i]) {
        Fail("output texture");
        state.failed = true;
        return false;
      }
      outputs[i] = state.outputs[i].Get();
    }
    const uint32_t bases[kMaxFrames * 2]{};
    execute.outputs = outputs;
    execute.outputBases = bases;
    execute.outputCount = static_cast<uint8_t>(frameCount);
    execute.firstFrame = frameIndex - 1;
    execute.frameCount = 1;
    // Execute reads the captured inputs: they must be barriered and copied before it records.
    batch.Flush();

    const auto result = Guarded(reinterpret_cast<unsigned char*>(state.fg) + kExecuteRva, state.context, list, &execute);
    if (result < 0) {
      // -4 is XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT. A scene change or a mode switch refuses a
      // single evaluate and the next one succeeds, so logging the transition in and out printed two
      // lines per hiccup -- hundreds a session, no signal. The count is cumulative and reported on a
      // slow cadence; the panel carries the running total.
      if (result == -4) {
        const uint32_t refused = g_refused.fetch_add(1, std::memory_order_relaxed) + 1u;
        if ((refused % 5000u) == 0u) {
          Log("execute refused its arguments (-4) " + std::to_string(refused) + " time(s) so far, last at " +
              std::to_string(execute.outputCount) + " output(s), " + std::to_string(state.width) + "x" +
              std::to_string(state.height));
        }
      } else {
        Log("execute failed: " + std::to_string(result));
      }
      if (result == -100000) state.failed = true;
    } else {
      generated = result == 0;
      if (generated) {
        g_last_generated_tick.store(GetTickCount64(), std::memory_order_relaxed);
        // The new context has produced a frame, so the retired resources' submissions are behind a
        // completed one: release them here instead of holding them for the process's life.
        if (!state.retired.empty()) {
          Log("released " + std::to_string(state.retired.size()) + " retired resource(s)");
          state.retired.clear();
        }
      }
    }
  }
  // The real-frame copy recorded above (if any) is still pending; add the output and let the
  // batch's destructor flush them together as one barrier pair.
  const D3D12_RESOURCE_STATES sourceState = generated ? kOutputState : kBackbufferState;
  ID3D12Resource* const source = generated ? state.outputs[frameIndex - 1].Get() : backbuffer;
  batch.Add(interpolated, kOutputState, source, sourceState);
  return true;
}
// Repeats the real frame into every output NGX asked for: what a disabled mode and a failed
// generation both hand back. Less smooth, never black.
inline bool RepeatRealFrame(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) {
  auto* backbuffer = Resource(params, "DLSSG.Backbuffer");
  auto* interpolated = Resource(params, "DLSSG.OutputInterpolated");
  if (backbuffer == nullptr || interpolated == nullptr) return false;
  if (auto* real = Resource(params, "DLSSG.OutputReal")) Copy(list, real, kOutputState, backbuffer, kBackbufferState);
  Copy(list, interpolated, kOutputState, backbuffer, kBackbufferState);
  return true;
}
// A generation that fails repeats the real frame rather than handing the evaluate back: the frame
// is already half-copied by then, so the core must not also write these buffers (hook.cpp's caller
// treats true as "the outputs are mine"). Less smooth, never black.
inline bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* params) {
  if (list == nullptr || params == nullptr) return false;
  if (Generate(list, params)) return true;
  return RepeatRealFrame(list, params);
}
}  // namespace uplift::xmx::generator
