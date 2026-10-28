#pragma once

// The Reflex override (addon/reflex_override.{hpp,cpp}).
//
// The panel's Reflex mode and output-FPS cap, applied by rewriting the game's
// NvAPI_D3D_SetSleepMode calls in transit (the implementation nvapi_QueryInterface resolves for
// that id, like the FG preset hook reaches NvAPI_DRS_GetSetting's). Native Reflex games and
// Streamline's sl.reflex both funnel into this call, so one hook covers both.
// The rewrite is fail-safe: an unknown params version or a rejected call falls back to
// the game's own request, and a frame limit is only ever tightened, never relaxed.

#include <cstdint>

namespace uplift::addon::reflex {

enum class ReflexModeOverride : uint32_t {
  GAME = 0u,  // the game's/driver's own mode
  ON = 1u,
  BOOST = 2u,
  OFF = 3u,
};

// The panel's current choice; applies on the game's next SetSleepMode call.
void SetDesired(ReflexModeOverride mode, uint32_t fps_cap);

// Installs the hook on a one-shot thread (nvapi_query.hpp loads nvapi64.dll on demand). Safe to
// call from any poll; a no-op once installed or failed for good.
void EnsureReflexHook();

// Submits a pending panel change through the last observed (device, params), so the mode and cap
// land without waiting for the game's own next call. Called from the add-on's Present poll.
void RefreshFromPanel();

// Diagnostics: samples NvAPI_D3D_GetSleepStatus at most once every two seconds -- the interval
// the driver holds, whether the game actually calls the Reflex sleep, and the live FG multiplier.
void LogSleepStatusThrottled();

}  // namespace uplift::addon::reflex
