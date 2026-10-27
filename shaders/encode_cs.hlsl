#include "change_common.hlsli"
#include "foveation.hlsli"
#include "vk_common.hlsli"

cbuffer EncodeConstants : register(b0) {
  uint encoding;
  uint width;          // the work image: the source region resampled to this size
  uint height;
  float input_scale;
  uint second_output;  // encode mode: clear u1 (the zero motion); change mode: write NR's input chroma κ_M into u1
  uint has_exposure;
  float exposure_factor;
  uint source_x;       // the source region
  uint source_y;
  uint source_width;
  uint source_height;
  uint canvas_width;   // the model texture: the image plus mirrored padding
  uint canvas_height;
  uint change_mode;    // 0: encode; 1: extract changes with recomputed input; 2: extract changes with preserved FP16 input
  uint options;        // Plan 5: OPTION_* (the primaries, the near-black guard)
  uint reserved0;
  uint foveation_x;    // foveated grid: the packed per-axis warp (0 = identity) and its realized density
  uint foveation_y;
  float foveation_gmax_x;
  float foveation_gmax_y;
};

// Plan 13: on Vulkan's After DLSS the source is DLSS's output, read in place from its storage view (u2).
#if UPLIFT_VK_SOURCE_STORAGE
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> source_texture : register(u2);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture[uint2(pixel)]
#else
Texture2D<float4> source_texture : register(t0);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture.Load(int3((pixel), 0))
#endif
Texture2D<float4> nr_texture : register(t1);
Texture2D<float> exposure_texture : register(t2);
Texture2D<float4> model_input_texture : register(t3);
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> output_texture : register(u0);  // the model texture, or the change field in change mode
UPLIFT_IMAGE_FORMAT("rg16f") RWTexture2D<float2> motion_texture : register(u1);

// Per side of one footprint. A foveated axis' coarsest local slope is 1/Edge of the uniform grid's, and the
// uniform grid is at least Custom's 25 %, so the widest footprint is sixteen source pixels; one more covers a
// fractional phase. The bound only clamps the loop, whose trip count the footprint itself decides, so a
// too-narrow one silently averages a corner of the footprint instead -- which is why nr/foveation.hpp asserts the
// two numbers this one is derived from (kMinEdge, kMinWorkScale).
static const int MAX_TAPS = 17;

// A sliver of coverage this thin is the footprint's own edge rather than a tap: the plateau's footprint lands on
// an integer to within fp32's rounding (2.4e-4 px at 5K), and 1e-3 px of a pixel moves the mean by 0.1 %, well
// under the FP16 store's step. Trimming it on both sides keeps a footprint an ulp wider than one pixel at one tap
// -- which is every plateau texel of the 1:1 grid, whose edge only misses an integer by that rounding.
static const float FOOTPRINT_SLIVER = 1e-3f;

// v2 design §3.8's downscale: the mean of the source under image pixel `pixel`'s footprint in linear
// light, each source pixel weighted by the share of it the footprint covers, in the model domain.
//
// A footprint that lies inside one source pixel is that pixel, and EncodeModel gives its model value
// directly: the foveated plateau of a full-size grid, where the grid is 1:1 with the source, is then one
// load -- not a decode/average/encode round trip costing six transcendentals for a value it already has.
float3 AreaModel(uint2 pixel, float scale) {
  const uint primaries = options & OPTION_PRIMARIES_MASK;
  const float2 ratio = float2(source_width, source_height) / float2(width, height);
  float2 low = float2(pixel) * ratio;
  float2 high = low + ratio;
  // Foveated grid: this work texel covers the unwarped span of the full image, so the centre keeps the
  // uniform density exactly and the fall-off and edge zones average fewer source pixels.
  FovWorkFootprint(pixel, uint2(width, height), uint2(source_width, source_height), uint2(foveation_x, foveation_y),
                   float2(foveation_gmax_x, foveation_gmax_y), low, high);
  const int2 first = int2(floor(low + FOOTPRINT_SLIVER));
  const int2 last = min(int2(ceil(high - FOOTPRINT_SLIVER)) - 1, int2(source_width, source_height) - 1);
  const int2 source = int2(source_x, source_y);
  if (all(first == last)) {
    return QuantizeHalf(EncodeModel(encoding, primaries, UPLIFT_LOAD_SOURCE(source + first).rgb, scale));
  }
  float3 sum = float3(0.f, 0.f, 0.f);
  float total = 0.f;
  [loop] for (int y = first.y; y <= min(last.y, first.y + MAX_TAPS - 1); ++y) {
    const float cover_y = min(high.y, float(y + 1)) - max(low.y, float(y));
    [loop] for (int x = first.x; x <= min(last.x, first.x + MAX_TAPS - 1); ++x) {
      const float cover = cover_y * (min(high.x, float(x + 1)) - max(low.x, float(x)));
      sum += cover * LinearLight(encoding, primaries, UPLIFT_LOAD_SOURCE(source + int2(x, y)).rgb, scale);
      total += cover;
    }
  }
  // The warp is strictly monotone and both grids are non-empty, so the loop ran and `total` is positive.
  return QuantizeHalf(EncodeLinear(encoding, sum / total));
}

// The model input at `pixel`, shared by encode and change extraction: direct at matching dimensions, otherwise the encoded linear-light area mean.
float3 ModelAt(uint2 pixel, float scale) {
  if (source_width == width && source_height == height) {
    return QuantizeHalf(EncodeModel(encoding, options & OPTION_PRIMARIES_MASK,
                                    UPLIFT_LOAD_SOURCE(uint2(source_x, source_y) + pixel).rgb, scale));
  }
  return AreaModel(pixel, scale);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 extent = (change_mode != 0u ? uint2(width, height) : uint2(canvas_width, canvas_height));
  if (id.x >= extent.x || id.y >= extent.y) return;
  // A texture read is not free even from a 1x1, and without an exposure texture there is nothing to read: the
  // branch is uniform, so the load disappears from every texel of a pass that has none.
  float exposure = 1.f;
  if (has_exposure != 0u) {
    exposure = exposure_texture.Load(int3(0, 0, 0));
  }
  // `precise` keeps the model value bit-identical between encode and change mode. The value is an exact FP16 number, so the UAV conversion cannot round differently from QuantizeHalf.
  precise float scale = input_scale * GameExposure(has_exposure, exposure, exposure_factor);
  precise float3 model_value;
  if (change_mode == 2u) {
    model_value = model_input_texture.Load(int3(id.xy, 0)).rgb;
  } else {
    model_value = ModelAt(uint2(Mirror(id.x, width), Mirror(id.y, height)), scale);
  }
  if (change_mode != 0u) {
    output_texture[id.xy] =
        ChangeField(encoding, model_value, nr_texture.Load(int3(id.xy, 0)).rgb, (options & OPTION_NEAR_BLACK_GUARD) != 0u);
    if (second_output != 0u) {
      motion_texture[id.xy] = Chroma(LinearToOkLab(SrgbDecode(model_value)));  // κ_M, the look's colour basis
    }
    return;
  }
  output_texture[id.xy] = float4(model_value, 1.f);
  if (second_output != 0u) {
    motion_texture[id.xy] = float2(0.f, 0.f);
  }
}
