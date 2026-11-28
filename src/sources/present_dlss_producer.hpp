#pragma once

#include <cstdint>

#include "nr/types.hpp"

namespace uplift::sources {

// An isolated DLSS viewport cannot supply motion for the whole Present image. A
// full-size intermediate is allowed, since games often tone map it into the swapchain.
[[nodiscard]] constexpr bool CoversPresent(nr::Rect region, nr::Size output, nr::Size present) {
  return !present.Empty() && output == present && region.x == 0u && region.y == 0u
      && nr::Size{region.width, region.height} == present;
}

// Learn a unique full-frame producer from a closed Present window, then capture
// only that producer in the next window. Picking the first Evaluate would make
// a resource or handle switch depend on callback order. Only an output address
// is compared within a window; the game resource itself is never retained.
class PresentDlssProducer {
 public:
  enum class Capture { NOT_FULL_FRAME, UNSELECTED, ALREADY_CAPTURED, RECORD, NO_INPUTS };
  struct Inputs { bool motion = false, depth = false; };
  struct Boundary { bool changed = false, game_reset = false, ambiguous = false; };

  [[nodiscard]] nr::Size Extent() const { return extent_; }
  [[nodiscard]] uint64_t Serial() const { return serial_; }

  // Invalidate any previous mapping when the Present target changes or goes away.
  bool SetExtent(nr::Size extent) {
    if (extent == extent_) return false;
    const bool invalidated = !extent_.Empty();
    extent_ = extent;
    serial_ = 0u;
    window_ = {};
    return invalidated;
  }

  Capture Observe(uint64_t frame, uint64_t serial, uintptr_t output, bool covers, bool reset, Inputs requested) {
    if (window_.frame != frame) window_ = {.frame = frame};
    if (!covers || serial == 0u || output == 0u) return Capture::NOT_FULL_FRAME;
    if (window_.candidate == 0u) {
      window_.candidate = serial;
      window_.output = output;
    } else if (window_.candidate != serial || window_.output != output) {
      window_.ambiguous = true;
    }
    if (window_.ambiguous) return Capture::UNSELECTED;
    window_.game_reset |= reset;
    if (serial != serial_) return Capture::UNSELECTED;
    if (!requested.motion && !requested.depth) return Capture::NO_INPUTS;
    const Inputs pending = Pending(requested);
    return pending.motion || pending.depth ? Capture::RECORD : Capture::ALREADY_CAPTURED;
  }

  [[nodiscard]] Inputs Pending(Inputs requested) const {
    return {.motion = requested.motion && !window_.captured.motion, .depth = requested.depth && !window_.captured.depth};
  }
  void Captured(Inputs copied) {
    window_.captured.motion |= copied.motion;
    window_.captured.depth |= copied.depth;
  }

  Boundary Finish(uint64_t frame) {
    if (window_.frame != frame) return {};  // No evaluate: leave the one-frame-old fallback eligible.
    const uint64_t next = window_.ambiguous ? 0u : window_.candidate;
    const Boundary result{.changed = serial_ != 0u && serial_ != next,
                          .game_reset = next != 0u && window_.game_reset,
                          .ambiguous = window_.ambiguous};
    serial_ = next;
    window_ = {};
    return result;
  }

 private:
  struct Window {
    uint64_t frame = 0u, candidate = 0u;
    uintptr_t output = 0u;
    bool ambiguous = false, game_reset = false;
    Inputs captured;
  };
  nr::Size extent_;
  uint64_t serial_ = 0u;
  Window window_;
};

}  // namespace uplift::sources
