#pragma once

#include "sources/motion_source.hpp"

namespace uplift::sources {

struct ChangeStabilizeInput {
  bool reset;
  bool reproject = true;
};

struct PresentChangeMotion {
  MotionSource source;  // real vectors only, never an NGX zero stand-in
  bool fresh;
  bool allow_gap;
};

// Present's delta history is independent of the model's motion binding. Keep it through one missed
// copy, but use only same-frame vectors to reproject it. Repeated misses settle into static history.
class PresentChangeHistory {
 public:
  [[nodiscard]] constexpr ChangeStabilizeInput Advance(PresentChangeMotion motion, bool reset) {
    const bool has_source = motion.source != MotionSource::NONE;
    if (!reset && motion.allow_gap && !has_source && source_ != MotionSource::NONE && !missed_) {
      missed_ = true;
      return {.reset = false, .reproject = false};
    }
    reset |= motion.source != source_;
    source_ = motion.source;
    missed_ = false;
    return {.reset = reset, .reproject = has_source && motion.fresh};
  }

 private:
  MotionSource source_ = MotionSource::NONE;
  bool missed_ = false;
};

}  // namespace uplift::sources
