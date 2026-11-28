#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "color/encoding.hpp"

namespace uplift::sources {

// The meaning of metered values is independent of the processing image's dimensions.
struct MeterBasis {
  uint8_t stage = 0u;
  color::Encoding encoding = color::Encoding::AUTO;
  color::Primaries primaries = color::Primaries::AUTO;
  float input_scale = 0.f;
  friend bool operator==(const MeterBasis&, const MeterBasis&) = default;
};

// One look at the game's DLSS exposure beside Uplift's meter, read back from the meter's state a few frames after it was written.
struct ExposureSample {
  float texture_value = 1.f;  // the game's ExposureTexture texel (1 without one)
  float factor = 1.f;         // DLSS.Exposure.Scale / DLSS.Pre.Exposure
  float game = 1.f;           // what the encode multiplies by: color::GameExposure of the two
  float metered_stops = 0.f;  // independent governed meter EV; the applied exposure has its own adaptation history
  float target_stops = 0.f;   // the meter's target this frame, which E moves toward at the Adaptation rates
  float anchor_stops = 0.f;   // the meter's anchor (the scene's mean log luminance it exposes to middle grey), for the held check
  float applied_stops = 0.f;  // actual final exposure read by the encode, including blend adaptation
};

// Which validity rule latched Auto to Metered.
enum class AutoLatch : uint8_t {
  NONE,
  ABOVE,  // the game's exposure sat more than ABOVE_LIMIT_STOPS above the meter's span
  BELOW,  // more than BELOW_LIMIT_STOPS below it
  HELD,   // the unit game exposure stayed fixed while the scene moved substantially
};

// Input exposure = Auto with a game that passes an exposure. The game's exposure is valid while it lies within the window around Uplift's meter;
// once it has been invalid for SUSTAIN_SECONDS (and SUSTAIN_SAMPLES samples) in a row, Auto latches to Metered for the pipeline's life: a game whose value is
// wrong stays wrong, and a latch never flips back. Pure, so the unit tests drive it.
// The window is asymmetric. A non-unit game exposure that holds still while the scene moves remains valid because
// some games intentionally pre-expose their input. The unit ExposureTexture/factor sentinel is different: when it
// holds while the scene moves, Auto treats it as a broken game exposure and latches to the meter.
class AutoExposure {
 public:
  // NR After DLSS sees the image before the game's tone mapper, so a right game exposure sits 1.5-2.5 stops above the meter, and a scene the game keeps dark
  // on purpose sits below it. A game passing 1 on physical-unit images sits 5-10 stops above.
  static constexpr float ABOVE_LIMIT_STOPS = 4.f;
  static constexpr float BELOW_LIMIT_STOPS = 5.f;
  // Held: a unit game exposure within HELD_EPSILON_STOPS of one value while the meter's anchor spans more than HELD_ANCHOR_STOPS within
  // SUSTAIN_SECONDS.
  static constexpr float HELD_EPSILON_STOPS = 0.01f;
  static constexpr float HELD_ANCHOR_STOPS = 2.f;
  static constexpr double SUSTAIN_SECONDS = 1.0;
  static constexpr uint32_t SUSTAIN_SAMPLES = 8u;
  static constexpr double DEBUG_SECONDS = 1.0;  // one debug line per this many seconds

  // `seconds`: a steady clock, in seconds, when the sample was recorded. True when this sample latched Metered.
  bool Observe(const ExposureSample& sample, double seconds);
  // Discard an interrupted probe run while preserving a permanent latch.
  void DiscardPending();
  [[nodiscard]] bool Latched() const { return latched_; }
  [[nodiscard]] AutoLatch Reason() const { return reason_; }
  // At the latch: how far the game's exposure was from the meter's E, in stops (always positive).
  [[nodiscard]] float StopsOff() const { return stops_off_; }
  // How far the meter's anchor has moved while a unit game exposure held still, in stops, once that is more than HELD_ANCHOR_STOPS within
  // SUSTAIN_SECONDS; none outside a unit candidate. Auto latches when this becomes available.
  [[nodiscard]] std::optional<float> HeldThroughMove() const;
  // The game's exposure minus the meter's E, in stops, of the latest usable sample: none before one.
  [[nodiscard]] std::optional<float> LastGap() const { return last_gap_; }
  // True once per DEBUG_SECONDS: the pipelines log one debug line when it is.
  bool DebugDue(double seconds);
  // How far `sample`'s game exposure lies outside the meter's [E, target] span, in stops: positive above it, negative below, 0 inside. The nearer end counts,
  // so neither side's adaptation lag does. Not finite for an unusable sample, which Observe ignores.
  [[nodiscard]] static float OutsideSpan(const ExposureSample& sample);

 private:
  // The held check's bookkeeping for one usable sample.
  void ClearHeld();
  void NoteHeld(float game_stops, float anchor_stops, double seconds);

  bool latched_ = false;
  AutoLatch reason_ = AutoLatch::NONE;
  float stops_off_ = 0.f;
  std::optional<float> last_gap_;
  std::optional<double> since_;  // the first sample of the current run of invalid ones
  uint32_t run_samples_ = 0u;
  std::optional<double> last_debug_;
  // Held: the value the game's exposure holds, the anchors seen within the last SUSTAIN_SECONDS since, and whether they spanned the limit (kept until the
  // game's exposure moves).
  std::optional<float> held_stops_;
  std::deque<std::pair<double, float>> held_anchors_;
  bool held_through_move_ = false;
  float held_anchor_span_ = 0.f;
};

// Auto blends in stops, with 0 the game's and 1 the meter's; a latch always uses the meter.
[[nodiscard]] float AutoStops(float blend, bool latched, float game_stops, float metered_stops);

// What the latest recording's encode read as its exposure, for the Details line.
enum class ExposureUse : uint8_t {
  NONE,     // none: an sRGB, scRGB or HDR10 image, Manual, or Game without a game exposure
  GAME,     // the game's DLSS exposure
  METERED,  // Uplift's meter
  BLEND,    // Auto's Blend of the two
};
struct ExposureReport {
  ExposureUse use = ExposureUse::NONE;
  std::optional<float> stops_off;  // METERED because Auto latched: how far off the game's exposure was
  AutoLatch latch = AutoLatch::NONE;  // and which rule fired
  float blend = 0.f;                  // BLEND: AutoExposureBlend, 0 the game's, 1 the meter's
  std::optional<float> gap;           // BLEND: the game's exposure minus the meter's E, in stops, once a check has been read
  friend bool operator==(const ExposureReport&, const ExposureReport&) = default;
};
// "Input exposure: the game's", "Input exposure: metered (the game's was 6.3 stops off: above)", "Input exposure: metered",
// "Input exposure: blend 50 % game/meter (gap +2.3 stops)"; empty for NONE.
[[nodiscard]] std::string ExposureLine(const ExposureReport& report);
// The one INFO line Auto logs when it latches to Metered: the rule, the game's texel, its factor, what that gives, and the meter's values, in stops too.
[[nodiscard]] std::string AutoLatchMessage(const ExposureSample& sample, const AutoExposure& judge);
// The debug line Auto logs once every AutoExposure::DEBUG_SECONDS before a latch: the game in stops, the meter's E and target, the gap, what the blend
// gives, and `held_move` (AutoExposure::HeldThroughMove) when there is one.
[[nodiscard]] std::string AutoDebugMessage(const ExposureSample& sample, float blend, std::optional<float> held_move);

// Common CPU sample validation and rate-limited diagnostics for both GPU backends.
void ObserveExposureSample(AutoExposure& judge, const ExposureSample& sample, double recorded_seconds, float blend);
[[nodiscard]] double ExposureCheckSeconds();

// A new stage, encoding, resolved primaries or input scale invalidates the meter's previous basis.
inline bool UpdateMeterBasis(std::optional<MeterBasis>& current, bool& snap, uint8_t stage, color::Encoding encoding,
                             color::Primaries primaries, float input_scale) {
  const MeterBasis next = {.stage = stage, .encoding = encoding, .primaries = color::ResolvePrimaries(primaries, encoding), .input_scale = input_scale};
  if (current == next) return false;
  current = next;
  snap = true;
  return true;
}

}  // namespace uplift::sources
