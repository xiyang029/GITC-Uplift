#include "sources/work_size.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace uplift::sources {
namespace {

constexpr auto SETTLE_TIME = std::chrono::milliseconds(500);
constexpr uint64_t SETTLE_PERCENT = 2u;  // a side must differ by more than this share of the applied size

}  // namespace

nr::Size WorkSize(double target_width, double target_height, nr::Size output) {
  if (!nr::MeetsNrFloor(output)) return output;
  const auto round_even = [](double side) { return static_cast<uint32_t>(std::max(2.0 * std::round(side / 2.0), 2.0)); };
  nr::Size work = {round_even(target_width), round_even(target_height)};
  if (work.width >= output.width && work.height >= output.height) return output;
  if (!nr::MeetsNrFloor(work)) {
    // f = max(MIN_NR_SHORT_SIDE / shorter, MIN_NR_LONG_SIDE / longer) as the exact fraction numerator / denominator, chosen by
    // cross-multiplying, so the scaled sides carry no floating-point error before rounding up to even.
    const uint64_t shorter = std::min(work.width, work.height);
    const uint64_t longer = std::max(work.width, work.height);
    const bool short_side_limits = uint64_t{nr::MIN_NR_SHORT_SIDE} * longer >= uint64_t{nr::MIN_NR_LONG_SIDE} * shorter;
    const uint64_t numerator = (short_side_limits ? nr::MIN_NR_SHORT_SIDE : nr::MIN_NR_LONG_SIDE);
    const uint64_t denominator = (short_side_limits ? shorter : longer);
    const auto scale_up = [numerator, denominator](uint64_t side) {
      return static_cast<uint32_t>(2u * ((side * numerator + 2u * denominator - 1u) / (2u * denominator)));
    };
    work = {scale_up(work.width), scale_up(work.height)};
  }
  work = {std::min(work.width, output.width), std::min(work.height, output.height)};
  if (!nr::MeetsNrFloor(work)) {
    const bool landscape = output.width >= output.height;
    work.width = std::max(work.width, landscape ? nr::MIN_NR_LONG_SIDE : nr::MIN_NR_SHORT_SIDE);
    work.height = std::max(work.height, landscape ? nr::MIN_NR_SHORT_SIDE : nr::MIN_NR_LONG_SIDE);
  }
  return work;
}

nr::foveation::Plan FoveatedWorkPlan(bool enabled, const nr::foveation::Shares& shares, nr::Size output, nr::Size uniform) {
  if (!nr::MeetsNrFloor(uniform)) return {.width = uniform.width, .height = uniform.height};
  const auto requested = nr::foveation::MakePlanFromShares(enabled, shares, output.width, output.height, uniform.width, uniform.height);
  if (nr::MeetsNrFloor({requested.width, requested.height})) return requested;
  // Preserve any already-eligible warp, including one whose aspect changes. Otherwise choose a floor
  // orientation the uniform grid can contain, preferring the requested image's orientation when both fit.
  const bool landscape = uniform.height < nr::MIN_NR_LONG_SIDE
                         || (uniform.width >= nr::MIN_NR_LONG_SIDE && requested.width >= requested.height);
  return nr::foveation::MakePlanFromShares(enabled, shares, output.width, output.height, uniform.width, uniform.height,
                                         landscape ? nr::MIN_NR_LONG_SIDE : nr::MIN_NR_SHORT_SIDE,
                                         landscape ? nr::MIN_NR_SHORT_SIDE : nr::MIN_NR_LONG_SIDE);
}

nr::Size WorkSizeSettle::Update(nr::Size target, std::chrono::steady_clock::time_point now, bool held) {
  if (target != last_target_) {
    last_target_ = target;
    target_since_ = now;
  }
  if (held) {
    was_held_ = true;
    if (applied_.Empty()) {
      applied_ = target;  // nothing applied yet: there is nothing to hold
    }
    return applied_;
  }
  if (applied_.Empty() || std::exchange(was_held_, false)) {
    applied_ = target;
    differing_since_.reset();
    return applied_;
  }
  const auto differs = [](uint32_t side, uint32_t applied) {
    return uint64_t{side > applied ? side - applied : applied - side} * 100u > uint64_t{applied} * SETTLE_PERCENT;
  };
  if (differs(target.width, applied_.width) || differs(target.height, applied_.height)) {
    if (!differing_since_) {
      differing_since_ = now;
    }
  } else {
    differing_since_.reset();
  }
  const bool moved_far = (differing_since_.has_value() && now - *differing_since_ >= SETTLE_TIME);
  const bool rested = (target != applied_ && now - target_since_ >= SETTLE_TIME);
  if (moved_far || rested) {
    applied_ = target;
    differing_since_.reset();
  }
  return applied_;
}

void WorkSizeSettle::Reset() {
  applied_ = {};
  last_target_ = {};
  differing_since_.reset();
  was_held_ = false;
}

}  // namespace uplift::sources
