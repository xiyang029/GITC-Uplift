// Foveated NR working grid - host side.  Mirrors shaders/foveation.hlsli: the
// packed per-axis dword, the density integral G and the working-size factor.
// Change the two together.
//
// The NR model is same-resolution, so "foveated" means a separable warp of the
// working image: per axis, density 1 inside the centre zone, a smoothstep
// fall-off over that axis' `falloff` pixels, and `edge` beyond it.  Centre W x H
// and both fall-offs are given in WORKING-GRID pixels -- the same grid the warp is applied
// to -- and clamped against it each frame, so an axis whose centre covers the
// whole grid is simply not warped.  The player's sliders are shares of the screen: FromPercent turns
// them into output pixels and MapToWork carries those across, so the centre keeps its share of the screen
// when the Resolution mode shrinks the working grid.
//
// One NR evaluate, one temporal history, no tile seams.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace uplift::nr::foveation {

inline constexpr uint32_t kEnabledBit = 0x80000000u;

// Field layout of one axis dword (foveation.hlsli FovDecode):
//   bits  0..11  a = centre half-extent / half-extent of the screen, code/4095
//   bits 12..23  b = fall-off length / half-extent, code/2048 (0..2)
//   bits 24..30  e = edge density, code/128 (32..127 -> 0.25..0.99)
//   bit  31      axis enabled
inline constexpr uint32_t kCenterMax = 4095u;
inline constexpr double kCenterScale = 4095.0;
inline constexpr uint32_t kRampMax = 4095u;
inline constexpr double kRampScale = 2048.0;
inline constexpr uint32_t kEdgeMin = 32u;
inline constexpr uint32_t kEdgeMax = 127u;
inline constexpr double kEdgeScale = 128.0;

inline constexpr float kMinEdge = 0.25f;
inline constexpr float kMaxEdge = 1.f;

// Below this saving the guide resample and the warped mappings cost more than
// the NR pixels they remove: the grid stands down to uniform.
inline constexpr double kMinSaving = 0.05;

// shaders/encode_cs.hlsl's MAX_TAPS: the widest footprint is one source pixel over the coarsest work texel, which is
// kMinEdge times the smallest Resolution = Custom (25 %, settings_schema.cpp's resolution_scale rows), plus one for a
// fractional phase. A loop bound below that silently averages a corner of the footprint, so the two move together.
inline constexpr double kMinWorkScale = 0.25;
static_assert(1.0 / (static_cast<double>(kMinEdge) * kMinWorkScale) + 1.0 <= 17.0,
              "encode_cs.hlsl's MAX_TAPS no longer covers the widest footprint: raise it with kMinEdge or the Custom floor");

// The foveation geometry in output pixels: FromPercent builds it from the player's sliders,
// MapToWork carries it to the working grid.
struct Params {
  uint32_t center_width;
  uint32_t center_height;
  uint32_t falloff_x;  // the fall-off on the left/right sides
  uint32_t falloff_y;  // the fall-off on the top/bottom sides
  float edge;          // density beyond the fall-off, per axis
};

// The player's sliders, whole per cent of the screen: the centre's width as % of the output's width, its height and both
// fall-offs as % of the output's height.  Returns the output-pixel Params MapToWork takes; 100 % is the screen itself.
// The two fall-offs are separate because the vertical one is the one a screen's aspect cuts short: shortening it alone
// lets the vertical ramp reach the edge density.  Called at the two places that know the output's size.
inline Params FromPercent(uint32_t width_pct, uint32_t height_pct, uint32_t falloff_x_pct, uint32_t falloff_y_pct,
                          uint32_t edge_pct, uint32_t out_width, uint32_t out_height) {
  const auto pixels = [](uint32_t pct, uint32_t full) {
    return (std::clamp(pct, 0u, 100u) * full + 50u) / 100u;  // rounded to the nearest pixel
  };
  return {pixels(width_pct, out_width), pixels(height_pct, out_height), pixels(falloff_x_pct, out_height),
          pixels(falloff_y_pct, out_height), static_cast<float>(edge_pct) * 0.01f};
}

// The player's output-pixel parameters on a working grid of work_width x
// work_height.  Called once per frame before MakeGrid, so the two never
// disagree about which grid the numbers describe.
inline Params MapToWork(const Params& params, uint32_t out_width, uint32_t out_height, uint32_t work_width, uint32_t work_height) {
  const double sx = (out_width == 0u ? 1.0 : static_cast<double>(work_width) / static_cast<double>(out_width));
  const double sy = (out_height == 0u ? 1.0 : static_cast<double>(work_height) / static_cast<double>(out_height));
  Params work = params;
  work.center_width = static_cast<uint32_t>(std::lround(params.center_width * sx));
  work.center_height = static_cast<uint32_t>(std::lround(params.center_height * sy));
  // Each axis carries its own fall-off, so a shorter vertical one is not dragged back to the horizontal's length.
  work.falloff_x = static_cast<uint32_t>(std::lround(params.falloff_x * sx));
  work.falloff_y = static_cast<uint32_t>(std::lround(params.falloff_y * sy));
  return work;
}

// The exact values the shaders decode from one axis dword.
struct Decoded {
  double a = 1.0;
  double b = 0.0;
  double e = 1.0;
  double k = 0.0;
};

// Integral of the density over [0, tau], tau = |2n - 1|.  The fall-off may run
// past the screen edge (tau = 1): then the edge value is never reached and the
// ramp is truncated, keeping its physical length at every resolution.
inline double G(double tau, const Decoded& p) {
  if (tau <= p.a) return tau;
  const double ramp_end = p.a + p.b;
  const double ramp_area = p.b * (1.0 - 0.5 * p.k);
  if (tau >= ramp_end) return p.a + ramp_area + p.e * (tau - ramp_end);
  const double q = (tau - p.a) / p.b;
  const double q2 = q * q;
  return p.a + p.b * (q - p.k * (q2 * q - 0.5 * q2 * q2));
}

inline Decoded Decode(uint32_t packed) {
  Decoded p;
  if ((packed & kEnabledBit) == 0u) return p;
  p.a = static_cast<double>(packed & 0xFFFu) / kCenterScale;
  p.b = static_cast<double>((packed >> 12) & 0xFFFu) / kRampScale;
  p.e = static_cast<double>((packed >> 24) & 0x7Fu) / kEdgeScale;
  p.k = 1.0 - p.e;
  return p;
}

// One axis: the packed dword (0 = identity) and the mean density G(1).  MakeAxis leaves the ideal value, which sizes
// the working grid; MakePlan replaces it with the realized extent / uniform, which is what the shaders must divide
// by.  The two differ by the extent's rounding, and that difference is a phase drift: the plateau is only an exact
// 1:1 copy when the normalizer is the realized ratio.
struct Axis {
  uint32_t packed = 0;
  float gmax = 1.f;
};

inline Axis MakeAxis(uint32_t full, uint32_t center, uint32_t falloff, float edge) {
  Axis axis;
  const uint32_t center_px = std::min(center, full);
  if (full < 16u || center_px >= full) return axis;
  const uint32_t e_code = static_cast<uint32_t>(
      std::clamp<long>(std::lround(std::clamp(edge, kMinEdge, kMaxEdge) * kEdgeScale), static_cast<long>(kEdgeMin),
                       static_cast<long>(kEdgeMax) + 1));
  if (e_code > kEdgeMax) return axis;  // edge 1.0: no warp
  const double half = 0.5 * static_cast<double>(full);
  const uint32_t a_code = static_cast<uint32_t>(
      std::clamp<long>(std::lround(0.5 * center_px / half * kCenterScale), 0, kCenterMax));
  // Fall-off longer than the grid is pointless: cap at the full extent.
  const double ramp_px = std::min<double>(falloff, full);
  const uint32_t b_code = static_cast<uint32_t>(
      std::clamp<long>(std::lround(ramp_px / half * kRampScale), 0, kRampMax));
  axis.packed = kEnabledBit | a_code | (b_code << 12) | (e_code << 24);
  const Decoded p = Decode(axis.packed);
  axis.gmax = static_cast<float>(G(1.0, p));
  return axis;
}

struct Grid {
  Axis x;
  Axis y;
  bool active() const { return x.packed != 0u || y.packed != 0u; }
  // NR pixels over the uniform grid's.
  double PixelFactor() const { return static_cast<double>(x.gmax) * y.gmax; }
  bool operator==(const Grid& o) const { return x.packed == o.x.packed && y.packed == o.y.packed; }
  bool operator!=(const Grid& o) const { return !(*this == o); }
};

// The grid for a working grid of full_width x full_height, whose parameters are
// in that grid's pixels (MapToWork).  An axis whose centre covers it, or whose
// saving cannot pay for the guide resample, stands down to uniform.
inline Grid MakeGrid(bool enabled, const Params& params, uint32_t full_width, uint32_t full_height) {
  Grid grid;
  if (!enabled) return grid;
  grid.x = MakeAxis(full_width, params.center_width, params.falloff_x, params.edge);
  grid.y = MakeAxis(full_height, params.center_height, params.falloff_y, params.edge);
  if (grid.active() && grid.PixelFactor() > 1.0 - kMinSaving) {
    grid = Grid{};
  }
  return grid;
}

// Working extent on one axis: the uniform extent times the mean density, so the
// centre keeps the uniform density exactly.
//
// Its parity matches the uniform extent's, rounding down (never up: the extent must
// stay below the uniform one).  A work texel on the plateau covers exactly one
// uniform texel, and its preimage on the full grid starts at 0.5*(full - work)
// (foveation.hlsli's affine plateau); an odd difference puts that edge half a
// source pixel off, so every full-density texel blends two source pixels 50/50
// instead of copying one.  Full resolution is the common case and the one the
// centre is judged by, so the parity is the fix, not the rounding.
inline uint32_t WarpedExtent(uint32_t uniform, const Axis& axis) {
  if (axis.packed == 0u) return uniform;
  uint32_t extent = std::max<uint32_t>(16u, static_cast<uint32_t>(std::lround(static_cast<double>(uniform) * axis.gmax)));
  if (((extent ^ uniform) & 1u) != 0u && extent > 16u) --extent;
  return extent;
}

// The frame's grid and working extent together, with `gmax` in both axes replaced by the realized
// extent / uniform: the axis dword is already packed from the ideal value, and only the normalizer the shaders
// divide by needs the realized one, so this is the last step that may touch `gmax`.
//
// Without it the plateau's footprint on the full grid is r = full * gmax / work wide, r != 1 by the rounding and
// its phase drifting across the centre -- so the full-density zone blurs instead of copying, and no work texel
// takes the encode's single-tap path.  With it r is 1 to fp32 and the plateau is a 1:1 copy to within 5.3e-5 px.
struct Plan {
  Grid grid;
  uint32_t width = 0;
  uint32_t height = 0;
};

inline Plan MakePlan(bool enabled, const Params& params, uint32_t uniform_width, uint32_t uniform_height) {
  Plan plan;
  plan.grid = MakeGrid(enabled, params, uniform_width, uniform_height);
  plan.width = WarpedExtent(uniform_width, plan.grid.x);
  plan.height = WarpedExtent(uniform_height, plan.grid.y);
  if (plan.grid.x.packed != 0u && uniform_width != 0u) {
    plan.grid.x.gmax = static_cast<float>(plan.width) / static_cast<float>(uniform_width);
  }
  if (plan.grid.y.packed != 0u && uniform_height != 0u) {
    plan.grid.y.gmax = static_cast<float>(plan.height) / static_cast<float>(uniform_height);
  }
  return plan;
}

}  // namespace uplift::nr::foveation
