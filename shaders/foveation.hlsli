// Foveated NR working grid (separable warp).
//
// The NR model is same-resolution, so a spatially varying working resolution is
// expressed as a warp: the frame is resampled into a smaller working image whose
// density, per axis, is 1 (relative to the uniform working scale) inside the
// centre zone, falls along a smoothstep ramp, and settles at Edge.  The axes are
// independent (corners get Edge x Edge).  The model runs once on the warped
// image with warped guides, and every stage that maps between the working grid
// and the full grid goes through FovWarp / FovUnwarp.
//
// Coordinates are normalized: n in [0, 1] across the full image, v in [0, 1]
// across the working image.  Both extend monotonically past [0, 1] so off-screen
// reprojection stays well defined.
//
// Per-axis packed dword (src/nr/foveation.hpp mirrors it; 0 = identity axis):
//   bits  0..11  a: centre half-extent / screen half-extent, code / 4095
//   bits 12..23  b: fall-off length / screen half-extent, code / 2048
//   bits 24..30  e: edge density, code / 128
//   bit  31      enabled
// gmax is passed beside the dword: MakePlan sets it to the axis' realized
// work/uniform ratio, which is what keeps the plateau an exact 1:1 copy, so no
// shader evaluates G(1) per pixel.
//
// Cost: identity axes return immediately; the plateau and edge segments are
// closed form; only the fall-off band runs the 4-step Newton inverse, and the
// encode footprint skips even that on the plateau, where the warp is affine.

#ifndef UPLIFT_FOVEATION_HLSLI
#define UPLIFT_FOVEATION_HLSLI

struct FovAxis {
  float a;     // plateau half-width
  float b;     // ramp length
  float e;     // edge density
  float k;     // 1 - e
  float gmax;  // the normalizer: the axis' realized work / uniform ratio
};

bool FovAxisOn(uint packed) {
  return (packed & 0x80000000u) != 0u;
}

bool FovAny(uint2 packed) {
  return ((packed.x | packed.y) & 0x80000000u) != 0u;
}

FovAxis FovDecode(uint packed, float gmax) {
  FovAxis p;
  p.a = float(packed & 0xFFFu) * (1.0 / 4095.0);
  p.b = float((packed >> 12) & 0xFFFu) * (1.0 / 2048.0);
  p.e = float((packed >> 24) & 0x7Fu) * (1.0 / 128.0);
  p.k = 1.0 - p.e;
  p.gmax = gmax;
  return p;
}

// G(tau): integral of the density over [0, tau], tau = |2n - 1|.  The ramp may
// run past tau = 1 (truncated by the screen edge); G stays monotone.
float FovG(float tau, FovAxis p) {
  [branch] if (tau <= p.a) return tau;
  const float ramp_end = p.a + p.b;
  const float ramp_area = p.b * (1.0 - 0.5 * p.k);
  [branch] if (tau >= ramp_end) return p.a + ramp_area + p.e * (tau - ramp_end);
  const float q = (tau - p.a) / p.b;
  const float q2 = q * q;
  return p.a + p.b * (q - p.k * (q2 * q - 0.5 * q2 * q2));
}

// Inverse of FovG.  The ramp segment h(q) = q - k(q^3 - q^4/2) has
// h'(q) = 1 - k * smoothstep(q) >= Edge >= 0.25, so Newton from q = r reaches
// fp32 precision in four steps (measured: <0.003 px at 7680 px, Edge 0.25).
float FovGInv(float g, FovAxis p) {
  [branch] if (g <= p.a) return g;
  const float ramp_area = p.b * (1.0 - 0.5 * p.k);
  [branch] if (g >= p.a + ramp_area) return p.a + p.b + (g - p.a - ramp_area) / p.e;
  const float r = (g - p.a) / p.b;
  float q = r;
  [unroll]
  for (int i = 0; i < 4; ++i) {
    const float q2 = q * q;
    const float h = q - p.k * (q2 * q - 0.5 * q2 * q2) - r;
    const float dh = 1.0 - p.k * (3.0 * q2 - 2.0 * q2 * q);
    q = saturate(q - h / dh);
  }
  return p.a + p.b * q;
}

// One axis: full normalized -> working normalized, and back.
float FovWarp1(float n, uint packed, float gmax) {
  [branch] if (!FovAxisOn(packed)) return n;
  const FovAxis p = FovDecode(packed, gmax);
  const float t = 2.0 * n - 1.0;
  return 0.5 + 0.5 * sign(t) * FovG(abs(t), p) / p.gmax;
}

// One axis, already decoded: working normalized -> full normalized.  Callers that hold the decoded axis (the
// footprint, which needs it four times) decode once.
float FovUnwarpAxis1(float v, FovAxis p) {
  const float s = 2.0 * v - 1.0;
  return 0.5 + 0.5 * sign(s) * FovGInv(abs(s) * p.gmax, p);
}

// Whether working position `v` is inside the axis' full-density plateau, where the warp is affine.  FovGInv's
// own first test, without running the inverse: on the plateau FovGInv is the identity, so the warp is
// x = 0.5 + (v - 0.5) * gmax.
bool FovPlateau1(float v, FovAxis p) {
  return abs(2.0 * v - 1.0) * p.gmax <= p.a;
}

float FovUnwarp1(float v, uint packed, float gmax) {
  [branch] if (!FovAxisOn(packed)) return v;
  return FovUnwarpAxis1(v, FovDecode(packed, gmax));
}

float2 FovWarp(float2 n, uint2 packed, float2 gmax) {
  return float2(FovWarp1(n.x, packed.x, gmax.x), FovWarp1(n.y, packed.y, gmax.y));
}

float2 FovUnwarp(float2 v, uint2 packed, float2 gmax) {
  return float2(FovUnwarp1(v.x, packed.x, gmax.x), FovUnwarp1(v.y, packed.y, gmax.y));
}

// Full-grid rectangle [lo, hi) covered by working texel `pixel` (`work` texels
// over `full`): the encode footprint, exact under the warp.
void FovWorkFootprint(uint2 pixel, uint2 work, uint2 full, uint2 packed, float2 gmax, inout float2 lo,
                      inout float2 hi) {
  [branch] if (FovAxisOn(packed.x) && work.x != full.x) {
    const FovAxis p = FovDecode(packed.x, gmax.x);
    const float v0 = float(pixel.x) / float(work.x);
    const float v1 = float(pixel.x + 1u) / float(work.x);
    if (FovPlateau1(v0, p) && FovPlateau1(v1, p)) {
      // Both edges are on the plateau, where the warp is affine: two multiplies instead of two four-step
      // Newton inverses.  At Full resolution this is most of the frame.
      lo.x = float(full.x) * (0.5 + (v0 - 0.5) * p.gmax);
      hi.x = float(full.x) * (0.5 + (v1 - 0.5) * p.gmax);
    } else {
      lo.x = FovUnwarpAxis1(v0, p) * float(full.x);
      hi.x = FovUnwarpAxis1(v1, p) * float(full.x);
    }
  }
  [branch] if (FovAxisOn(packed.y) && work.y != full.y) {
    const FovAxis p = FovDecode(packed.y, gmax.y);
    const float v0 = float(pixel.y) / float(work.y);
    const float v1 = float(pixel.y + 1u) / float(work.y);
    if (FovPlateau1(v0, p) && FovPlateau1(v1, p)) {
      lo.y = float(full.y) * (0.5 + (v0 - 0.5) * p.gmax);
      hi.y = float(full.y) * (0.5 + (v1 - 0.5) * p.gmax);
    } else {
      lo.y = FovUnwarpAxis1(v0, p) * float(full.y);
      hi.y = FovUnwarpAxis1(v1, p) * float(full.y);
    }
  }
  lo = max(lo, 0.0);
  hi = min(hi, float2(full));
}

// One axis' working position and the grid's density there, from one decode of the dword and one evaluation of
// the plateau/ramp/edge branch: the branch's own slope is the density (1 on the plateau, 1/Edge past the ramp,
// 1/h'(q) in it).  FovGInv would want four Newton steps per axis, and the decode's kernel would want it per
// tap, so the closed form is what makes the density affordable at all.
struct FovSample {
  float position;  // continuous texel position on the dst grid
  // The warp's slope here, inverted: texels of the axis' own (pre-warp) grid per dst texel, at least 1 -- 1 on
  // the plateau and 1 / Edge past the ramp.  This is only the caller's upsampling when the dst grid is that
  // pre-warp grid too; against any other, multiply by the uniform scale (decode's ratio times the warp's mean).
  float density;
};

// FovFullToWorkPos and the density for one axis.  An axis whose grids match is left alone, as there.
FovSample FovFullToWorkSample1(uint pixel, uint src, uint dst, uint packed, float gmax) {
  FovSample sample;
  const float n = (float(pixel) + 0.5) / float(src);
  sample.position = (float(pixel) + 0.5) * float(dst) / float(src) - 0.5;
  sample.density = 1.f;
  [branch] if (FovAxisOn(packed) && src != dst) {
    const FovAxis p = FovDecode(packed, gmax);
    const float t = 2.0 * n - 1.0;
    const float tau = abs(t);
    const float ramp_end = p.a + p.b;
    float g;
    [branch] if (tau <= p.a) {
      g = tau;
    } else if (tau >= ramp_end) {
      g = p.a + p.b * (1.0 - 0.5 * p.k) + p.e * (tau - ramp_end);
      sample.density = 1.f / max(p.e, 0.25f);  // the edge density's floor is what FovGInv assumes
    } else {
      const float q = (tau - p.a) / p.b;
      const float q2 = q * q;
      g = p.a + p.b * (q - p.k * (q2 * q - 0.5 * q2 * q2));
      sample.density = 1.f / max(1.f - p.k * (3.f * q2 - 2.f * q2 * q), 0.25f);
    }
    sample.position = (0.5 + 0.5 * sign(t) * g / p.gmax) * float(dst) - 0.5;
  }
  return sample;
}

// Continuous texel position on a `dst` grid (working) of the full-grid pixel centre `pixel` (src grid), for the
// caller that does not want the density.  One definition of the map -- the sample's -- so the Classic read and
// the edge-aware one cannot drift apart; the density computes away here.
float2 FovFullToWorkPos(uint2 pixel, uint2 src, uint2 dst, uint2 packed, float2 gmax) {
  return float2(FovFullToWorkSample1(pixel.x, src.x, dst.x, packed.x, gmax.x).position,
                FovFullToWorkSample1(pixel.y, src.y, dst.y, packed.y, gmax.y).position);
}

#endif  // UPLIFT_FOVEATION_HLSLI
