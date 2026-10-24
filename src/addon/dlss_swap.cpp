#include "addon/dlss_swap.hpp"

#include <array>
#include <format>
#include <utility>

#include "addon/ngx_bridge.hpp"
#include "ngx_hooks/hook_installer.hpp"
#include "nr/log.hpp"

namespace uplift::addon {
namespace {

// How long a swap feature stays unreleased after it stopped being evaluated (frames), and how long a
// failed create waits before it is tried again (frames; ~10 s at 60 fps). The create costs a real hitch
// (DLSS loads its network), so a core that refuses the preset keys must not be probed every frame.
constexpr uint64_t RETIRE_FRAMES = 3u;
constexpr uint64_t RETRY_FRAMES = 600u;

// The core's unhooked parameter allocator per API, resolved on the copy that received the hooked call.
using AllocateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using DestroyFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
constexpr const char* AllocateName(ngx_hooks::NgxApi api) {
  return (api == ngx_hooks::NgxApi::D3D12 ? "NVSDK_NGX_D3D12_AllocateParameters"
          : api == ngx_hooks::NgxApi::D3D11 ? "NVSDK_NGX_D3D11_AllocateParameters"
                                            : "NVSDK_NGX_VULKAN_AllocateParameters");
}
constexpr const char* DestroyName(ngx_hooks::NgxApi api) {
  return (api == ngx_hooks::NgxApi::D3D12 ? "NVSDK_NGX_D3D12_DestroyParameters"
          : api == ngx_hooks::NgxApi::D3D11 ? "NVSDK_NGX_D3D11_DestroyParameters"
                                            : "NVSDK_NGX_VULKAN_DestroyParameters");
}

// One "logged once" latch, module-wide: a core that cannot serve the swap says so once, whatever the
// games, features and frames after it.
void WarnOnce(const char* what) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) return;
  nr::Logf(nr::LogLevel::WARN, "the DLSS preset swap cannot run: {}", what);
}

// The swap feature's create-parameters block, freed when the create returns (NGX copies what it needs
// into the feature; the block holds nothing the evaluate ever reads).
class ScopedParameters {
 public:
  ScopedParameters(ngx_hooks::NgxApi api, HMODULE core) {
    if (core == nullptr) {
      WarnOnce("no NGX core module is known for this call");
      return;
    }
    const auto allocate = reinterpret_cast<AllocateFn>(GetProcAddress(core, AllocateName(api)));
    const auto destroy = reinterpret_cast<DestroyFn>(GetProcAddress(core, DestroyName(api)));
    if (allocate == nullptr || destroy == nullptr) {
      WarnOnce("the NGX core does not export its parameters allocator");
      return;
    }
    if (NVSDK_NGX_FAILED(allocate(&parameters_)) || parameters_ == nullptr) {
      WarnOnce("the NGX core could not allocate a create-parameters block");
      return;
    }
    destroy_ = destroy;
  }
  ~ScopedParameters() {
    if (destroy_ != nullptr) destroy_(parameters_);  // best effort: a refused free leaks one block, once
  }
  ScopedParameters(const ScopedParameters&) = delete;
  ScopedParameters& operator=(const ScopedParameters&) = delete;

  [[nodiscard]] NVSDK_NGX_Parameter* Get() const { return parameters_; }

 private:
  NVSDK_NGX_Parameter* parameters_ = nullptr;
  DestroyFn destroy_ = nullptr;
};

}  // namespace

NVSDK_NGX_Handle* DlssSwap::CreateSwap(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list,
                                       const ngx_hooks::FeatureRecord& record, const ngx_hooks::SrOverrides& desired) {
  const ScopedParameters parameters(api, ngx_hooks::CurrentNgxCore());
  NVSDK_NGX_Parameter* const block = parameters.Get();
  if (block == nullptr) return nullptr;
  // The game feature's create snapshot: a feature must match what it evaluates (the render and output
  // sizes it was sized for, its flags -- HDR, low-res vectors, jitter, auto-exposure -- and its quality
  // mode), or the evaluate of the game's own block is refused.
  const ngx_hooks::CreateSnapshot& snapshot = record.snapshot;
  block->Set(NVSDK_NGX_Parameter_Width, snapshot.render.width);
  block->Set(NVSDK_NGX_Parameter_Height, snapshot.render.height);
  block->Set(NVSDK_NGX_Parameter_OutWidth, snapshot.output.width);
  block->Set(NVSDK_NGX_Parameter_OutHeight, snapshot.output.height);
  if (snapshot.perf_quality >= 0) {
    block->Set(NVSDK_NGX_Parameter_PerfQualityValue, snapshot.perf_quality);
  }
  block->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, snapshot.flags);
  // The panel's choices: one preset key per DLSS mode that is not Game (a J-M render preset, 10-13).
  for (size_t mode = 0u; mode < ngx_hooks::PRESET_KEYS.size(); ++mode) {
    if (desired.presets[mode] != 0u) {
      block->Set(ngx_hooks::PRESET_KEYS[mode], desired.presets[mode]);
    }
  }
  NVSDK_NGX_Handle* handle = nullptr;
  if (NVSDK_NGX_FAILED(ngx_hooks::OwnCreate(api, list, record.feature, block, &handle)) || handle == nullptr) {
    WarnOnce("the NGX core refused the swap feature's create (an older DLSS runtime has no J-M presets, or the core is shutting down)");
    return nullptr;
  }
  nr::Logf(nr::LogLevel::INFO, "the DLSS preset swap created a feature ({}x{} -> {}x{}, flags {:x}, presets {} {} {} {} {} {})",
           snapshot.render.width, snapshot.render.height, snapshot.output.width, snapshot.output.height, snapshot.flags, desired.presets[0],
           desired.presets[1], desired.presets[2], desired.presets[3], desired.presets[4], desired.presets[5]);
  return handle;
}

void DlssSwap::ReleaseSwap(ngx_hooks::NgxApi api, NVSDK_NGX_Handle* handle) {
  if (handle == nullptr) return;
  if (NVSDK_NGX_FAILED(ngx_hooks::OwnRelease(api, handle))) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      nr::Logf(nr::LogLevel::WARN, "the NGX core refused the swap feature's release; it stays to the core's shutdown");
    }
  }
}

void DlssSwap::RetireDue() {
  std::erase_if(retired_, [this](const Retired& retired) {
    if (frames_ < retired.due) return false;
    ReleaseSwap(retired.api, retired.handle);
    return true;
  });
}

const NVSDK_NGX_Handle* DlssSwap::BeforeEvaluate(ngx_hooks::NgxApi api, const void* device, ID3D12GraphicsCommandList* list,
                                                 const NVSDK_NGX_Handle* game_handle, const ngx_hooks::FeatureRecord& record) {
  ++frames_;
  // The add-on's lock first, alone: the settings read must never run under this module's leaf lock.
  const ngx_hooks::SrOverrides desired = routing_.DlssPresetOverrides();
  const std::unique_lock lock(mutex_);
  RetireDue();
  SwapState& state = states_[game_handle];
  state.api = api;
  state.device = device;
  if (!desired.Any()) {
    if (state.handle != nullptr) {
      ReleaseSwap(api, state.handle);
      state.handle = nullptr;
    }
    if (retired_.empty()) {
      states_.erase(game_handle);  // the panel returned to Game and the swap is gone: forget the feature
    }
    RefreshActive();
    return game_handle;
  }
  if (state.handle != nullptr && state.applied.presets == desired.presets) {
    RefreshActive();
    return state.handle;
  }
  if (state.failed && frames_ < state.retry_at) {
    RefreshActive();
    return (state.handle != nullptr ? state.handle : game_handle);  // an old-preset swap beats no swap
  }
  // A fresh create, or a recreate for changed choices: the new feature is created before the old one is
  // retired, so the panel's change never costs a frame without a swap.
  if (NVSDK_NGX_Handle* const created = CreateSwap(api, list, record, desired); created != nullptr) {
    if (state.handle != nullptr) {
      retired_.push_back({.handle = state.handle, .api = api, .due = frames_ + RETIRE_FRAMES});
    }
    state.handle = created;
    state.applied = desired;
    state.failed = false;
  } else {
    state.failed = true;
    state.retry_at = frames_ + RETRY_FRAMES;
  }
  RefreshActive();
  return (state.handle != nullptr ? state.handle : game_handle);
}

void DlssSwap::BeforeRelease(const NVSDK_NGX_Handle* game_handle) {
  const std::unique_lock lock(mutex_);
  const auto found = states_.find(game_handle);
  if (found == states_.end()) return;
  ReleaseSwap(found->second.api, found->second.handle);
  states_.erase(found);
  RefreshActive();
}

void DlssSwap::OnCoreShutdown(ngx_hooks::NgxApi api, const void* device) {
  const std::unique_lock lock(mutex_);
  std::erase_if(states_, [api, device](const auto& entry) {
    const SwapState& state = entry.second;
    return state.api == api && (device == nullptr || state.device == device);
  });
  // The retirees died with the core too: releasing them through a re-initialised core would name
  // handles it never handed out.
  std::erase_if(retired_, [api](const Retired& retired) { return retired.api == api; });
  RefreshActive();
}

}  // namespace uplift::addon
