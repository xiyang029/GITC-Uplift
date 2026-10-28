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

}  // namespace uplift::addon
