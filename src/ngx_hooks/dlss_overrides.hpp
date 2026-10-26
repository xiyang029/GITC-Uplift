#pragma once

#include <array>
#include <optional>
#include <vector>

#include "ngx_hooks/hook_installer.hpp"
#include "nr/ngx.hpp"
#include <nvsdk_ngx_defs_dlssd.h>  // the Ray Reconstruction preset keys (RR_PRESET_KEYS below)

namespace uplift::ngx_hooks {

// The DLSS-SR page's create overrides (spec §10). The key mapping follows the NGX SDK header.
struct SrOverrides {
  int perf_quality = -1;                     // an NVSDK_NGX_PerfQuality_Value to force; -1 keeps the game's
  std::array<unsigned int, 6> presets = {};  // per PRESET_KEYS entry: a render preset to force; 0 keeps the game's
  std::optional<bool> auto_exposure;         // force NVSDK_NGX_DLSS_Feature_Flags_AutoExposure on or off

  [[nodiscard]] bool Any() const;
};

// DLSS.Hint.Render.Preset.<Mode>, in SrOverrides::presets and ui::DlssPresetMode order.
inline constexpr std::array<const char*, 6> PRESET_KEYS = {
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
};

// The PRESET_KEYS slot a feature runs in: the create's PerfQualityValue picks the one
// DLSS.Hint.Render.Preset.<Mode> key the runtime reads (MaxPerf 0, Balanced 1, MaxQuality 2,
// UltraPerformance 3, UltraQuality 4, DLAA 5 -- the keys list the modes in UI order). 6 when
// `perf_quality` names no mode.
inline constexpr unsigned PresetSlotOf(int perf_quality) {
  switch (perf_quality) {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return 4u;
    case NVSDK_NGX_PerfQuality_Value_Balanced: return 3u;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality: return 2u;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return 5u;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality: return 1u;
    case NVSDK_NGX_PerfQuality_Value_DLAA: return 0u;
    default: return 6u;
  }
}

// Applies `overrides` to a SuperSampling create's parameter block. The game's value of every key it
// changes goes into `saved` first, so the create detour can put it back (Task 1). The create helper
// writes PerfQualityValue and the create flags as int; games write presets as unsigned int.
// The Ray Reconstruction preset keys, in ui::DlssPresetMode order (the swap writes the RR table for a
// Ray Reconstruction feature, the DLSS table for a SuperSampling one).
inline constexpr std::array<const char*, 6> RR_PRESET_KEYS = {
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
};

void ApplySrOverrides(NVSDK_NGX_Parameter* parameters, const SrOverrides& overrides, std::vector<SavedParameter>* saved);

}  // namespace uplift::ngx_hooks
