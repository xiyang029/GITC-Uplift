#include "addon/dlss_swap.hpp"

#include <array>
#include <atomic>
#include <format>
#include <string_view>
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

// Whether the two choices differ in the preset the feature actually reads: DLSS reads only its mode's
// hint, selected by the create's PerfQualityValue. A change to another mode's row lands when the game
// re-creates for that mode (a new handle, and so a fresh `applied`), not now -- and re-creating the
// swap feature for a key it will not read costs a real hitch (DLSS loads its network).
bool SameActivePresets(const ngx_hooks::SrOverrides& a, const ngx_hooks::SrOverrides& b, int perf_quality) {
  const unsigned slot = ngx_hooks::PresetSlotOf(perf_quality);
  // With no quality mode in the create there is no slot to single out: any change re-creates.
  return (slot < a.presets.size() ? a.presets[slot] == b.presets[slot] : a.presets == b.presets);
}

// One "logged once" latch, module-wide: a core that cannot serve the swap says so once, whatever the
// games, features and frames after it.
void WarnOnce(std::string_view what) {
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

NVSDK_NGX_Handle* DlssSwap::CreateSwap(ngx_hooks::NgxApi api, ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* evaluate,
                                       const ngx_hooks::FeatureRecord& record, const ngx_hooks::SrOverrides& desired) {
  const ScopedParameters parameters(api, ngx_hooks::CurrentNgxCore());
  NVSDK_NGX_Parameter* const block = parameters.Get();
  if (block == nullptr) return nullptr;
  // A Ray Reconstruction feature's create needs its guide buffers bound: copy them from the evaluate's
  // own block (they stay alive for this call, and NGX takes its own references). A SuperSampling create
  // needs none of these keys.
  if (record.feature == NVSDK_NGX_Feature_RayReconstruction && evaluate != nullptr) {
    constexpr const char* RR_CREATE_KEYS[] = {
        NVSDK_NGX_Parameter_DiffuseAlbedo,
        NVSDK_NGX_Parameter_SpecularAlbedo,
        NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y,
        NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
        NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y,
        NVSDK_NGX_Parameter_Depth,
        NVSDK_NGX_Parameter_ExposureTexture,
        NVSDK_NGX_Parameter_TransparencyMask,
        NVSDK_NGX_Parameter_GBuffer_Emissive,
    };
    auto* const source = const_cast<NVSDK_NGX_Parameter*>(evaluate);
    for (const char* key : RR_CREATE_KEYS) {
      if (void* resource = nullptr; source->Get(key, &resource) == NVSDK_NGX_Result_Success && resource != nullptr) {
        block->Set(key, resource);
      } else if (unsigned int value = 0u; source->Get(key, &value) == NVSDK_NGX_Result_Success) {
        block->Set(key, value);
      }
    }
    // The scalar create keys an RR create consumes besides the guide buffers (NGX_D3D12_CREATE_DLSSD_EXT,
    // after MFG-Unlocker's list): node masks, the output-subrects switch and the denoise/roughness/HW-depth
    // modes. The evaluate's block still carries them from the game's create (Streamline replays the same set).
    for (const char* key : {NVSDK_NGX_Parameter_CreationNodeMask, NVSDK_NGX_Parameter_VisibilityNodeMask,
                            NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, NVSDK_NGX_Parameter_DLSS_Denoise_Mode,
                            NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_Parameter_Use_HW_Depth}) {
      if (unsigned int value = 0u; source->Get(key, &value) == NVSDK_NGX_Result_Success) {
        block->Set(key, value);
      } else if (int svalue = 0; source->Get(key, &svalue) == NVSDK_NGX_Result_Success) {
        block->Set(key, svalue);
      }
    }
  }
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
  // The panel's choices: one preset key per mode that is not Game, from the feature kind's own table
  // (a SuperSampling feature reads the DLSS.* keys, a Ray Reconstruction one the RayReconstruction.*).
  const bool rr = (record.feature == NVSDK_NGX_Feature_RayReconstruction);
  const std::array<const char*, 6>& keys = (rr ? ngx_hooks::RR_PRESET_KEYS : ngx_hooks::PRESET_KEYS);
  for (size_t mode = 0u; mode < keys.size(); ++mode) {
    if (desired.presets[mode] != 0u) {
      block->Set(keys[mode], desired.presets[mode]);
    }
  }
  NVSDK_NGX_Handle* handle = nullptr;
  const NVSDK_NGX_Result created = ngx_hooks::OwnCreate(api, list, record.feature, block, &handle);
  if (NVSDK_NGX_FAILED(created) || handle == nullptr) {
    WarnOnce(std::format("the NGX core refused the swap feature's create (result {:#x}; the create keys or the runtime disagree)",
                         static_cast<unsigned>(created)).c_str());
    return nullptr;
  }
  // The first create is the evidence the swap feature exists; a runtime rebuild recreates it with the
  // same snapshot and presets, so later creates add nothing.
  static std::atomic<bool> created_logged{false};
  if (!created_logged.exchange(true, std::memory_order_relaxed)) {
    nr::Logf(nr::LogLevel::INFO, "the DLSS preset swap created a {} feature ({}x{} -> {}x{}, flags {:x}, presets {} {} {} {} {} {})",
             (rr ? "Ray Reconstruction" : "SuperSampling"), snapshot.render.width, snapshot.render.height, snapshot.output.width,
             snapshot.output.height, snapshot.flags, desired.presets[0], desired.presets[1], desired.presets[2], desired.presets[3],
             desired.presets[4], desired.presets[5]);
  }
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
                                                 const NVSDK_NGX_Parameter* evaluate, const NVSDK_NGX_Handle* game_handle,
                                                 const ngx_hooks::FeatureRecord& record) {
  ++frames_;
  // The add-on's lock first, alone: the settings read must never run under this module's leaf lock.
  // An RR feature takes the RayReconstruction.* preset keys, a SuperSampling one the DLSS.* keys.
  const ngx_hooks::SrOverrides desired = (record.feature == NVSDK_NGX_Feature_RayReconstruction
                                              ? routing_.RrPresetOverrides()
                                              : routing_.DlssPresetOverrides());
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
  if (state.handle != nullptr && SameActivePresets(state.applied, desired, record.snapshot.perf_quality)) {
    RefreshActive();
    return state.handle;
  }
  if (state.failed && frames_ < state.retry_at) {
    RefreshActive();
    return (state.handle != nullptr ? state.handle : game_handle);  // an old-preset swap beats no swap
  }
  // A fresh create, or a recreate for changed choices: the new feature is created before the old one is
  // retired, so the panel's change never costs a frame without a swap.
  if (NVSDK_NGX_Handle* const created = CreateSwap(api, list, evaluate, record, desired); created != nullptr) {
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
