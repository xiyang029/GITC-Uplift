#pragma once

#include <cmath>
#include <cstdint>

#include "color/shader_values.h"
#include "nr/types.hpp"

namespace uplift::look {

// The Stabilize combo, in stored-index order.
enum class StabilizeMode : uint32_t {
  OFF = 0u,
  STATIC = 1u,  // a moving average in place: photo modes and still scenes
  MOTION = 2u,  // reprojected along the motion vectors; without them, change-gated static
};

// Shape multipliers and limits, in the order of operations.
struct ShapeSettings {
  float tone = 1.f;           // k_T, Large-scale tone
  float detail = 1.f;         // k_F, Fine detail
  float halo = 0.f;           // h, Halo suppression
  float brighten = 1.f;       // k_B
  float darken = 1.f;         // k_D
  float color = 1.f;          // k_C: movement toward or away from grey
  float hue = 1.f;            // k_H: the hue turn
  float shadows = 1.f;        // k_sh
  float midtones = 1.f;       // k_mid
  float highlights = 1.f;     // k_hi
  float edit_strength = 1.f;  // k_E
  float max_brighten = 0.f;   // M_B, stops; 0 = no limit
  float max_darken = 0.f;     // M_D, stops; 0 = no limit
  float max_color = 0.f;      // M_C, normalised chroma; 0 = no limit
  friend bool operator==(const ShapeSettings&, const ShapeSettings&) = default;
};

// Shape result and its rows. All live: none restarts NR's history.
struct LookSettings {
  bool enabled = false;
  ShapeSettings shape;
  float detail_radius = 2.f;  // % of the work image's height: where broad change ends
  StabilizeMode stabilize = StabilizeMode::OFF;
  float stabilize_ms = 250.f;  // τ
  bool stabilize_detail = false;
  // NR's own change field, averaged along the vectors whatever the look does with it (the pass runs after the shape, and without Shape result too): NR's
  // output flickers by a grey level or two where the input is still. `stabilize_change` switches it off.
  bool stabilize_change = false;  // independent of `enabled`: it alters the picture, so it is opted into
  uint32_t stabilize_change_ms = 60u;  // τ, whole milliseconds
  friend bool operator==(const LookSettings&, const LookSettings&) = default;
};

// The CUT pass resets Look history on a large mean-light change or a substantial change in the coarse tonal spread.
inline constexpr float SCENE_CUT_MEAN_STOPS = UPLIFT_SCENE_CUT_MEAN_STOPS;
inline constexpr float SCENE_CUT_SPREAD_STOPS = UPLIFT_SCENE_CUT_SPREAD_STOPS;
[[nodiscard]] inline bool SceneCutDetected(float mean, float spread, float previous_mean, float previous_spread, bool previous_valid) {
  return previous_valid && (std::abs(mean - previous_mean) > SCENE_CUT_MEAN_STOPS
                            || std::abs(spread - previous_spread) > SCENE_CUT_SPREAD_STOPS);
}

// Whether the look stage runs: Shape result is on and a control would change NR's result.
[[nodiscard]] bool LookRuns(const LookSettings& settings);

// The pyramid's levels 1..levels side by side in one atlas texture: level k is ceil(image / 2^k), at x = the widths
// of levels 1..k-1, y = 0 (level 0 is the image). look_common.hlsli derives the same layout.
[[nodiscard]] nr::Size LevelSize(nr::Size image, uint32_t level);
[[nodiscard]] uint32_t LevelOffset(nr::Size image, uint32_t level);
struct Atlas { nr::Size image; uint32_t levels = 0u; nr::Size size; };  // levels: down to a shorter side of 4, at least 1
[[nodiscard]] Atlas MakeAtlas(nr::Size image);

// Where the bands sit for Detail radius (% of height) on a work image of `height` rows:
// λ = log2(2σ) clamped to [1, levels]; the halo's max level ceil(log2 σ); the stabiliser's floor(λ) (and the next).
struct Bands { float low_level = 1.f; uint32_t peak_level = 1u; uint32_t stable_level = 1u; };
[[nodiscard]] Bands MakeBands(float detail_radius, uint32_t height, uint32_t levels);
[[nodiscard]] nr::Size LowHistorySize(const Atlas& atlas, float detail_radius);
// The face mask's live tuning in the Advanced rows. Shader constants only: none restarts NR or
// the twin.
// The threshold and full protection are relative, % of the pixel's own brightness (faces_cs.hlsl's FirstWeight), so
// a specular glint's brightness alone does not make NR's noise exceed them; and the lighting is added back only where the weights are dense (`density`).
struct FacesTuning {
  float threshold = 1.5f;      // FaceMaskThreshold: a change below this % of the pixel's brightness is the two runs' noise
  float full = 6.f;            // FaceMaskFull: from this % on it protects fully (kept above `threshold` by MakeFacesParameters)
  float strength = 2.f;        // FaceMaskStrength: the mask's gain on its coverage (2: a half-covered neighbourhood reads as inside)
  float softness = 0.5f;       // FaceMaskSoftness: the mask's radius, % of the height
  float edge_falloff = 0.15f;  // FaceEdgeFalloff: the light blur of the coverage that fades kept lighting out at the outline, % of the height
  float density = 0.3f;        // FaceLightingDensity: the weights' coverage at the Lighting scale from which lighting is added back in full
  // FaceSpeckSize, the despike ring's Chebyshev distance in pixels (spikes up to 2r - 1 px wide keep NR's normal result); 0: off.
  uint32_t speck_size = 2u;
  bool fill_skin = true;         // FaceFillSkin: the mask grows over skin of the marked skin's colour
  float fill_radius = 3.f;       // FaceFillRadius: how far it grows, % of the height
  float fill_tolerance = 0.04f;  // FaceFillTolerance: the rg-chromaticity distance from the local skin colour at which it stops
  friend bool operator==(const FacesTuning&, const FacesTuning&) = default;
};
// Keep faces: faces_cs.hlsl's parameters on a canvas of `levels` levels for a work image of `height` rows. The levels are MakeBands' λ:
// `low_level` for Lighting scale (% of the height), the band above which a change on characters counts as lighting; `mask_level` for the mask's softness,
// where the coverage of the character's pixels is read; `edge_level` for the edge falloff, the only one that goes down to 0 (the per-pixel
// weights). The weights' dead zone and full weight are fractions of the pixel's brightness, the full weight at least
// FACES_MIN_SPAN percent above the dead zone (the user's own values are left as they are); `density` is FacesTuning's, kept above FACES_MIN_DENSITY.
inline constexpr float FACES_PERCENT = 0.01f;
inline constexpr float FACES_MIN_SPAN = 0.25f;  // in percent
inline constexpr float FACES_MIN_DENSITY = 0.01f;
inline constexpr uint32_t FACES_MAX_SPECK_RADIUS = 3u;  // faces_cs.hlsl's despike tile apron (SPECK_MAX_RADIUS)
inline constexpr float FACES_MIN_FILL_TOLERANCE = 0.001f;
struct FacesParameters {
  float low_level = 1.f;
  float mask_level = 1.f;
  float edge_level = 1.f;
  float dead_zone = 1.5f * FACES_PERCENT;
  float full_weight = 6.f * FACES_PERCENT;
  float coverage_gain = 2.f;
  float density = 0.3f;
  uint32_t speck_radius = 2u;    // FacesTuning::speck_size, at most FACES_MAX_SPECK_RADIUS; 0: no despike
  bool fill = true;              // FacesTuning::fill_skin
  float fill_level = 1.f;        // MakeBands' λ for the Fill radius
  float fill_tolerance = 0.04f;  // at least FACES_MIN_FILL_TOLERANCE
};
[[nodiscard]] FacesParameters MakeFacesParameters(float lighting_scale, const FacesTuning& tuning, uint32_t height, uint32_t levels);
// α = 1 − exp(−Δt/τ), with Δt clamped to [1 ms, 100 ms]: frame-rate independent smoothing.
[[nodiscard]] float StabilizeRate(float frame_seconds, float stabilize_ms);

}  // namespace uplift::look
