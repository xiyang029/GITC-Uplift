#pragma once

#include "ngx_hooks/dlss_overrides.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// The panel's DLSSPreset<Mode> choices as the preset swap reads them (addon/dlss_swap.hpp): one J-M
// render preset value (10-13) per DLSS mode, 0 where the choice is Game.
[[nodiscard]] ngx_hooks::SrOverrides ToPresetOverrides(const ui::Settings& settings);
// The panel's RayReconstructionPreset<Mode> choices, same shape (render preset values, 0 = Game).
[[nodiscard]] ngx_hooks::SrOverrides ToRrPresetOverrides(const ui::Settings& settings);
// The hidden DLSS-SR settings (DLSSQualityMode / DLSSAutoExposure) never override the game, including keys
// loaded from a hand-edited INI. NgxBridge::BeforeCreate calls this unconditionally and receives no overrides.
[[nodiscard]] ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& settings);

}  // namespace uplift::addon
