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
  uint options;        // Plan 5: OPTION_* (the primaries, Consistent transfer, the mask)
  float chroma_clamp;  // Plan 5 (F14): stops; 0 = off
  uint foveation_x;    // foveated grid: the packed per-axis warp (0 = identity) and its realized density
  uint foveation_y;
  float foveation_gmax_x;
  float foveation_gmax_y;
  float guided_low_gain;   // Edge-aware: the base band's gain (1 = the field's low frequencies unchanged)
  float guided_high_gain;  // Edge-aware: the detail band's gain (1 = the field's high frequencies unchanged)
  float edge_density_attenuation;  // the band ceiling per unit of extra work-grid density (1 = no attenuation)
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

static const float SPATIAL_RADIUS = 1.5f;  // the tent's half-width, in output pixels (v2 design §3.8; per axis since 2026-10-29)
static const float MIN_WEIGHT = 1e-4f;     // below this total the edge-aware sample falls back to bilinear

// Band shaping (2026-10-26; density scale 2026-10-27). The upsample's two bands are shaped before they are
// combined: the detail band by how coarse the working grid is here, both bands by how dark the pixel is.
// Edge-aware (Upsampling 2) sets the gains; they are ceilings on the shaping rather than factors over it.
//
// `density` covers the uniform work scale as well as the foveated grid: a Custom resolution below Full is
// exactly as coarse as the foveated edges are, so leaving it out left the shaping inert for the common case.
// The ceiling per unit of extra density is the caller's now (the UI's slider); the fade-in rate beside it is the
// constant above, which the same density scale drives.
static const float DARK_EDIT_FLOOR = 0.80f;           // the edit's weight in the deepest shadows
static const float DARK_GUIDE_LOW = -7.f;             // log2 stops below diffuse white: fully dark
static const float DARK_GUIDE_HIGH = -3.5f;           // and fully lit
static const float CLAMP_SLACK = 0.15f;               // how far past the neighbourhood's own range the shaped bands may reach
// -1 / (2 * sigma^2 * ln 2) for the range kernel's sigma of 0.5 stops at density 1: the exponent per stop^2.  The
// caller divides it by the density, which widens the Gaussian as sqrt(density).
static const float RANGE_K = -2.885390082f;
// Kernel fade: 1 / the density excess at which the correction reaches full strength; 4 puts that at density 1.25
// (an 80 % work grid). It was a UI row for a day (2026-10-29) and is a constant again: the fade saturates by
// 80 % whatever the rate, so the row only paced the 90-85 % steps, and the coarser steps are the ceiling's to
// set. See ChangeBands: at density 1 the field is 1:1 with the output and the kernel is pure blur, so the
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

// One tap's spatial weight: a tent around `position`, its half-width SPATIAL_RADIUS *output* pixels on each axis,
// which is `radius` work texels. The two axes differ (2026-10-29): a Custom resolution may scale width and height
// apart, and one work texel then spans 1.67 output pixels across and 1.25 down. A tent fixed at 1.5 work texels
// would be an ellipse on screen -- flat across, tall down -- so the residual edge - classic would mean something
// different per axis and no single density could shape both. Dividing the radius by each axis' own upsampling
// keeps the support round in output space, which is the space the image is judged in.
float TentWeight(float2 position, float2 tap, float2 radius) {
  const float2 gap = abs(position - tap);  // not `distance`: an HLSL intrinsic
  return max(1.f - gap.x / radius.x, 0.f) * max(1.f - gap.y / radius.y, 0.f);
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
void AccumulateTap(float4 change, float2 position, float2 tap, float2 radius, float guide, float range_k,
                   bool want_range, inout float3 sum, inout float total, inout float3 tap_min, inout float3 tap_max) {
  const bool finite = all(isfinite(change));
  if (want_range && finite) {
    tap_min = min(tap_min, change.xyz);
    tap_max = max(tap_max, change.xyz);
  }
  const float difference = guide - change.w;
  const float weight = finite ? TentWeight(position, tap, radius) * exp2(difference * difference * range_k) : 0.f;
  sum += weight * (finite ? change.xyz : float3(0.f, 0.f, 0.f));
  total += weight;
}

// Edge-aware: v2 design §3.8's joint bilateral upsample over the work pixels around `position`: a tent in
// space, times how close each pixel's guide ℓ_M is to this output pixel's. The guide is the change field's
// own ℓ_M, bilinear over the same four centre taps that make `classic`, so the range compares two samples
// of one field at one resolution instead of re-encoding the model here -- and the caller still gets its
// guide, its Classic sample and the bilateral answer out of one pass over one neighbourhood.
//
// The centre 2x2 comes first because it is both the Classic answer and the guide. The tent's half-width is
// SPATIAL_RADIUS, so along each axis only the three offsets the cell's own centre falls between can carry
// weight: -1..1 while the fraction is below 0.5, 0..2 above -- nine cells, four of them the 2x2. The five
// that remain are the outer row, the outer column and the far corner, and all five sit inside the support:
// the corner holds 1/9 of its weight at fraction 0 and falls to zero exactly at 0.5, and leaving it out pulled
// the kernel's centroid 0.042 px diagonally -- worst at fraction 0, which is the plateau, where the field is
// 1:1. The row and the column on the opposite side sit at exactly zero at every tap (four loads and four exp2
// per pixel for nothing); only the hull below reads them, for its range.
//
// `want_range` is ChangeBands' clamp flag. It only needs the hull when the detail gain can push the mix
// below it, which means a gain split wider than CLAMP_SLACK; the defaults cannot, so
// that path skips the hull's min/max entirely. The value is uniform, so the branch costs nothing. The hull
// is over every tap, the zero-weight ones included, so the clamp sees the range it always has: that is what
// the second loop loads them for.
float3 ChangeEdgeAware(float2 position, uint2 size, float density, float2 axis_density, bool want_range,
                       out float guide, out float3 tap_min, out float3 tap_max, out float3 classic, out float reach) {
  const int2 base = int2(floor(position));
  const float2 fraction = position - float2(base);
  const float range_k = RANGE_K / density;
  // The tent's half-width per axis, in work texels: output-space round, so an axis the grid resolves less often
  // gets a proportionally narrower one.
  //
  // The floor is one texel, and the band is scaled down to match rather than the tent widened to hide it. A tent
  // narrower than 1 + fraction reaches no outer tap at all, and the nine-tap sum then collapses to its centre
  // 2x2 -- which is the bilinear sample, so `edge` equals `classic`, the detail band is exactly zero and
  // edge-aware quietly turns into Classic. Widening the tent past its output-space width instead would average
  // taps the grid cannot resolve, which is a false sharpness. So the tent stops at one texel and the caller
  // multiplies the band by how much of the intended support survived: a kernel that only reaches 60 % of 1.5
  // pixels has seen 60 % of the neighbourhood it was going to describe, and gets 60 % of the say.
  const float2 ideal = SPATIAL_RADIUS / max(axis_density, float2(1.f, 1.f));
  const float2 radius = max(ideal, float2(1.f, 1.f));
  // Reach is measured on the ideal width, not on the floored one: the floor exists to keep nine taps alive, and
  // reading the band's scaling off it would flatten that scaling for every grid coarser than 67 % -- 50 %, 33 %
  // and 25 % would all report the same 0.67 and lose the distinction the scale is for. This is 1 / max axis.
  reach = saturate(min(ideal.x, ideal.y) / SPATIAL_RADIUS);

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
  guide = bilinear.w;

  float3 sum = float3(0.f, 0.f, 0.f);
  float total = 0.f;
  tap_min = float3(1e30f, 1e30f, 1e30f);
  tap_max = float3(-1e30f, -1e30f, -1e30f);
  AccumulateTap(c00, position, float2(base), radius, guide, range_k, want_range, sum, total, tap_min, tap_max);
  AccumulateTap(c10, position, float2(base + int2(1, 0)), radius, guide, range_k, want_range, sum, total, tap_min, tap_max);
  AccumulateTap(c01, position, float2(base + int2(0, 1)), radius, guide, range_k, want_range, sum, total, tap_min, tap_max);
  AccumulateTap(c11, position, float2(base + int2(1, 1)), radius, guide, range_k, want_range, sum, total, tap_min, tap_max);

  const int2 outer = int2(fraction.x < 0.5f ? -1 : 2, fraction.y < 0.5f ? -1 : 2);
  const int2 ring[4] = {int2(outer.x, 0), int2(outer.x, 1), int2(0, outer.y), int2(1, outer.y)};
  [unroll] for (int i = 0; i < 4; ++i) {
    const int2 tap = base + ring[i];
    AccumulateTap(ChangeTap(tap, size), position, float2(tap), radius, guide, range_k, want_range, sum, total, tap_min,
                  tap_max);
  }
  const int2 corner = base + outer;
  AccumulateTap(ChangeTap(corner, size), position, float2(corner), radius, guide, range_k, want_range, sum, total, tap_min,
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

  if (total < MIN_WEIGHT) {
    // Nothing to average, so the caller has no range to hold on to either: hand it an open one. The magnitude
    // is finite because ChangeBands subtracts the two ends: their sum overflows fp32, and an infinite slack
    // turns the clamp into NaN.
    tap_min = float3(-1e30f, -1e30f, -1e30f);
    tap_max = float3(1e30f, 1e30f, 1e30f);
    return classic;  // the bilinear sample this pass has already built
  }
  return sum / total;
}

// Both bands' shadow attenuation, from the pixel's own guide: shadows carry most of NR's per-frame churn and
// least of its real detail, and that churn is broadband -- attenuating one band alone would only move it to the
// other, so both bands scale together.
float EditDarkness(float guide) {
  return DARK_EDIT_FLOOR + (1.f - DARK_EDIT_FLOOR) * smoothstep(DARK_GUIDE_LOW, DARK_GUIDE_HIGH, guide);
}

// The upsample's two bands, shaped and combined. `classic` is the change field's low frequencies (the
// bilinear sample); `edge` is the joint bilateral answer, so edge - classic is the detail the guided kernel
// holds on the correct side of an edge. The gains come from the caller, both 1 by default. They apply
// under the foveated grid too now that the density shapes the detail band
// by itself; suppressing the setting there as well only took the control away where the grid is coarsest.
//
// Density: a coarser working grid means a work texel carries detail its footprint cannot resolve, so a band
// is held to a ceiling per unit of extra density. At the plateau, and with the grid off, the ceiling is 1 and
// nothing changes. The ceiling is a min, not a factor: a gain below it lowers the band further, and the two
// never compound.
//
// Which band (2026-10-29, OPTION_DENSITY_ON_BASE): the detail band by default, the base band when the option
// is set. Coarsening costs both bands -- the base loses the shape of a large edit as much as the detail loses
// its edge -- but only the detail band was ever attenuated, so the base arrived at the coarsest grids at full
// strength while the detail was gone. Moving the correction to the base leaves the detail at full weight and
// instead holds back the broad edit, which is the trade the setter asks for. It is an option, not the rule,
// because suppressing the base is visible as a fall-off in overall change strength toward a foveated edge,
// and whether that reads as a natural softness or as a vignette depends on the scene.
//
// The kernel's correction fades in over the same density (2026-10-27): edge - classic is the tent's blur, which
// only earns its keep once the field is actually coarser than the output. At density 1 there is no resample to
// smooth, so the correction is zero and the result is `classic * low_gain` -- exactly what DecodePixel's one-load
// path returns -- and by density 1 / KERNEL_FADE_IN it is the full kernel.
//
// `reach` (2026-10-29) is the third gate, and the one an anisotropic Custom scale needs: the tent stops at one
// work texel per axis, so an axis coarser than SPATIAL_RADIUS output pixels per texel cannot span the support
// the kernel was designed around. Rather than widen the tent -- which would average taps the grid cannot resolve
// and read as false sharpness -- ChangeEdgeAware reports how much of that support survived and the band scales
// by it. At a square grid reach is 1 and nothing changes; at X 60 % it is about 0.6, so the detail band keeps
// 60 % of its say, honestly, instead of the kernel pretending to see a neighbourhood it never sampled.
//
// Clamp: `classic * low + (edge - classic) * detail` never rises above the hull -- both bands are convex
// combinations of the same taps and both gains are at most 1 -- so the only excursion is downward, and it
// is the detail gain above the base gain (an extrapolation) that causes it. The base band alone can
// sit below the hull too; that is what a base gain under 1 asks for, so the floor is the lower of the hull
// and the base band, less CLAMP_SLACK. Clamping against the hull alone would undo the base gain wherever
// the neighbourhood is flat, where the whole band sits below a hull only the taps occupy.
//
// With the defaults (1 against 1) the excursion is zero, so the caller does not hand the hull over at all; the
// flag is uniform, and only a base gain under 0.85 turns it on. A hard cut would turn an overstep into
// a flat plateau right where the eye is looking, so the last CLAMP_SLACK rolls off continuously and a moving
// step does not flicker as it cuts in and out. Dark multiplies after the clamp, so the shadow attenuation is
// never undone by it.
float3 ChangeBands(float3 classic, float3 edge, float3 tap_min, float3 tap_max, float low_gain, float high_gain,
                   float density, float guide, bool clamp_bands, float reach) {
  const float dark = EditDarkness(guide);
  const float fade = saturate((density - 1.f) * KERNEL_FADE_IN);
  const float ceiling = pow(edge_density_attenuation, max(density - 1.f, 0.f));
  const bool on_base = (options & OPTION_DENSITY_ON_BASE) != 0u;
  const float low = (on_base ? low_gain * ceiling : low_gain);
  const float detail = min(high_gain, on_base ? 1.f : ceiling) * fade * reach;
  const float3 bands = classic * low + (edge - classic) * detail;
  if (!clamp_bands) return dark * bands;
  const float3 base = classic * low + (edge - classic) * min(detail, low);
  const float3 slack = max((tap_max - tap_min) * CLAMP_SLACK, 1e-4f);
  const float3 floor_band = min(tap_min, base) - slack;
  const float3 over = max(floor_band - bands, 0.f);
  // Asymptote floor + slack: the allowance a peak that genuinely extrapolates this far may keep.
  return dark * (bands + (floor_band + slack - bands) * over / (slack + over));
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
  float3 change;
  if (upsampling == 0u) {
    change = change_texture.Load(int3(pixel, 0)).xyz;
  } else {
    uint2 size;
    change_texture.GetDimensions(size.x, size.y);
    if (upsampling == 2u) {
      // Edge-aware, with its two band gains. Until 2026-10-29 this branch also served Guided (3), which passed
      // the same gains but was only reachable in that mode; the modes merged and this one now always shapes.
      // The guide is the change field's own ℓ_M, which ChangeEdgeAware returns from the taps it reads anyway,
      // so this path no longer re-encodes the model to find it.
      //
      // Only a detail gain more than CLAMP_SLACK above the base gain can push the mix past the floor ChangeBands
      // checks; both defaults are 1, so the hull is skipped, and the UI's lowest base (0.5) is what turns it on.
      //
      // OPTION_DENSITY_ON_BASE widens that split on its own: it multiplies the base band by the density ceiling
      // (the slider's factor per unit of excess density -- 0.85 at a 50 % grid by default, 0.5 at the slider's floor)
      // while the detail band keeps its full gain, so the effective split is 1 - ceiling there -- 0.15 at the default,
      // wider than CLAMP_SLACK as soon as the slider drops below it -- whenever the user leaves the gains at their
      // defaults. The ceiling is per pixel and this flag must stay uniform, so the option asks for the hull outright.
      const bool hull = (options & OPTION_DENSITY_ON_BASE) != 0u || guided_high_gain - guided_low_gain > CLAMP_SLACK;
      // The position and the density of this output pixel's cell come out of one pass over each axis: both
      // want the axis' decoded dword and the same plateau/ramp/edge branch, and the density is that branch's
      // slope. `pixel` is the region's, as the position's own normalization is: the density is about where
      // this output pixel sits in the region, not on the screen.
      const FovSample sx = FovFullToWorkSample1(pixel.x, width, size.x, foveation_x, foveation_gmax_x);
      const FovSample sy = FovFullToWorkSample1(pixel.y, height, size.y, foveation_y, foveation_gmax_y);
      // Output texels per change-field texel, per axis: the tent's support has to be round in output space, so
      // each axis needs its own upsampling rather than one number for both -- a Custom resolution may scale width
      // and height apart, and then the two differ. `ratio` is the uniform scale on that axis, `warp` the foveated
      // grid's own: sx/sy measure against the warp's normalizer (gmax is work / uniform) while ratio measures
      // against the whole working grid, so ratio carries that normalizer's mean as well. Multiplying by the
      // warp's factor leaves the local upsampling -- 1 where the field is 1:1 with the output, the field's own
      // scale elsewhere. Without it a foveated Full grid read 1.17 on its plateau: the range kernel widened, the
      // detail ceiling dropped, and the fast path below could never fire.
      const float2 ratio = float2(float(width), float(height)) / float2(float(size.x), float(size.y));
      const float2 warp = float2(foveation_gmax_x, foveation_gmax_y);
      // ratio >= 1 by construction: sources::WorkSize caps the work grid at the region on every side.
      const float2 axis = float2(sx.density, sy.density) * ratio * warp;
      // The shaping still needs one number: the geometric mean is the scale whose per-axis shaping matches the
      // tent's area, which is what the band ceiling describes.
      const float density = sqrt(axis.x * axis.y);
      // 1e-5 covers the few ulp the two sqrt'd products carry; the next real scale up (Custom 99 %) is four
      // orders of magnitude further, so nothing that resamples can slip in.
      if (density <= 1.f + 1e-5f) {
        // Density 1 means this cell is 1:1 with the change field: each axis unwarped or on the plateau, and no
        // uniform scale. The bilateral on that cell would be a nine-tap average (only 36 % of its weight on the
        // texel itself) over a field that needs no resample, so the texel is the answer and the shaping reduces
        // to the gains and the shadow term; the clamp cannot fire either, as it only lowers below the
        // neighbourhood's own range and this is a texel of it. The load is at the sample's position, not at
        // `pixel`: a foveated grid is shorter than the region and its plateau sits an integer offset in. The
        // test is spatially coherent (the centre rectangle), so it costs divergence only along that border. The
        // ramp's first texels, just past it, are continuous with this: ChangeBands fades the kernel in with the
        // density excess, so they too return `classic * low_gain` at the boundary.
        const uint2 texel = min(uint2(max(float2(sx.position, sy.position) + 0.5f, 0.f)), size - 1u);
        const float4 value = change_texture.Load(int3(texel, 0));
        change = EditDarkness(value.w) * guided_low_gain * value.xyz;
      } else {
        const float2 position = float2(sx.position, sy.position);
        float guide;
        float3 tap_min, tap_max;
        float3 classic;
        float reach;
        const float3 edge =
            ChangeEdgeAware(position, size, density, axis, hull, guide, tap_min, tap_max, classic, reach);
        change = ChangeBands(classic, edge, tap_min, tap_max, guided_low_gain, guided_high_gain, density, guide, hull, reach);
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
  // Compose returns the source untouched when the change is zero, which is what NR leaves wherever it did not
  // edit: skip its argument, the exposure sample above all, rather than read a texture for a value the caller
  // discards. `change == 0` here is exactly Compose's own test, and the mask multiply cannot disturb it.
  if (all(change == float3(0.f, 0.f, 0.f))) return source;
  // A texture read is not free even from a 1x1, and without an exposure texture there is nothing to read: the
  // branch is uniform, so the load disappears from every pixel of a pass that has none.
  float exposure = 1.f;
  if (has_exposure != 0u) {
    exposure = exposure_texture.Load(int3(0, 0, 0));
  }
  precise float scale = input_scale * GameExposure(has_exposure, exposure, exposure_factor);
  return float4(Compose(encoding, primaries, source.rgb, change, scale, transfer_strength, color_strength, options, chroma_clamp),
                source.a);
}

#endif  // UPLIFT_DECODE_COMMON_HLSLI
