#ifndef UPLIFT_DECODE_COMMON_HLSLI
#define UPLIFT_DECODE_COMMON_HLSLI

#include "change_common.hlsli"
#include "foveation.hlsli"
#include "vk_common.hlsli"

// Shared by decode_ps.hlsl (the Present path, through an RTV) and decode_cs.hlsl (UAV targets).
cbuffer DecodeConstants : register(b0) {
  uint encoding;
  uint target_srgb;    // decode_ps only: the RTV sRGB-encodes, so the shader writes linear values
  float input_scale;
  float transfer_strength;
  float color_strength;
  uint has_exposure;
  float exposure_factor;
  uint origin_x;       // decode_cs only: the region's top-left corner in the target
  uint origin_y;
  uint width;          // the region: the output size
  uint height;
  uint upsampling;     // change-field reads: 0 direct, 1 Classic, 2 Edge-aware, 3 Guided
  uint source_x;       // the region's top-left corner in the source
  uint source_y;
  uint options;        // Plan 5: OPTION_* (the primaries, Consistent transfer, the mask)
  float chroma_clamp;  // Plan 5 (F14): stops; 0 = off
  uint foveation_x;    // foveated grid: the packed per-axis warp (0 = identity) and G(1), its mean density
  uint foveation_y;
  float foveation_gmax_x;
  float foveation_gmax_y;
  float guided_low_gain;   // Guided: the base band's gain (1 = the field's low frequencies unchanged)
  float guided_high_gain;  // Guided: the detail band's gain (1 = the field's high frequencies unchanged)
};

// Plan 13: on Vulkan's After DLSS the decode works in place: the target (DLSS's output, u0) is also where the original pixel is read.
#if UPLIFT_VK_SOURCE_STORAGE
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> target_texture : register(u0);
#define UPLIFT_LOAD_SOURCE(pixel) target_texture[uint2(pixel)]
#else
Texture2D<float4> source_texture : register(t0);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture.Load(int3((pixel), 0))
#endif
Texture2D<float4> change_texture : register(t1);
Texture2D<float> exposure_texture : register(t2);
Texture2D<float4> mask_texture : register(t3);  // Plan 5 (§3.13): Uplift's copy of UPLIFT_MASK, read with OPTION_MASK

static const float SPATIAL_RADIUS = 1.5f;  // the tent's half-width, in work pixels (v2 design §3.8)
static const float RANGE_SIGMA = 0.5f;     // the range kernel's width, in stops
static const float MIN_WEIGHT = 1e-4f;     // below this total the edge-aware sample falls back to bilinear

// Band shaping (2026-10-26). The upsample's two bands are shaped before they are combined: the detail band
// by how coarse the working grid is here, both bands by how dark the pixel is. Unguided (Upsampling 2) the
// band gains are 1, so only the shaping applies; Guided (3) adds its own gains on top.
static const float EDGE_DENSITY_ATTENUATION = 0.95f;  // detail gain per unit of extra work-grid density
static const float DARK_EDIT_FLOOR = 0.80f;           // the edit's weight in the deepest shadows
static const float DARK_GUIDE_LOW = -7.f;             // log2 stops below diffuse white: fully dark
static const float DARK_GUIDE_HIGH = -3.5f;           // and fully lit
static const float CLAMP_SLACK = 0.15f;               // how far past the 4x4's own range the shaped bands may reach

// Classic: a bilinear sample of the change field at `position` (work pixels), clamped to its texels.
float3 ChangeBilinear(float2 position, uint2 size) {
  const float2 clamped = clamp(position, float2(0.f, 0.f), float2(size) - 1.f);
  const int2 first = int2(floor(clamped));
  const int2 last = min(first + 1, int2(size) - 1);
  const float2 fraction = clamped - float2(first);
  const float3 top = lerp(change_texture.Load(int3(first, 0)).xyz, change_texture.Load(int3(last.x, first.y, 0)).xyz, fraction.x);
  const float3 bottom = lerp(change_texture.Load(int3(first.x, last.y, 0)).xyz, change_texture.Load(int3(last, 0)).xyz, fraction.x);
  return lerp(top, bottom, fraction.y);
}

// Edge-aware: v2 design §3.8's joint bilateral upsample over the 4x4 work pixels around `position`:
// a tent in space, times how close each pixel's guide ℓ_M is to this output pixel's `guide`. It also
// reports those same 4x4 taps' own value range, which the caller holds the shaped bands inside, and the
// bilinear answer over that neighbourhood -- Classic reads the same four texels this loop already loads,
// so the caller gets both samples out of one pass, at no extra texture traffic.
float3 ChangeEdgeAware(float2 position, uint2 size, float guide, out float3 tap_min, out float3 tap_max,
                       out float3 classic) {
  const int2 base = int2(floor(position));
  const float2 fraction = position - float2(base);
  float3 sum = float3(0.f, 0.f, 0.f);
  classic = float3(0.f, 0.f, 0.f);
  float total = 0.f;
  tap_min = float3(1e30f, 1e30f, 1e30f);
  tap_max = float3(-1e30f, -1e30f, -1e30f);
  [unroll] for (int y = -1; y <= 2; ++y) {
    [unroll] for (int x = -1; x <= 2; ++x) {
      const int2 tap = base + int2(x, y);
      const float2 gap = abs(position - float2(tap));  // not `distance`: an HLSL intrinsic
      const float spatial = max(1.f - gap.x / SPATIAL_RADIUS, 0.f) * max(1.f - gap.y / SPATIAL_RADIUS, 0.f);
      const float4 change = change_texture.Load(int3(clamp(tap, int2(0, 0), int2(size) - 1), 0));
      // The centre 2x2 is what ChangeBilinear samples, at the same clamped coordinates and the same
      // fraction, so Classic costs no extra load. x and y are unrolled constants: four mul-adds, no branch.
      if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
        const float wx = (x == 0) ? (1.f - fraction.x) : fraction.x;
        const float wy = (y == 0) ? (1.f - fraction.y) : fraction.y;
        classic += change.xyz * (wx * wy);
      }
      if (!all(isfinite(change))) continue;
      // The range comes from every finite tap, the spatial zeroes at the corners included: it is a
      // statement about the neighbourhood, not about this kernel's weighting.
      tap_min = min(tap_min, change.xyz);
      tap_max = max(tap_max, change.xyz);
      const float difference = guide - change.w;
      const float weight = spatial * exp(-difference * difference / (2.f * RANGE_SIGMA * RANGE_SIGMA));
      if (weight > 0.f) {
        sum += weight * change.xyz;
        total += weight;
      }
    }
  }
  if (total < MIN_WEIGHT) {
    // Nothing to average, so the caller has no range to hold on to either: hand it an open one rather
    // than the sentinels, which would clamp every channel to 1e30.
    tap_min = float3(-3.4e38f, -3.4e38f, -3.4e38f);
    tap_max = float3(3.4e38f, 3.4e38f, 3.4e38f);
    return classic;  // the bilinear sample this loop has already built
  }
  return sum / total;
}

// How much coarser the working grid is at `n` than at the foveated plateau, one axis: 1 where the axis
// keeps NR's full density, up to 1/Edge past the ramp. The warp's slope is closed form (foveation.hlsli's
// FovG: 1 on the plateau, Edge past the ramp, h'(q) = 1 - k*smoothstep(q) in the ramp), so this is a few
// ALU per output pixel -- FovGInv would be four Newton steps per axis, and the kernel would want it per tap.
float FovDensity1(float n, uint packed, float gmax) {
  if (!FovAxisOn(packed)) return 1.f;
  const FovAxis p = FovDecode(packed, gmax);
  const float tau = abs(2.f * n - 1.f);
  [branch] if (tau <= p.a) return 1.f;
  [branch] if (tau >= p.a + p.b) return 1.f / max(p.e, 0.25f);  // FovGInv assumes an edge density >= 0.25
  const float q = (tau - p.a) / p.b;
  return 1.f / max(1.f - p.k * (3.f * q * q - 2.f * q * q * q), 0.25f);
}

// The axes are independent, so a point sees their geometric mean: the corners get Edge x Edge.
float FovDensity(float2 n, uint2 packed, float2 gmax) {
  return sqrt(FovDensity1(n.x, packed.x, gmax.x) * FovDensity1(n.y, packed.y, gmax.y));
}

// The upsample's two bands, shaped and combined. `classic` is the change field's low frequencies (the
// bilinear sample); `edge` is the joint bilateral answer, so edge - classic is the detail the guided kernel
// holds on the correct side of an edge. The gains come from the caller: 1 and 1 unguided, the Guided
// settings otherwise (its detail gain reads 1 under the foveated grid, as it always has).
//
// Density: a coarser working grid means a work texel carries detail its footprint cannot resolve, so the
// detail band is attenuated per unit of extra density. At the plateau, and with the grid off, it is 1.
//
// Dark: shadows carry most of NR's per-frame churn and least of its real detail, and that churn is
// broadband -- attenuating one band alone would only move it to the other, so both bands scale together.
//
// Clamp: classic and edge are both convex combinations of the same taps, so neither of them overshoots on
// its own, and unguided (where the detail gain never exceeds the classic gain) the mix cannot either. It
// is Guided's extrapolation that leaves the neighbourhood -- its default detail gain 0.950 against base
// 0.850 -- and a step in the edit field is where that shows, so the mix is held to the 4x4 it was built
// from, plus CLAMP_SLACK so the peaks that extrapolation exists for still land. Dark multiplies after the
// clamp, so the shadow attenuation is never undone by it.
float3 ChangeBands(float3 classic, float3 edge, float3 tap_min, float3 tap_max, float low_gain, float high_gain,
                   float density, float guide) {
  const float dark = DARK_EDIT_FLOOR + (1.f - DARK_EDIT_FLOOR) * smoothstep(DARK_GUIDE_LOW, DARK_GUIDE_HIGH, guide);
  const float detail = high_gain * pow(EDGE_DENSITY_ATTENUATION, max(density - 1.f, 0.f));
  const float3 bands = classic * low_gain + (edge - classic) * detail;
  const float3 range = tap_max - tap_min;
  return dark * clamp(bands, tap_min - range * CLAMP_SLACK, tap_max + range * CLAMP_SLACK);
}

// The mask's R channel, bilinear at this output pixel whatever the mask's size, clamped to [0, 1]; 1 when not finite.
float MaskAt(uint2 pixel) {
  uint2 size;
  mask_texture.GetDimensions(size.x, size.y);
  const float2 position = (float2(pixel) + 0.5f) * (float2(size) / float2(width, height)) - 0.5f;
  const float2 clamped = clamp(position, float2(0.f, 0.f), float2(size) - 1.f);
  const int2 first = int2(floor(clamped));
  const int2 last = min(first + 1, int2(size) - 1);
  const float2 fraction = clamped - float2(first);
  const float top = lerp(mask_texture.Load(int3(first, 0)).r, mask_texture.Load(int3(last.x, first.y, 0)).r, fraction.x);
  const float bottom = lerp(mask_texture.Load(int3(first.x, last.y, 0)).r, mask_texture.Load(int3(last, 0)).r, fraction.x);
  const float value = lerp(top, bottom, fraction.y);
  return (isfinite(value) ? saturate(value) : 1.f);
}

// Region pixel `pixel` (0-based) in the stored encoding, with the source's alpha.
float4 DecodePixel(uint2 pixel) {
  const float4 source = UPLIFT_LOAD_SOURCE(uint2(source_x, source_y) + pixel);
  precise float scale = input_scale * GameExposure(has_exposure, exposure_texture.Load(int3(0, 0, 0)), exposure_factor);
  const uint primaries = options & OPTION_PRIMARIES_MASK;
  float3 change;
  if (upsampling == 0u) {
    change = change_texture.Load(int3(pixel, 0)).xyz;
  } else {
    uint2 size;
    change_texture.GetDimensions(size.x, size.y);
    const float2 position = FovFullToWorkPos(pixel, uint2(width, height), size, uint2(foveation_x, foveation_y),
                                             float2(foveation_gmax_x, foveation_gmax_y));
    if (upsampling == 2u || upsampling == 3u) {
      const float3 model = EncodeModel(encoding, primaries, source.rgb, scale);
      const float guide = log2(max(Luminance(SrgbDecode(model)), 0.f) + ChangeEpsilon(encoding));
      // The Guided settings reach the bands here; Edge-aware passes both at full weight and everything it
      // gets is the shaping below.
      const bool guided = upsampling == 3u;
      const bool foveated = FovAny(uint2(foveation_x, foveation_y));
      // `pixel` is the region's, as FovFullToWorkPos's own normalization is: the density is about where
      // this output pixel sits in the region, not on the screen.
      const float2 normalized = (float2(pixel) + 0.5f) / float2(width, height);
      const float density = FovDensity(normalized, uint2(foveation_x, foveation_y), float2(foveation_gmax_x, foveation_gmax_y));
      float3 tap_min, tap_max, classic;
      const float3 edge = ChangeEdgeAware(position, size, guide, tap_min, tap_max, classic);
      change = ChangeBands(classic, edge, tap_min, tap_max,
                           guided ? guided_low_gain : 1.f, (guided && !foveated) ? guided_high_gain : 1.f, density, guide);
    } else {
      change = ChangeBilinear(position, size);
    }
  }
  // v2 design §3.13, step 11: the mask scales the final change at this pixel, so 0 is exactly the original.
  if ((options & OPTION_MASK) != 0u) {
    change *= MaskAt(pixel);
  }
  return float4(Compose(encoding, primaries, source.rgb, change, scale, transfer_strength, color_strength, options, chroma_clamp),
                source.a);
}

#endif  // UPLIFT_DECODE_COMMON_HLSLI
