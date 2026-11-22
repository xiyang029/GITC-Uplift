#include "look/look_math.hpp"

#include <algorithm>
#include <cmath>

namespace uplift::look {
namespace {

constexpr uint32_t MIN_LEVEL_SIDE = 4u;
constexpr uint32_t MAX_LEVELS = 16u;

}  // namespace

bool LookRuns(const LookSettings& settings) {
  return settings.enabled && (settings.shape != ShapeSettings{} || settings.stabilize != StabilizeMode::OFF);
}

nr::Size LevelSize(nr::Size image, uint32_t level) {
  const uint32_t round_up = (1u << level) - 1u;
  return {(image.width + round_up) >> level, (image.height + round_up) >> level};
}

uint32_t LevelOffset(nr::Size image, uint32_t level) {
  uint32_t offset = 0u;
  for (uint32_t below = 1u; below < level; ++below) {
    offset += LevelSize(image, below).width;
  }
  return offset;
}

Atlas MakeAtlas(nr::Size image) {
  uint32_t levels = 1u;
  while (levels < MAX_LEVELS) {
    const nr::Size next = LevelSize(image, levels + 1u);
    if (std::min(next.width, next.height) < MIN_LEVEL_SIDE) break;
    ++levels;
  }
  return {.image = image, .levels = levels, .size = {LevelOffset(image, levels + 1u), LevelSize(image, 1u).height}};
}

nr::Size LowHistorySize(const Atlas& atlas, float detail_radius) {
  const uint32_t level = MakeBands(detail_radius, atlas.image.height, atlas.levels).stable_level;
  nr::Size size = LevelSize(atlas.image, level);
  if (level < atlas.levels) size.width += LevelSize(atlas.image, level + 1u).width;
  return size;
}

Bands MakeBands(float detail_radius, uint32_t height, uint32_t levels) {
  const float sigma = std::max(detail_radius / 100.f * static_cast<float>(height), 0.5f);
  const float top = static_cast<float>(levels);
  const float low_level = std::clamp(std::log2(2.f * sigma), 1.f, top);
  return {
      .low_level = low_level,
      .peak_level = static_cast<uint32_t>(std::clamp(std::ceil(std::log2(sigma)), 1.f, top)),
      .stable_level = static_cast<uint32_t>(std::floor(low_level)),
  };
}

FacesParameters MakeFacesParameters(float lighting_scale, const FacesTuning& tuning, uint32_t height, uint32_t levels) {
  return {.low_level = MakeBands(lighting_scale, height, levels).low_level,
          .mask_level = MakeBands(tuning.softness, height, levels).low_level,
          // Edge falloff reaches the combine's per-pixel weights at level zero; the other bands stop at the first pyramid level.
          .edge_level = std::clamp(std::log2(2.f * std::max(tuning.edge_falloff / 100.f * static_cast<float>(height), 0.5f)), 0.f, static_cast<float>(levels)),
          .dead_zone = tuning.threshold * FACES_PERCENT,
          .full_weight = std::max(tuning.full, tuning.threshold + FACES_MIN_SPAN) * FACES_PERCENT,
          .coverage_gain = tuning.strength,
          .density = std::max(tuning.density, FACES_MIN_DENSITY),
          .speck_radius = std::min(tuning.speck_size, FACES_MAX_SPECK_RADIUS),
          .fill = tuning.fill_skin,
          .fill_level = MakeBands(tuning.fill_radius, height, levels).low_level,
          .fill_tolerance = std::max(tuning.fill_tolerance, FACES_MIN_FILL_TOLERANCE)};
}

float StabilizeRate(float frame_seconds, float stabilize_ms) {
  const float seconds = std::clamp(frame_seconds, 0.001f, 0.1f);
  return 1.f - std::exp(-seconds / (stabilize_ms / 1000.f));
}

}  // namespace uplift::look
