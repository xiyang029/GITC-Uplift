#ifndef UPLIFT_NR_BOOST_COMMON_HLSLI
#define UPLIFT_NR_BOOST_COMMON_HLSLI

// Half rate (MFG-Unlocker's Boost): the look model. On every frame the network runs, Fit and Solve fit an affine map
// from its input to its output per cell of the canvas and per luminance bin (bilateral guided upsampling, Chen et al.
// 2016), and Hold keeps what that map does not explain. On every frame it skips, Compose accumulates the game's motion
// vectors up to that last network frame, and Carry applies the map to the frame's own input at where the texels were
// then and adds the held detail on top: edges and animation come from the current image, its noise and fine texture
// from the network's own last frame, in its own place.
cbuffer BoostConstants : register(b0) {
  int2 motion_origin;      // the game's motion vectors: this rect of the motion texture
  uint2 motion_size;
  uint2 motion_extent;     // ... and that texture's own size, which the held vectors are kept at
  uint2 extent;            // the canvas the model is fitted on, in texels
  float2 motion_scale;     // MV.Scale x MotionScale: a vector's pixels, in its rect
  float stable_below;      // detail: how far a texel's neighbours may move from where they came, in luma, and keep the
  float unstable_above;    // detail ... and where it has lost all of it. Carry reads nine of them, two texels apart.
  uint cell;               // the grid: cell x cell texels per cell
  uint bins;               // ... and its luminance bins
  uint2 cells;
  float lambda;            // the fit's pull toward the identity plus the mean change
  float mv_stable_below;   // a vector shorter than this many canvas pixels carries the held detail whole
  float mv_unstable_above; // ... and past this many it carries none of it
  float mv_field_above;    // ... and so does a vector that disagrees with itself by this many canvas pixels
  uint compose_reset;      // the accumulated motion starts over here: the network just ran, so there is nothing to add to
};

float BoostLuma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }
float BoostBin(float3 c) { return saturate(BoostLuma(c)) * float(bins - 1u); }
bool BoostInside(float2 q) { return all(isfinite(q)) && all(q >= 0.f) && all(q < float2(extent)); }

// The pixel displacement the game's vectors give extent texel e, in the canvas's own pixels.
float2 BoostMotionAt(uint2 e, Texture2D<float4> motion) {
  const float2 at = (float2(e) + 0.5f) * float2(motion_size) / float2(extent);
  const int2 m = motion_origin + clamp(int2(at), int2(0, 0), int2(motion_size) - 1);
  return motion.Load(int3(m, 0)).xy * motion_scale * float2(extent) / float2(motion_size);
}

// Where extent texel e was in the previous evaluation (pixel centres), from the game's motion vectors.
float2 BoostSource(uint2 e, Texture2D<float4> motion) {
  return float2(e) + 0.5f + BoostMotionAt(e, motion);
}

// The luminance Hold recorded for a position: its alpha channel, point sampled. It is a smooth field and only ever a
// threshold here, so its bilinear neighbours could not change the answer.
float BoostHeldLuma(Texture2D<float4> held, float2 q) {
  const int2 hi = int2(extent) - 1;
  return held.Load(int3(clamp(int2(floor(q)), int2(0, 0), hi), 0)).a;
}

// 0 at or below below, 1 at or above above.
float BoostFade(float d, float below, float above) {
  return saturate((d - below) / max(above - below, 1e-6f));
}

float4 BoostBilinear(Texture2D<float4> t, float2 q) {
  const float2 s = q - 0.5f;
  const int2 i = int2(floor(s));
  const float2 f = s - float2(i);
  const int2 hi = int2(extent) - 1;
  const float4 a = t.Load(int3(clamp(i, int2(0, 0), hi), 0));
  const float4 b = t.Load(int3(clamp(i + int2(1, 0), int2(0, 0), hi), 0));
  const float4 c = t.Load(int3(clamp(i + int2(0, 1), int2(0, 0), hi), 0));
  const float4 d = t.Load(int3(clamp(i + int2(1, 1), int2(0, 0), hi), 0));
  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// The model applied to input x, with the cells around extent position q: bilinear between cells, linear between bins.
float3 BoostLook(Texture2D<float4> model, float3 x, float2 q) {
  const float2 g = clamp(q / float(cell) - 0.5f, 0.f, float2(cells) - 1.f);
  const int2 i0 = int2(floor(g));
  const float2 f = g - float2(i0);
  const int2 i1 = min(i0 + 1, int2(cells) - 1);
  const float z = BoostBin(x);
  const uint z0 = uint(floor(z));
  const uint z1 = min(z0 + 1u, bins - 1u);
  const float fz = z - float(z0);
  float4 m0 = 0.f, m1 = 0.f, m2 = 0.f;
  [unroll] for (uint corner = 0u; corner < 8u; ++corner) {
    const bool bx = (corner & 1u) != 0u, by = (corner & 2u) != 0u, bz = (corner & 4u) != 0u;
    const int2 at = int2(bx ? i1.x : i0.x, by ? i1.y : i0.y);
    const uint zb = bz ? z1 : z0;
    const float w = (bx ? f.x : 1.f - f.x) * (by ? f.y : 1.f - f.y) * (bz ? fz : 1.f - fz);
    const int row = at.y * int(bins) + int(zb);
    m0 += w * model.Load(int3(at.x * 3, row, 0));
    m1 += w * model.Load(int3(at.x * 3 + 1, row, 0));
    m2 += w * model.Load(int3(at.x * 3 + 2, row, 0));
  }
  const float4 x1 = float4(x, 1.f);
  return float3(dot(m0, x1), dot(m1, x1), dot(m2, x1));
}
#endif
