#pragma once

#include <chrono>
#include <cstdint>

namespace uplift::addon::framerate {

// What the panel shows for a frame-rate row, in milli-FPS; 0 means "not measured".
//
// Under frame generation the presents ReShade sees ARE the output frames: the provider hands every
// generated frame over too, so a 4x game running 160 real fps presents ~650 times a second. The present
// count is therefore the OUTPUT rate, and the real rate is that divided by the frames presented per real
// frame -- the provider's counted batch when a game reports it, else the panel's multiplier.
struct Snapshot {
  uint32_t real_milli = 0u;    // application frames per second (what the game itself renders)
  uint32_t output_milli = 0u;  // frames per second actually presented, generated ones included
  uint64_t age_ms = 0u;        // time since the window that produced them closed
  bool output_estimated = false;  // real_milli came from the multiplier, not from a counted batch
};

// One present of the primary swapchain, on the present thread: closes the window of the previous frame
// (counting that frame's DLSS-G batch into it) and starts the next.
void OnPresent(std::chrono::steady_clock::time_point now);

// DLSS-G's numFramesActuallyPresented for the frame being presented: one real frame plus the generated
// ones. The provider reports a batch, not a running total, so it is attributed to the present it belongs
// to and summed once per real frame (several GetState calls in one frame: the last wins).
void OnPresentedBatch(uint32_t batch);

// The frames presented per real frame when the provider's batch is unavailable (an NGX
// frame-generation feature, or a game without Streamline). 1 = frame generation off.
void SetMultiplier(uint32_t multiplier);

// The last closed window. Cheap: a lock, four reads.
[[nodiscard]] Snapshot Read();

}  // namespace uplift::addon::framerate
