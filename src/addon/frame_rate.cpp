#include "addon/frame_rate.hpp"

#include <atomic>
#include <mutex>

namespace uplift::addon::framerate {
namespace {

// The window: long enough that a 30 fps game still gets a sane count, short enough that a panel row
// follows a scene change without a visible lag.
constexpr auto WINDOW = std::chrono::milliseconds(500);

std::mutex g_mutex;  // guards everything below except g_multiplier
std::chrono::steady_clock::time_point g_window_start;
std::chrono::steady_clock::time_point g_window_closed;
uint64_t g_window_presents = 0u;
uint64_t g_window_presented_frames = 0u;  // summed DLSS-G batches inside the window
uint64_t g_window_batches = 0u;           // presents that carried a batch
uint32_t g_batch = 0u;                    // this frame's numFramesActuallyPresented; 0 = none read
bool g_window_open = false;
Snapshot g_snapshot;
std::atomic<uint32_t> g_multiplier{1u};

uint32_t Milli(uint64_t frames, std::chrono::steady_clock::duration elapsed) {
  const double ticks = std::chrono::duration<double>(elapsed).count();
  if (frames == 0u || ticks <= 0.0) return 0u;
  const double value = static_cast<double>(frames) * 1000.0 / ticks;
  return value >= static_cast<double>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(value + 0.5);
}

}  // namespace

void OnPresent(std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(g_mutex);
  // The batch read during the frame that just presented belongs to this present.
  if (g_batch != 0u) {
    g_window_presented_frames += g_batch;
    ++g_window_batches;
    g_batch = 0u;
  }
  ++g_window_presents;
  if (!g_window_open) {
    g_window_open = true;
    g_window_start = now;
    return;
  }
  const auto elapsed = now - g_window_start;
  if (elapsed < WINDOW) return;
  // The presents ARE the output frames (frame generation hands every generated frame over as its own
  // present), so the present count measures the output rate directly. The real rate is that divided by
  // the frames presented per real frame: the provider's counted batch when it reports one, else the
  // panel's multiplier. A counted batch needs a sample on nearly every present (one missing is the last
  // frame, committed by the next present); anything less is a window a GetState did not cover.
  const bool counted = (g_window_batches != 0u && g_window_batches + 1u >= g_window_presents);
  const uint32_t output = Milli(g_window_presents, elapsed);
  const uint32_t per_real = counted
                                ? static_cast<uint32_t>((g_window_presented_frames + g_window_batches / 2u) / g_window_batches)
                                : g_multiplier.load(std::memory_order_relaxed);
  g_snapshot.output_milli = output;
  g_snapshot.output_estimated = !counted;
  g_snapshot.real_milli = (per_real >= 2u && output != 0u) ? (output + per_real / 2u) / per_real : output;
  g_window_closed = now;
  g_window_start = now;
  g_window_presents = 0u;
  g_window_presented_frames = 0u;
  g_window_batches = 0u;
}

void OnPresentedBatch(uint32_t batch) {
  std::lock_guard lock(g_mutex);
  g_batch = batch;
}

void SetMultiplier(uint32_t multiplier) {
  g_multiplier.store(multiplier >= 1u ? multiplier : 1u, std::memory_order_relaxed);
}

Snapshot Read() {
  std::lock_guard lock(g_mutex);
  Snapshot snapshot = g_snapshot;
  if (g_window_closed.time_since_epoch().count() == 0) return snapshot;
  const auto now = std::chrono::steady_clock::now();
  snapshot.age_ms = now > g_window_closed ? static_cast<uint64_t>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - g_window_closed).count())
                                          : 0u;
  return snapshot;
}

}  // namespace uplift::addon::framerate
