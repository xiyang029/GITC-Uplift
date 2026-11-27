#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace uplift::nr {

// Order throughout the native replacement: depth, motion, control mask, backbuffer, UI, UI alpha.
enum class NativeInputStatus : uint8_t {
  MISSING, READY, INVALID_REGION, OUTPUT_ALIAS, SRV_DENIED, STATE_UNKNOWN, STATE_UNREADABLE,
  DISABLED, REMAPPED, FORMAT_UNSUPPORTED, REMAP_FAILED,
};
using NativeInputStatuses = std::array<NativeInputStatus, 6>;

constexpr std::string_view NativeInputStatusName(NativeInputStatus status) {
  switch (status) {
    case NativeInputStatus::MISSING: return "missing";
    case NativeInputStatus::READY: return "game";
    case NativeInputStatus::INVALID_REGION: return "invalid-region";
    case NativeInputStatus::OUTPUT_ALIAS: return "aliases-output";
    case NativeInputStatus::SRV_DENIED: return "SRV-denied";
    case NativeInputStatus::STATE_UNKNOWN: return "state-unknown";
    case NativeInputStatus::STATE_UNREADABLE: return "state-not-readable";
    case NativeInputStatus::DISABLED: return "disabled-by-panel";
    case NativeInputStatus::REMAPPED: return "remapped";
    case NativeInputStatus::FORMAT_UNSUPPORTED: return "unsupported-format";
    case NativeInputStatus::REMAP_FAILED: return "remap-failed";
  }
  return "unknown";
}

struct NativeInputReport {
  NativeInputStatuses status{};
  bool zero_motion = false;
  bool foveated = false;
  bool ui_correction = false;
  friend bool operator==(const NativeInputReport&, const NativeInputReport&) = default;
};

}  // namespace uplift::nr
