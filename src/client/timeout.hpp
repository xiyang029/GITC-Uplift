#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "bridge/bridge_sequence.hpp"
#include "text/timeout_messages.hpp"

namespace uplift::client {

// Each transport keeps its own reset points and Stop callback (including API-specific resource teardown).
template <typename Stop>
void CountTimeout(uint32_t& consecutive, std::string_view what, std::chrono::milliseconds cap, Stop&& stop) {
  if (++consecutive >= bridge::MAX_CONSECUTIVE_TIMEOUTS) {
    stop(text::ConsecutiveTimeoutsText(what, cap, bridge::MAX_CONSECUTIVE_TIMEOUTS));
  }
}

}  // namespace uplift::client
