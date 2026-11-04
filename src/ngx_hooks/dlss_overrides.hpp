#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>

#include "nr/ngx.hpp"
#include <nvsdk_ngx_defs_dlssd.h>  // the Ray Reconstruction preset keys (RR_PRESET_KEYS below)

namespace uplift::ngx_hooks {

// One value a create hook changed on the game's parameter block, with the game's own value to put back (0
// when the game never set it: NGX's default for every key Uplift overrides).
struct SavedParameter {
  enum class Type : uint8_t {
    INT,
    UINT,
  };
  const char* key = nullptr;
  Type type = Type::INT;
  int64_t value = 0;
};

// Writes the saved values back, last change first, with the type each was read with.
void RestoreParameters(NVSDK_NGX_Parameter* parameters, std::span<const SavedParameter> saved);

// The DLSS-SR create overrides (spec §10, the hidden DLSSQualityMode / DLSSAutoExposure rows) and the
// preset swap's render presets (addon/dlss_swap.hpp). The key mapping follows the NGX SDK header.
struct SrOverrides {
  int perf_quality = -1;                     // an NVSDK_NGX_PerfQuality_Value to force; -1 keeps the game's
  std::array<unsigned int, 6> presets = {};  // per PRESET_KEYS entry: a render preset to force; 0 keeps the game's
  std::optional<bool> auto_exposure;         // force NVSDK_NGX_DLSS_Feature_Flags_AutoExposure on or off

  [[nodiscard]] bool Any() const {
    return perf_quality >= 0 || auto_exposure.has_value()
           || std::ranges::any_of(presets, [](unsigned int preset) { return preset != 0u; });
  }
};

// Applies `overrides` to a SuperSampling create's parameter block. The game's value of every key it changes
// goes into `saved` first, so the create detour can put it back on every path.
void ApplySrOverrides(NVSDK_NGX_Parameter* parameters, const SrOverrides& overrides, std::vector<SavedParameter>* saved);

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

}  // namespace uplift::ngx_hooks
