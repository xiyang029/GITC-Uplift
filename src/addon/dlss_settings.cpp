#include "addon/dlss_settings.hpp"

#include "ui/settings.hpp"

namespace uplift::addon {

// DlssPresetOverride runs GAME=0, then J..M = 1..4; a DLSS render preset is 9 above that (J is 10).
ngx_hooks::SrOverrides ToPresetOverrides(const ui::Settings& settings) {
  ngx_hooks::SrOverrides overrides;
  for (size_t mode = 0u; mode < overrides.presets.size(); ++mode) {
    const uint32_t choice = static_cast<uint32_t>(settings.dlss_presets[mode]);
    overrides.presets[mode] = (choice == 0u ? 0u : 9u + choice);
  }
  return overrides;
}

// RrPresetOverride runs GAME=0, then D..F = 1..3; an RR render preset is 3 above that (D is 4).
ngx_hooks::SrOverrides ToRrPresetOverrides(const ui::Settings& settings) {
  ngx_hooks::SrOverrides overrides;
  const uint32_t choice = static_cast<uint32_t>(settings.rr_preset);
  const uint32_t value = (choice == 0u ? 0u : 3u + choice);  // Game, or D/E/F (4/5/6)
  overrides.presets.fill(value);  // one choice for every mode: RR's default does not vary by mode
  return overrides;
}

// The hidden DLSSQualityMode row: the stored index is NVSDK_NGX_PerfQuality_Value + 1 (GAME = 0 keeps the
// game's own). DLSS reads PerfQualityValue once, at CreateFeature, so the override lands on the game's next
// feature create and never on a live one -- which is why the row stays hidden.
ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& settings) {
  ngx_hooks::SrOverrides overrides;
  if (const auto quality = static_cast<uint32_t>(settings.dlss_quality_mode); quality != 0u) {
    overrides.perf_quality = static_cast<int>(quality) - 1;
  }
  // The hidden DLSSAutoExposure row: Game keeps DLSS's own flag, Off/On force NVSDK_NGX_DLSS_Feature_Flags_AutoExposure.
  switch (settings.dlss_auto_exposure) {
    case ui::AutoExposureOverride::GAME: break;
    case ui::AutoExposureOverride::OFF: overrides.auto_exposure = false; break;
    case ui::AutoExposureOverride::ON: overrides.auto_exposure = true; break;
  }
  return overrides;
}

}  // namespace uplift::addon
