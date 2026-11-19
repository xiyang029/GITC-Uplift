#include "../src/color/shader_values.h"
#include "foveation.hlsli"
#include "look_common.hlsli"
#include "vk_common.hlsli"

// NR's change field stabilised on its own. The network's own output flickers (its change moves by a grey level or two from frame to frame) and the game's
// vectors are not trustworthy everywhere, so C is averaged along the vectors in C's own encoding - as look_stabilize_cs.hlsl averages the look's history -
// except that this pass runs after the shape, on the one field the decode consumes. The history is the previous frame's stabilised C, and three gates decide
// how much of it this frame keeps, each of them only ever lowering that:
// - the vectors compete with the static texel: whichever explains this frame's C better is the candidate, so a wrong vector costs sharpness rather than a
//   ghost, and a vector too short to be motion (the frame's jitter) is not resampled along at all;
// - the static texel's own worth is how well it explains C: a rejected vector on a still surface keeps the history whole, on a changed one none of it;
// - l_M (w) gates the history, so a real change of the input's own luminance is taken at once and a flicker is not. w itself is always this frame's.
// An edit of C itself - NR's change moving the display value by a few codes of 255 - is taken at once too: that is what l_M alone misses, a soft shadow
// moving the luminance too little to clear its gate while the change still steps. A small perceptual clip margin lets flat regions average their own
// flicker without letting stale history cross a nearby surface.
cbuffer ChangeStabilizeConstants : register(b0) {
  uint image_width;
  uint image_height;
  float rate;            // alpha = 1 - exp(-dt/tau), look::StabilizeRate
  uint flags;            // CHANGE_STABILIZE_FLAG_*
  uint motion_x;         // the vectors' rect in t2
  uint motion_y;
  uint motion_width;
  uint motion_height;
  float motion_scale_x;  // the vectors' scale into the pixels of their rect (MV.Scale x MotionScale)
  float motion_scale_y;
  uint foveation_x;     // map work-grid coordinates to/from the full image
  uint foveation_y;
  float foveation_gmax_x;
  float foveation_gmax_y;
};

static const uint FLAG_RESET = UPLIFT_CHANGE_STABILIZE_FLAG_RESET;      // this frame starts the history afresh
static const uint FLAG_MOTION = UPLIFT_CHANGE_STABILIZE_FLAG_MOTION;    // reproject along the vectors
static const uint FLAG_VECTORS = UPLIFT_CHANGE_STABILIZE_FLAG_VECTORS;  // t2 holds them; without them Motion is change-gated static
static const float GUIDE_GATE_LOW = 0.25f;   // stops of l_M: below this the history is trusted fully
static const float GUIDE_GATE_HIGH = 1.0f;   // and above it, not at all (a real change is taken at once)
static const float VECTOR_MARGIN = 0.01f;       // how much better the reprojection must explain C than the static texel to be kept: a tie stays with the
                                             // texel, which is not resampled
static const float MIN_MOTION_PIXELS = 0.25f;  // a shorter vector is the frame's own jitter: reprojecting along it would only soften the history
static const float SUBPIXEL_ERROR = 0.02f;     // PredictionError below this is the bilinear resample, not a change of C: the texel is worth its whole self
static const float UNCERTAIN_ERROR = 0.12f;    // and above it, no more than UNCERTAIN_CONFIDENCE
static const float UNCERTAIN_CONFIDENCE = 0.5f;  // what a rejected vector's static sample is worth once it explains C no better than that
// An edit, in OkLab lightness (which is what the display value tracks): about 2 of 255 codes is the low gate and 6 is the high gate;
// a flicker moves C by about one. The amounts are the change's luminance move (ln2/3 of its stops, OkLab's L being the cube root of the light) and its chroma
// move (Chroma is per lightness, so Rebuild's chroma*lightness), both against the pixel's own lightness - the same codes at any level, as a display is.
static const float EDIT_LOW = 0.0067f;  // ~2 of 255: isolated one-code noise remains filtered
static const float EDIT_HIGH = 0.020f;  // ~6 of 255: persistent small edits catch up sooner
static const float CLIP_MAX_LUMINANCE_STOPS = 0.125f;  // limits how much a stale edge can escape the local range
static const float CLIP_MAX_CHROMA = 0.05f;            // likewise for the two chroma channels

Texture2D<float4> change_texture : register(t0);    // this frame's C (shape's output)
Texture2D<float4> previous_texture : register(t1);  // last frame's stabilised C
Texture2D<float4> motion_texture : register(t2);    // current -> previous
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> current_texture : register(u0);  // this frame's stabilised C, next frame's history

// u(p) in full-image UV, current -> previous. NR's vectors are in the source image's space; convert each endpoint
// through the foveation warp before reading or reprojecting the work-grid history.
float2 MotionAt(float2 full_uv) {
  if ((flags & FLAG_VECTORS) == 0u) return float2(0.f, 0.f);
  const uint2 rect = uint2(motion_width, motion_height);
  const uint2 texel = uint2(motion_x, motion_y) + min(uint2(full_uv * float2(rect)), rect - 1u);
  const float2 motion = motion_texture.Load(int3(texel, 0)).xy * float2(motion_scale_x, motion_scale_y) / float2(rect);
  return (all(isfinite(motion)) ? motion : float2(0.f, 0.f));
}

// How badly one candidate history explains this frame's C: xyz is what is stabilised, w (l_M) is what tells a real change from a flicker. The candidate
// that moved C the way this frame's C moved is the one that came from the same surface.
float PredictionError(float4 predicted, float4 current) {
  const float3 difference = abs(predicted.xyz - current.xyz);
  return difference.x + difference.y + difference.z + abs(predicted.w - current.w);
}

// A shared 10x10 tile gives each 8x8 group a clamped one-texel halo for the 3x3 range.
groupshared float4 change_tile[100];

void Neighbourhood(uint2 local, out float3 low, out float3 high) {
  const uint centre = (local.y + 1u) * 10u + local.x + 1u;
  low = change_tile[centre].xyz;
  high = low;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      const uint index = uint(int(centre) + y * 10 + x);
      const float3 neighbour = change_tile[index].xyz;
      low = min(low, neighbour);
      high = max(high, neighbour);
    }
  }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group_id : SV_GroupID, uint3 group_thread_id : SV_GroupThreadID, uint group_index : SV_GroupIndex) {
  const uint2 image = uint2(image_width, image_height);
  [unroll] for (uint tile_iteration = 0u; tile_iteration < 2u; ++tile_iteration) {
    const uint tile_index = group_index + tile_iteration * 64u;
    if (tile_index >= 100u) continue;
    const uint2 tile_pixel = uint2(tile_index % 10u, tile_index / 10u);
    const int2 source_pixel = clamp(int2(group_id.xy * 8u + tile_pixel) - int2(1, 1), int2(0, 0), int2(image) - int2(1, 1));
    change_tile[tile_index] = change_texture.Load(int3(source_pixel, 0));
  }
  GroupMemoryBarrierWithGroupSync();
  if (id.x >= image.x || id.y >= image.y) return;
  const uint2 local = group_thread_id.xy;
  const float4 current = change_tile[(local.y + 1u) * 10u + local.x + 1u];
  float4 result = current;
  if ((flags & FLAG_RESET) == 0u) {
    float4 history = previous_texture.Load(int3(id.xy, 0));  // the static sample, and the fallback
    float confidence = 1.f;
    if ((flags & (FLAG_MOTION | FLAG_VECTORS)) == (FLAG_MOTION | FLAG_VECTORS)) {
      const float2 work_uv = (float2(id.xy) + 0.5f) / float2(image);
      const uint2 packed = uint2(foveation_x, foveation_y);
      const float2 gmax = float2(foveation_gmax_x, foveation_gmax_y);
      const FovAxis fov_x = FovDecode(packed.x, gmax.x);
      const FovAxis fov_y = FovDecode(packed.y, gmax.y);
      float2 full_uv = work_uv;
      if (FovAxisOn(packed.x)) full_uv.x = FovUnwarpAxis1(work_uv.x, fov_x);
      if (FovAxisOn(packed.y)) full_uv.y = FovUnwarpAxis1(work_uv.y, fov_y);
      const float2 previous_full_uv = full_uv + MotionAt(full_uv);
      const bool inside = all(previous_full_uv >= 0.f) && all(previous_full_uv <= 1.f);
      if (!inside) {
        confidence = 0.f;  // off screen: nothing to reproject from
      } else {
        float2 previous_work_uv = previous_full_uv;
        if (FovAxisOn(packed.x)) previous_work_uv.x = FovWarpAxis1(previous_full_uv.x, fov_x);
        if (FovAxisOn(packed.y)) previous_work_uv.y = FovWarpAxis1(previous_full_uv.y, fov_y);
        const float2 work_motion = previous_work_uv - work_uv;
        if (length(work_motion * float2(image)) >= MIN_MOTION_PIXELS) {
          const float4 reprojected = RegionBilinear(previous_texture, int2(0, 0), int2(image), previous_work_uv * float2(image) - 0.5f);
          const float error_static = PredictionError(history, current);
          const float error_reprojected = PredictionError(reprojected, current);
          if (error_reprojected + VECTOR_MARGIN < error_static) {
            history = reprojected;  // the vector explains C better than the texel does: it came from the same surface
          } else {
            // The texel does. What it is worth is how well it explains C: a still surface keeps the whole history, a changed one only half of it.
            confidence = 1.f - (1.f - UNCERTAIN_CONFIDENCE) * smoothstep(SUBPIXEL_ERROR, UNCERTAIN_ERROR, error_static);
          }
        }  // else: the vector is the jitter, not motion - the texel is the right sample and is worth its whole self
      }
    }
    // l_M: the history is worth as much as the input's own luminance agrees with it.
    confidence *= 1.f - smoothstep(GUIDE_GATE_LOW, GUIDE_GATE_HIGH, abs(current.w - history.w));
    // An edit of C itself: NR moving the display value by a few codes, which l_M does not see when it is a soft shadow rather than a brightness step.
    const float lightness = max(exp2(current.w * (1.f / 3.f)), LIGHTNESS_FLOOR);  // OkLab's L: the cube root of the light, w being its stops
    const float edit_luminance = lightness * (0.693147f / 3.f) * abs(current.x - history.x);
    const float edit_chroma = lightness * length(current.yz - history.yz);
    confidence *= 1.f - smoothstep(EDIT_LOW, EDIT_HIGH, max(edit_luminance, edit_chroma));
    float3 low;
    float3 high;
    Neighbourhood(local, low, high);
    // A hard min/max erases temporal averaging when a whole flat patch flickers together. Allow only a small
    // perceptual error beyond the local hull, with caps that keep history from crossing a nearby surface.
    const float luminance_clip = min(CLIP_MAX_LUMINANCE_STOPS, EDIT_LOW / max(lightness * (0.693147f / 3.f), 1e-5f));
    const float chroma_clip = min(CLIP_MAX_CHROMA, EDIT_LOW / max(lightness, LIGHTNESS_FLOOR));
    const float3 clip_margin = float3(luminance_clip, chroma_clip, chroma_clip);
    history.xyz = clamp(history.xyz, low - clip_margin, high + clip_margin);
    const float blend = 1.f - (1.f - rate) * confidence;  // this frame's weight
    const float4 blended = float4(history.xyz + blend * (current.xyz - history.xyz), current.w);
    result = (all(isfinite(blended)) ? blended : current);
  }
  current_texture[id.xy] = result;  // a reset frame writes C itself: the new history starts here
}
