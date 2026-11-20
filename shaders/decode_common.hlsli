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
  uint origin_x;       // the region's top-left corner in the target (the Present path packs 0: its region is the target)
  uint origin_y;
  uint target_width;   // the target's size: the mask covers it
  uint target_height;
  uint width;          // the region: the output size
  uint height;
  uint upsampling;     // change-field reads: 0 direct, 1 Classic, 2 Edge-aware
  uint source_x;       // the region's top-left corner in the source
  uint source_y;
  uint options;        // OPTION_* (the primaries, Consistent transfer, the mask)
  float chroma_clamp;  // stops; 0 = off
  uint foveation_x;    // foveated grid: the packed per-axis warp (0 = identity) and its realized density
  uint foveation_y;
  float foveation_gmax_x;
  float foveation_gmax_y;
  float guided_low_gain;   // Edge-aware: the base band's gain (1 = the field's low frequencies unchanged)
};

// On Vulkan's After DLSS the decode works in place: the target (DLSS's output, u0) is also where the original pixel is read.
#if UPLIFT_VK_SOURCE_STORAGE
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> target_texture : register(u0);
#define UPLIFT_LOAD_SOURCE(pixel) target_texture[uint2(pixel)]
#else
Texture2D<float4> source_texture : register(t0);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture.Load(int3((pixel), 0))
#endif
Texture2D<float4> change_texture : register(t1);
Texture2D<float> exposure_texture : register(t2);
Texture2D<float4> mask_texture : register(t3);  // Uplift's copy of UPLIFT_MASK, read with OPTION_MASK

static const float SPATIAL_RADIUS = 1.5f;      // the tent's half-width, in work texels
static const float SPATIAL_INV_RADIUS = 1.f / SPATIAL_RADIUS;  // so the tent's weight is a multiply, not a divide
static const float MIN_WEIGHT = 1e-4f;         // below this total use the bilinear sample
static const float GUIDE_FULL_SUPPORT = 0.25f;  // total weight needed to trust the guided sample fully
static const float FOV_GUIDE_FULL_DENSITY = 1.125f;  // use the output guide fully at ~89% of unwarped density

// Band shaping (2026-10-26; density scale 2026-10-27). The upsample's two bands are shaped before they are
// combined: the detail band by how coarse the working grid is here, both bands by how dark the pixel is.
// Edge-aware (Upsampling 2) sets the base band's gain, which is a ceiling on the shaping rather than a factor.
//
// `density` covers the uniform work scale as well as the foveated grid: a Custom resolution below Full is
// exactly as coarse as the foveated edges are, so leaving it out left the shaping inert for the common case.
// The fade-in rate below is the only shaping the density drives now that the detail gain and the coarse-grid
// attenuation are gone (2026-11-04): both read as no-ops in practice, and a control that does nothing is worse
// than none.
static const float DARK_EDIT_FLOOR = 0.80f;           // the edit's weight in the deepest shadows
static const float DARK_GUIDE_LOW = -7.f;             // log2 stops below diffuse white: fully dark
static const float DARK_GUIDE_HIGH = -3.5f;           // and fully lit
static const float CLAMP_SLACK = 0.15f;               // how far past the neighbourhood's own range the shaped bands may reach
// -1 / (2 * sigma^2 * ln 2) for the range kernel's sigma of 0.5 stops at density 1: the exponent per stop^2.  The
// caller divides it by the density, which widens the Gaussian as sqrt(density).
static const float RANGE_K = -2.885390082f;
// Kernel fade: 1 / the density excess at which the correction reaches full strength; 4 puts that at density 1.25
// (an 80 % work grid). It was a UI row for a day (2026-10-29) and is a constant again: the fade saturates by
// 80 % whatever the rate, so the row only paced the 90-85 % steps, which did not earn a control. See
// ChangeBands: at density 1 the field is 1:1 with the output and the kernel is pure blur, so the
// correction has to fade out there or the plateau's one-load path steps against the ramp's first texels. 10 was
// tried and reverted: ghosting and flicker got markedly worse.
static const float KERNEL_FADE_IN = 4.f;

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

// One tap's spatial weight: a tent around `position`, its half-width SPATIAL_RADIUS work texels on both axes.
//
// It was per axis from 2026-10-29 to 2026-11-05, dividing SPATIAL_RADIUS by each axis' own upsampling so a Custom
// resolution that scales width and height apart kept the support round in output space. The floor added under
// that quotient on 2026-11-05 made it constant: the per-axis upsampling is density / scale with scale <= 1 and
// density >= 1, so it is never below one, and max(SPATIAL_RADIUS / upsampling, SPATIAL_RADIUS) is SPATIAL_RADIUS
// at every scale. No caller carries a radius any more, and each axis' weight is one multiply instead of a divide.
float TentWeight(float2 position, float2 tap) {
  const float2 gap = abs(position - tap);  // not `distance`: an HLSL intrinsic
  return max(1.f - gap.x * SPATIAL_INV_RADIUS, 0.f) * max(1.f - gap.y * SPATIAL_INV_RADIUS, 0.f);
}

// The change field at `tap`, clamped to the field's texels; the position a caller weights it by stays the
// unclamped one, as the tap grid is the one `position` was resolved against.
float4 ChangeTap(int2 tap, uint2 size) {
  return change_texture.Load(int3(clamp(tap, int2(0, 0), int2(size) - 1), 0));
}

// One tap into the joint bilateral: the tent, times how close the tap's guide is to this pixel's, and the
// tap's own value into the neighbourhood's min/max when the caller wants a range.
//
// The range is exp2(d^2 * range_k), the plateau's Gaussian widened by the grid's density: a coarse grid
// averages a few taps instead of snapping to the nearest one, which is what stepped at texel boundaries.
//
// The weight is accumulated unconditionally: the tent and exp2 are both non-negative, so a zero-weight tap
// adds zero, and a branch per tap (which the guard it replaced emitted) costs more than the six multiplies
// it skipped. Non-finite taps are the exception a branchless add cannot carry -- 0 * Inf is NaN, which would
// poison the sum and hand the pixel back to Compose's finite guard as an untouched source -- so they select
// a zero weight and a zero value instead, and stay out of the hull. The change field's own writers sanitise
// (ChangeField, ResolveStrengths), so this only guards a field read before it is written.
void AccumulateTap(float4 change, float2 position, float2 tap, float guide, float range_k, bool want_range,
                   inout float3 sum, inout float total, inout float3 tap_min, inout float3 tap_max) {
  const bool finite = all(isfinite(change));
  if (want_range && finite) {
    tap_min = min(tap_min, change.xyz);
    tap_max = max(tap_max, change.xyz);
  }
  const float difference = guide - change.w;
  const float weight = finite ? TentWeight(position, tap) * exp2(difference * difference * range_k) : 0.f;
  sum += weight * (finite ? change.xyz : float3(0.f, 0.f, 0.f));
  total += weight;
}

// The centre 2x2 supplies the Classic sample and field guide. In the foveated ramp, blend toward
// the output guide so the coarser field does not mix across moving edges. Load additional taps for
// the tent or, when enabled, for the clamp's full neighbourhood range.
float3 ChangeEdgeAware(float2 position, uint2 size, float density, float output_guide, float guide_mix,
                       bool want_range, out float3 tap_min, out float3 tap_max, out float3 classic) {
  const int2 base = int2(floor(position));
  const float2 fraction = position - float2(base);
  const float range_k = RANGE_K / density;
  // The tent's half-width is SPATIAL_RADIUS at every scale: the per-axis radius the 2026-10-29 kernel carried
  // became constant once 2026-11-05 floored it (see TentWeight), so it is not computed here any more. The floor
  // is still the design: a tent one texel wide is the bilinear cell, which put `edge` equal to `classic` and
  // silently turned edge-aware into Classic wherever the grid was coarser than 67 %.

  const float4 c00 = ChangeTap(base, size);
  const float4 c10 = ChangeTap(base + int2(1, 0), size);
  const float4 c01 = ChangeTap(base + int2(0, 1), size);
  const float4 c11 = ChangeTap(base + int2(1, 1), size);
  const float w00 = (1.f - fraction.x) * (1.f - fraction.y);
  const float w10 = fraction.x * (1.f - fraction.y);
  const float w01 = (1.f - fraction.x) * fraction.y;
  const float w11 = fraction.x * fraction.y;
  const float4 bilinear = c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11;
  classic = bilinear.xyz;
  const float guide = lerp(bilinear.w, output_guide, guide_mix);

  float3 sum = float3(0.f, 0.f, 0.f);
  float total = 0.f;
  tap_min = float3(1e30f, 1e30f, 1e30f);
  tap_max = float3(-1e30f, -1e30f, -1e30f);
  AccumulateTap(c00, position, float2(base), guide, range_k, want_range, sum, total, tap_min, tap_max);
  AccumulateTap(c10, position, float2(base + int2(1, 0)), guide, range_k, want_range, sum, total, tap_min, tap_max);
  AccumulateTap(c01, position, float2(base + int2(0, 1)), guide, range_k, want_range, sum, total, tap_min, tap_max);
  AccumulateTap(c11, position, float2(base + int2(1, 1)), guide, range_k, want_range, sum, total, tap_min, tap_max);

  const int2 outer = int2(fraction.x < 0.5f ? -1 : 2, fraction.y < 0.5f ? -1 : 2);
  const int2 ring[4] = {int2(outer.x, 0), int2(outer.x, 1), int2(0, outer.y), int2(1, outer.y)};
  [unroll] for (int i = 0; i < 4; ++i) {
    const int2 tap = base + ring[i];
    AccumulateTap(ChangeTap(tap, size), position, float2(tap), guide, range_k, want_range, sum, total, tap_min,
                  tap_max);
  }
  const int2 corner = base + outer;
  AccumulateTap(ChangeTap(corner, size), position, float2(corner), guide, range_k, want_range, sum, total, tap_min,
                tap_max);

  if (want_range) {
    // The hull's four taps the weights left at zero, min/max only.
    const int2 opposite = int2(fraction.x < 0.5f ? 2 : -1, fraction.y < 0.5f ? 2 : -1);
    const int2 far[4] = {int2(opposite.x, 0), int2(opposite.x, 1), int2(0, opposite.y), int2(1, opposite.y)};
    [unroll] for (int i = 0; i < 4; ++i) {
      const float4 change = ChangeTap(base + far[i], size);
      tap_min = min(tap_min, change.xyz);
      tap_max = max(tap_max, change.xyz);
    }
  }

  if (total <= MIN_WEIGHT) {
    // Keep the range open but finite so the band clamp does not overflow.
    tap_min = float3(-1e30f, -1e30f, -1e30f);
    tap_max = float3(1e30f, 1e30f, 1e30f);
    return classic;
  }
  // Fade back to Classic when the output guide has too few matching coarse taps.
  const float full_support = max(2.f * MIN_WEIGHT, lerp(MIN_WEIGHT, GUIDE_FULL_SUPPORT, guide_mix));
  return lerp(classic, sum / total, smoothstep(MIN_WEIGHT, full_support, total));
}

// Both bands' shadow attenuation, from the pixel's own guide: shadows carry most of NR's per-frame churn and
// least of its real detail, and that churn is broadband -- attenuating one band alone would only move it to the
// other, so both bands scale together.
float EditDarkness(float guide) {
  return DARK_EDIT_FLOOR + (1.f - DARK_EDIT_FLOOR) * smoothstep(DARK_GUIDE_LOW, DARK_GUIDE_HIGH, guide);
}

// The same guide ChangeField stores in .w -- log2 of the model domain's linear luminance plus the encoding's
// epsilon -- at the output pixel's own rate: a field coarser than the output has one average per cell, and
// anything that multiplies the change per output pixel needs this.
//
// The model domain is not scene light: on HDR it is the tone-mapped, gamut-clipped encode, which places shadows
// differently, so HDR reads the caller's linear colour through EncodeLinear rather than transforming the stored
// value again. SDR keeps reading the stored value, because the encode there is `saturate` and a round trip through
// SrgbEncode/SrgbDecode is not the same bits. `light` arrives sanitised, which is EncodeModel's finiteness guard.
float ChangeGuide(uint encoding, float3 value, float3 light) {
  const float3 model = (IsSdr(encoding) ? saturate(value) : EncodeLinear(encoding, light));
  return log2(max(Luminance(SrgbDecode(model)), 0.f) + ChangeEpsilon(encoding));
}

// The upsample's two bands, shaped and combined. `classic` is the change field's low frequencies (the
// bilinear sample); `edge` is the joint bilateral answer, so edge - classic is the detail the guided kernel
// holds on the correct side of an edge. Only the base band has a gain (1 by default, from the caller).
//
// The kernel's correction fades in over the same density (2026-10-27): edge - classic is the tent's blur, which
// only earns its keep once the field is actually coarser than the output. At density 1 there is no resample to
// smooth, so the correction is zero and the result is `classic * low_gain` -- exactly what DecodePixel's one-load
// path returns -- and by density 1 + 1 / KERNEL_FADE_IN it is the full kernel.
//
// Clamp: with a base gain at most 1 both bands are convex combinations of the same taps, so the band cannot
// leave the hull upward; the base band alone can sit below it, which is what a base gain under 1 asks for, so
// the floor is the lower of the hull and the base band, less CLAMP_SLACK. A dip rolls off over that slack
// rather than as a plateau that would flicker as a moving step cuts in and out. Dark multiplies after the
// clamp, so the shadow attenuation is never undone by it.
//
// With the default (1) the excursion is zero, so the caller does not hand the hull over at all; the flag is
// uniform, and only a base gain under 0.85 turns it on.
float3 ChangeBands(float3 classic, float3 edge, float3 tap_min, float3 tap_max, float low_gain, float density,
                   bool clamp_bands) {
  const float detail = saturate((density - 1.f) * KERNEL_FADE_IN);
  const float3 bands = classic * low_gain + (edge - classic) * detail;
  if (!clamp_bands) return bands;
  const float3 base = classic * low_gain + (edge - classic) * min(detail, low_gain);
  const float3 slack = max((tap_max - tap_min) * CLAMP_SLACK, 1e-4f);
  const float3 floor_band = min(tap_min, base) - slack;
  const float3 over = max(floor_band - bands, 0.f);
  // Asymptote floor + slack: the allowance a peak that genuinely extrapolates this far may keep.
  return bands + (floor_band + slack - bands) * over / (slack + over);
}

// The mask's R channel, bilinear at this output pixel whatever the mask's size, clamped to [0, 1]; 1 when not finite.
// The mask covers the target, and `pixel` is region-local, so the pixel's centre is (origin + pixel) + 0.5 in target
// texels: an After-DLSS region that is a subrect of the Output reads its own part of the mask, not a stretched whole.
float MaskAt(uint2 pixel) {
  uint2 size;
  mask_texture.GetDimensions(size.x, size.y);
  const uint2 target_position = uint2(origin_x, origin_y) + pixel;
  // A mask that already covers the target one texel per pixel needs no filtering: the bilinear position lands
  // on the texel centre with a zero fraction, so one Load is the same value and three loads cheaper.
  if (size.x == target_width && size.y == target_height) {
    const float value = mask_texture.Load(int3(target_position, 0)).r;
    return (isfinite(value) ? saturate(value) : 1.f);
  }
  const float2 position = (float2(target_position) + 0.5f) * (float2(size) / float2(target_width, target_height)) - 0.5f;
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
  // v2 design §3.13, step 11: the mask scales the final change at this pixel, so 0 is exactly the original.
  // It is read first: Compose's zero-change path returns the source untouched, so a masked-out pixel skips
  // the upsample and the rebuild entirely. OPTION_MASK is uniform, so the branch costs nothing.
  float mask = 1.f;
  if ((options & OPTION_MASK) != 0u) {
    mask = MaskAt(pixel);
    if (mask <= 0.f) return source;
  }
  const uint primaries = options & OPTION_PRIMARIES_MASK;
  // Taken before the compose's zero-change early return: `scale` feeds ChangeGuide's model encode below. The
  // load is uniform, so a pass without an exposure texture still reads nothing.
  float exposure = 1.f;
  if (has_exposure != 0u) {
    exposure = exposure_texture.Load(int3(0, 0, 0));
  }
  precise float scale = input_scale * GameExposure(has_exposure, exposure, exposure_factor);
  float3 change;
  // Edge-aware needs the pixel in scene light for its guide before it knows whether there is a change; the other
  // modes build it only once a change exists, which is what Compose's zero test buys them. The flag is uniform
  // (one mode per pass), so the branch below costs a coherent test rather than a per-pixel choice, and the compose
  // is one copy of the code.
  float3 original = float3(0.f, 0.f, 0.f);
  bool has_original = false;
  if (upsampling == 0u) {
    change = change_texture.Load(int3(pixel, 0)).xyz;
  } else {
    uint2 size;
    change_texture.GetDimensions(size.x, size.y);
    if (upsampling == 2u) {
      // Only a base gain below the clamp margin can need the neighbourhood range.
      const bool hull = 1.f - guided_low_gain > CLAMP_SLACK;
      const FovSample sx = FovFullToWorkSample1(pixel.x, width, size.x, foveation_x, foveation_gmax_x);
      const FovSample sy = FovFullToWorkSample1(pixel.y, height, size.y, foveation_y, foveation_gmax_y);
      // Undo gmax's normalization to get output texels per work texel. Use the coarsest axis.
      const float2 axis = float2(sx.density, sy.density) * (float2(width, height) / float2(size))
                          * float2(foveation_gmax_x, foveation_gmax_y);
      const float density = max(axis.x, axis.y);
      // Reuse the output pixel's scene light for the guide and composition.
      original = SceneLinear(encoding, primaries, source.rgb, scale);
      has_original = true;
      const float output_guide = ChangeGuide(encoding, source.rgb, SanitiseLight(original));
      const float dark = EditDarkness(output_guide);
      if (density <= 1.f + 1e-5f) {
        // Both axes are 1:1, so ChangeBands adds no correction. The warp can still offset the texel.
        const uint2 texel = min(uint2(max(float2(sx.position, sy.position) + 0.5f, 0.f)), size - 1u);
        const float4 value = change_texture.Load(int3(texel, 0));
        change = dark * guided_low_gain * value.xyz;
      } else {
        const float2 position = float2(sx.position, sy.position);
        // Only the warp, not uniform scaling, shifts the guide toward the output sample.
        const float guide_mix = smoothstep(1.f, FOV_GUIDE_FULL_DENSITY, max(sx.density, sy.density));
        float3 tap_min, tap_max;
        float3 classic;
        const float3 edge = ChangeEdgeAware(position, size, density, output_guide, guide_mix, hull, tap_min,
                                           tap_max, classic);
        change = dark * ChangeBands(classic, edge, tap_min, tap_max, guided_low_gain, density, hull);
      }
    } else {
      change = ChangeBilinear(FovFullToWorkPos(pixel, uint2(width, height), size, uint2(foveation_x, foveation_y),
                                               float2(foveation_gmax_x, foveation_gmax_y)),
                              size);
    }
  }
  if ((options & OPTION_MASK) != 0u) {
    change *= mask;
  }
  // A zero change leaves the pixel to the source untouched, which is what NR leaves wherever it did not edit. The
  // test sits before the scene-light transform below, so an untouched pixel pays neither that nor the rebuild;
  // Compose itself only guards non-finite values. The mask multiply cannot turn a non-zero change into zero.
  if (all(change == float3(0.f, 0.f, 0.f))) return source;
  if (!has_original) {
    original = SceneLinear(encoding, primaries, source.rgb, scale);
  }
  return float4(Compose(encoding, primaries, source.rgb, original, change, scale, transfer_strength, color_strength,
                        options, chroma_clamp),
                source.a);
}

#endif  // UPLIFT_DECODE_COMMON_HLSLI
