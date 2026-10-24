#pragma once

#include "ngx_hooks/dlss_overrides.hpp"
#include "ui/settings.hpp"

namespace uplift::addon {

// Minor (fix round 2, user decision): the DLSS-SR page is hidden, and the user does not want its
// hidden keys honoured from a hand-edited ini either -- always no overrides, whatever `settings`
// holds. The signature stays: NgxBridge::BeforeCreate calls this unconditionally, and a later plan
// may bring the page back. The preset choices took another road (ToPresetOverrides, the live swap).
[[nodiscard]] ngx_hooks::SrOverrides ToSrOverrides(const ui::Settings& settings);

// The panel's DLSSPreset<Mode> choices as the preset swap reads them (addon/dlss_swap.hpp): one J-M
// render preset value (10-13) per DLSS mode, 0 where the choice is Game. Only the preset keys -- the
// quality mode and auto-exposure overrides stay inert (see ToSrOverrides).
[[nodiscard]] ngx_hooks::SrOverrides ToPresetOverrides(const ui::Settings& settings);

}  // namespace uplift::addon
