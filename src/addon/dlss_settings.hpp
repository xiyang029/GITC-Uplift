#pragma once

#include "ngx_hooks/dlss_overrides.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// The panel's DLSSPreset<Mode> choices as the preset swap reads them (addon/dlss_swap.hpp): one J-M
// render preset value (10-13) per DLSS mode, 0 where the choice is Game.
[[nodiscard]] ngx_hooks::SrOverrides ToPresetOverrides(const ui::Settings& settings);
// The panel's RayReconstructionPreset<Mode> choices, same shape (render preset values, 0 = Game).
[[nodiscard]] ngx_hooks::SrOverrides ToRrPresetOverrides(const ui::Settings& settings);
// The hidden DLSS-SR create overrides (DLSSQualityMode / DLSSAutoExposure): the quality mode the game's next
// DLSS feature is created with and its auto-exposure flag. A value of Game leaves that key to the game, so an
// all-Game pair returns an override whose Any() is false.
[[nodiscard]] ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& settings);

}  // namespace uplift::addon
