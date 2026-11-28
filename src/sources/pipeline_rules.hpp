#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <optional>

#include "color/color_pipeline.hpp"
#include "color/encoding.hpp"
#include "nr/types.hpp"
#include "sources/auto_exposure.hpp"
#include "sources/look_plan.hpp"
#include "sources/nr_pipeline.hpp"
#include "sources/work_size.hpp"

namespace uplift::sources {

// The meter's RGBA32F state: texel 0 is (governed final multiplier, independent meter EV, anchor, set), texel 1 (the game's texel, its factor, the game's
// exposure, the meter's target). Auto's check reads both texels back.
inline constexpr uint32_t METER_STATE_TEXELS = 2u;
inline constexpr uint32_t METER_TEXEL_COMPONENTS = 4u;
inline constexpr size_t METER_SAMPLE_FLOATS = size_t{METER_STATE_TEXELS} * METER_TEXEL_COMPONENTS;
// Texel 0's components the check reads: the meter's E, and the scene's anchor.
inline constexpr size_t METERED_STOPS_COMPONENT = 1u;
inline constexpr size_t ANCHOR_STOPS_COMPONENT = 2u;
// Texel 1's: the game's exposure texel, its factor, what the encode would multiply by, and the meter's target.
inline constexpr size_t GAME_TEXEL_COMPONENT = METER_TEXEL_COMPONENTS + 0u;
inline constexpr size_t GAME_FACTOR_COMPONENT = METER_TEXEL_COMPONENTS + 1u;
inline constexpr size_t GAME_EXPOSURE_COMPONENT = METER_TEXEL_COMPONENTS + 2u;
inline constexpr size_t TARGET_STOPS_COMPONENT = METER_TEXEL_COMPONENTS + 3u;
// AutoExposureBlend's ends: 0 the game's exposure, 1 the meter's.
inline constexpr float AUTO_BLEND_GAME = 0.f;
inline constexpr float AUTO_BLEND_METER = 1.f;

// Clamp the effective image, never a padded canvas. The source's own size remains the eligibility gate.
// A malformed/stale layout that needs resizing loses its old warp rather than reusing mismatched coordinates.
[[nodiscard]] inline WorkLayout ResolveLayout(const WorkLayout& layout, nr::Size output) {
  const nr::Size requested = (layout.image.Empty() ? output : layout.image);
  const bool valid = nr::MeetsNrFloor(output) && nr::MeetsNrFloor(requested)
                     && requested.width <= output.width && requested.height <= output.height;
  const nr::Size image = (valid ? requested : WorkSize(requested.width, requested.height, output));
  return {.image = image, .upsampling = layout.upsampling,
          .foveation = (valid ? layout.foveation : nr::foveation::Grid{})};
}

// The encode's and the decodes' shader_options for `fixes`.
[[nodiscard]] inline uint32_t ShaderOptions(const ColorFixes& fixes) {
  return static_cast<uint32_t>(fixes.primaries)  // the shaders resolve AUTO per encoding
         | (fixes.near_black_guard ? color::shader_options::NEAR_BLACK_GUARD : 0u)
         | (fixes.transfer == color::NeuralTransfer::CONSISTENT ? color::shader_options::CONSISTENT : 0u);
}

// One of Auto's checks, from the meter state's two texels as read back.
[[nodiscard]] inline ExposureSample ExposureSampleFromTexels(const std::array<float, METER_SAMPLE_FLOATS>& texels) {
  return {.texture_value = texels[GAME_TEXEL_COMPONENT],
          .factor = texels[GAME_FACTOR_COMPONENT],
          .game = texels[GAME_EXPOSURE_COMPONENT],
          .metered_stops = texels[METERED_STOPS_COMPONENT],
          .target_stops = texels[TARGET_STOPS_COMPONENT],
          .anchor_stops = texels[ANCHOR_STOPS_COMPONENT],
          .applied_stops = std::log2(texels[0])};
}

// InputExposure's choice for one recording, before the meter's state is made or retired.
enum class ExposurePlan : uint8_t {
  NONE,
  GAME,
  CHECK_GAME,  // Auto at 0%: bind the game directly and run the meter only to validate it.
  BLEND,
  METERED,
};
struct ExposureDecision {
  ExposurePlan plan = ExposurePlan::NONE;
  bool fallback_game = false;  // Auto's blend (including 100%) keeps the game if allocating the state fails.
  [[nodiscard]] bool Probes() const { return plan == ExposurePlan::CHECK_GAME || plan == ExposurePlan::BLEND; }
  [[nodiscard]] bool WantsState() const { return Probes() || plan == ExposurePlan::METERED; }
};

// Plan before allocating the meter's state; PickExposure resolves allocation failure afterwards.
[[nodiscard]] inline ExposureDecision DecideExposure(const ColorFixes& fixes, bool game_exposure, bool can_meter, bool latched) {
  switch (fixes.input_exposure) {
    case color::InputExposure::AUTO:
      if (!can_meter) return {.plan = game_exposure ? ExposurePlan::GAME : ExposurePlan::NONE};
      if (!game_exposure || latched) return {.plan = ExposurePlan::METERED};
      if (fixes.auto_blend >= AUTO_BLEND_METER) return {.plan = ExposurePlan::METERED, .fallback_game = true};
      if (fixes.auto_blend <= AUTO_BLEND_GAME) return {.plan = ExposurePlan::CHECK_GAME};
      return {.plan = ExposurePlan::BLEND, .fallback_game = true};
    case color::InputExposure::GAME: return {.plan = ExposurePlan::GAME};
    case color::InputExposure::METERED: return {.plan = can_meter ? ExposurePlan::METERED : ExposurePlan::NONE};
    case color::InputExposure::MANUAL: return {};
  }
  return {};
}

// What the encode and the decode bind as their exposure.
enum class ExposureSource : uint8_t {
  NONE,   // nothing
  GAME,   // the game's texture and factor
  STATE,  // the meter's state, at a factor of 1
};
struct ExposureOutcome {
  ExposureSource source = ExposureSource::NONE;
  bool metered = false;
  bool probe = false;
  bool with_game = false;   // the meter is handed the game's texture and factor too
  float game_weight = 0.f;  // with `probe`: the game's share of the state's multiplier in stops
  ExposureReport report;    // the Details line's
};

// The decision's outcome once the meter's state was made, or could not be (`have_state`).
[[nodiscard]] inline ExposureOutcome PickExposure(const ExposureDecision& decision, const ColorFixes& fixes, bool game_exposure, bool have_state,
                                                  const AutoExposure& auto_exposure) {
  if (decision.plan == ExposurePlan::BLEND && have_state) {
    return {.source = ExposureSource::STATE, .metered = true, .probe = true, .with_game = true,
            .game_weight = AUTO_BLEND_METER - std::clamp(fixes.auto_blend, AUTO_BLEND_GAME, AUTO_BLEND_METER),
            .report = {.use = ExposureUse::BLEND, .blend = fixes.auto_blend, .gap = auto_exposure.LastGap()}};
  }
  if (decision.plan == ExposurePlan::METERED && have_state) {
    const bool latched = (fixes.input_exposure == color::InputExposure::AUTO && game_exposure && auto_exposure.Latched());
    return {.source = ExposureSource::STATE, .metered = true,
            .report = {.use = ExposureUse::METERED,
                       .stops_off = (latched ? std::optional<float>(auto_exposure.StopsOff()) : std::nullopt),
                       .latch = (latched ? auto_exposure.Reason() : AutoLatch::NONE)}};
  }
  // Direct game exposure, or Auto's blend when the state could not be allocated.
  if (decision.plan == ExposurePlan::GAME || decision.plan == ExposurePlan::CHECK_GAME || decision.fallback_game) {
    return {.source = ExposureSource::GAME, .probe = (decision.Probes() && have_state), .with_game = true,
            .game_weight = (decision.plan == ExposurePlan::CHECK_GAME ? 1.f : 0.f),
            .report = {.use = (game_exposure ? ExposureUse::GAME : ExposureUse::NONE)}};
  }
  return {};
}

// The rect of motion vectors the stabiliser reads in a texture of `texture`: the whole texture for a rect with an empty side; none when the rect reaches
// outside the texture.
[[nodiscard]] inline std::optional<nr::Rect> MotionRectOf(nr::Rect rect, nr::Size texture) {
  if (rect.width == 0u || rect.height == 0u) {
    rect = {.x = 0u, .y = 0u, .width = texture.width, .height = texture.height};
  }
  if (uint64_t{rect.x} + rect.width > texture.width || uint64_t{rect.y} + rect.height > texture.height) return std::nullopt;
  return rect;
}

}  // namespace uplift::sources
