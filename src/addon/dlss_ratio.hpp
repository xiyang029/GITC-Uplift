#pragma once

#include <array>

#include "ngx_hooks/ngx_api.hpp"
#include "nr/ngx.hpp"

namespace uplift::addon {

// The DLSS render-scale override (2026-10-25): the per-mode render ratio a game's DLSS uses, e.g. Ultra
// Performance at 40 % instead of 33 %.3. A DLSS feature renders what the game renders -- a mod cannot
// shrink the game's cost -- so the lever is the game's own size decision: the SDK's OptimalSettings
// helper reads a callback pointer out of the core's DLSS capability block and asks it for the
// recommended render size. Planting Uplift's callback there (GetCapabilityParameters, hooked for this)
// answers every later query with the panel's ratios; the game then sizes its render targets and
// re-creates its own feature, so no feature is ever owned or released here. Takes effect at the game's
// next OptimalSettings query (a quality-mode flip, a resolution change, a restart) -- games decide when
// to ask, the same limit OptiScaler's ratio override lives with.
//
// The planted callback runs on a game thread with none of Uplift's locks held: the scales are atomics,
// the per-block original-callback map is a leaf lock.

// The panel's per-mode render scales, percent of the output size (100 = the game's choice), indexed by
// ui::DlssPresetMode. Pushed by the add-on at init and on every SaveAndApply.
void SetDlssRenderScales(const std::array<float, 6>& scales_percent);

// Plants the override's callback into `block` (the core's cached DLSS capability block, seen through
// the hooked GetCapabilityParameters). Idempotent per block; keeps the snippet's own callback for
// rescaling its answers.
void PlantDlssRatio(ngx_hooks::NgxApi api, NVSDK_NGX_Parameter* block);

}  // namespace uplift::addon
