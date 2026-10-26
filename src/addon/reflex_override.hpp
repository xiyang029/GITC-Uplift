#pragma once

// The Reflex override (addon/reflex_override.{hpp,cpp}).
//
// The panel's Reflex mode and output-FPS cap, applied by rewriting the game's
// NvAPI_D3D_SetSleepMode calls in transit (nvapi_QueryInterface 0xAC1CA9E0's internal
// implementation, like the FG preset hook reaches NvAPI_DRS_GetSetting's). Native Reflex
// games and Streamline's sl.reflex both funnel into this call, so one hook covers both.
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

// The two advanced SetSleepMode bits the panel can force on (NV_SET_SLEEP_MODE_PARAMS'
// bUseMarkersToOptimize / bUseMinQueueTime). Either one raises the rewrite's own condition, so a
// ticked box is submitted even with Game mode and no cap; an unticked box leaves the game's value.
void SetAdvanced(bool use_markers, bool use_min_queue_time);

}  // namespace uplift::addon::reflex
