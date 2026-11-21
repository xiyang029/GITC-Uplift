#include "../src/color/shader_values.h"
#include "look_common.hlsli"
#include "vk_common.hlsli"

// NR's change field stabilised on its own. The network's own output flickers (its change moves by a grey level or two from frame to frame) and the game's
// vectors are not trustworthy everywhere, so C is averaged along the vectors in C's own encoding - as look_stabilize_cs.hlsl averages the look's history -
// except that this pass runs after the shape, on the one field the decode consumes. The history is the previous frame's stabilised C, and three gates decide
// how much of it this frame keeps:
// - l_M (w) gates each bilinear tap before mixing, so taps across an edge cannot average their way into a match. w itself is always this frame's.
// - the vector must explain the guide better than the static texel, accounting for how much of its footprint survived. Short vectors and close calls
//   keep the static texel, which is not resampled.
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
};

static const uint FLAG_RESET = UPLIFT_CHANGE_STABILIZE_FLAG_RESET;      // this frame starts the history afresh
static const uint FLAG_MOTION = UPLIFT_CHANGE_STABILIZE_FLAG_MOTION;    // reproject along the vectors
static const uint FLAG_VECTORS = UPLIFT_CHANGE_STABILIZE_FLAG_VECTORS;  // t2 holds them; without them Motion is change-gated static
static const float GUIDE_GATE_LOW = 0.25f;   // stops of l_M: below this the history is trusted fully
static const float GUIDE_GATE_HIGH = 1.0f;   // and above it, not at all (a real change is taken at once)
static const float MIN_MOTION_PIXELS = 0.25f;  // a shorter vector is the frame's own jitter: reprojecting along it would only soften the history
static const float MIN_VECTOR_SUPPORT = 0.15f;  // fraction of the bilinear footprint supported by the guide
static const float VECTOR_SUPPORT_PENALTY = 0.35f;  // guide-equivalent stops for missing footprint
static const float VECTOR_MARGIN = 0.025f;  // guide-equivalent stops; prefer unresampled history on close calls
static const float EDIT_SCORE_LIMIT = 0.04f;  // capped guide-equivalent edit penalty, secondary to l_M
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

// current -> previous in work-image UV. The host binds vectors already remapped for foveation.
float2 MotionAt(float2 work_uv) {
  const uint2 rect = uint2(motion_width, motion_height);
  const uint2 texel = uint2(motion_x, motion_y) + min(uint2(work_uv * float2(rect)), rect - 1u);
  const float2 motion = motion_texture.Load(int3(texel, 0)).xy * float2(motion_scale_x, motion_scale_y) / float2(rect);
  return (all(isfinite(motion)) ? motion : float2(0.f, 0.f));
}

// How badly one candidate history explains this frame's C: xyz is what is stabilised, w (l_M) is what tells a real change from a flicker. The candidate
// that moved C the way this frame's C moved is the one that came from the same surface.
float GuideConfidence(float error) {
  return 1.f - smoothstep(GUIDE_GATE_LOW, GUIDE_GATE_HIGH, error);
}

float PerceptualEdit(float3 history, float3 current, float lightness) {
  const float3 delta = current - history;
  return lightness * max((0.693147f / 3.f) * abs(delta.x), length(delta.yz));
}

float EditPenalty(float edit) {
  return EDIT_SCORE_LIMIT * smoothstep(EDIT_LOW, EDIT_HIGH, edit);
}

// Keep the original bilinear footprint. Normalise the change, but retain the pre-normalisation weight as confidence.
void GuidedHistory(float2 position, int2 size, float guide, out float3 change, out float support, out float guide_error) {
  const float2 clamped = clamp(position, float2(0.f, 0.f), float2(size - 1));
  const int2 first = int2(floor(clamped));
  const int2 last = min(first + 1, size - 1);
  const float2 fraction = clamped - float2(first);
  float4 sum = 0.f;
  support = 0.f;
  [unroll] for (int y = 0; y < 2; ++y) {
    [unroll] for (int x = 0; x < 2; ++x) {
      const int2 pixel = int2(x == 0 ? first.x : last.x, y == 0 ? first.y : last.y);
      const float4 tap = previous_texture.Load(int3(pixel, 0));
      const float error = abs(guide - tap.w);
      const float bilinear = (x == 0 ? 1.f - fraction.x : fraction.x) * (y == 0 ? 1.f - fraction.y : fraction.y);
      const float weight = bilinear * GuideConfidence(error);
      sum += weight * float4(tap.xyz, error);
      support += weight;
    }
  }
  sum /= max(support, MIN_VECTOR_SUPPORT);
  change = sum.xyz;
  guide_error = sum.w;
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
    const float4 stationary = previous_texture.Load(int3(id.xy, 0));
    const float lightness = max(exp2(current.w * (1.f / 3.f)), LIGHTNESS_FLOOR);
    const float static_guide_error = abs(current.w - stationary.w);
    float3 history = stationary.xyz;
    float confidence = GuideConfidence(static_guide_error);
    float edit = PerceptualEdit(history, current.xyz, lightness);
    if ((flags & (FLAG_MOTION | FLAG_VECTORS)) == (FLAG_MOTION | FLAG_VECTORS)) {
      const float2 work_uv = (float2(id.xy) + 0.5f) / float2(image);
      const float2 work_motion = MotionAt(work_uv);
      const float2 previous_work_uv = work_uv + work_motion;
      const bool inside = all(previous_work_uv >= 0.f) && all(previous_work_uv <= 1.f);
      if (!inside) {
        confidence = 0.f;  // off screen: nothing to reproject from
      } else if (length(work_motion * float2(image)) >= MIN_MOTION_PIXELS) {
        float3 reprojected;
        float support;
        float guide_error;
        GuidedHistory(previous_work_uv * float2(image) - 0.5f, int2(image), current.w, reprojected, support, guide_error);
        if (support >= MIN_VECTOR_SUPPORT) {
          const float vector_edit = PerceptualEdit(reprojected, current.xyz, lightness);
          const float vector_score = guide_error + VECTOR_SUPPORT_PENALTY * (1.f - saturate(support)) + EditPenalty(vector_edit);
          const float static_score = static_guide_error + EditPenalty(edit);
          if (vector_score + VECTOR_MARGIN < static_score) {
            history = reprojected;
            confidence = saturate(support);
            edit = vector_edit;
          }
        }
      }
    }
    // An edit of C itself: NR moving the display value by a few codes, which l_M does not see when it is a soft shadow rather than a brightness step.
    confidence *= 1.f - smoothstep(EDIT_LOW, EDIT_HIGH, edit);
    float3 low;
    float3 high;
    Neighbourhood(local, low, high);
    // A hard min/max erases temporal averaging when a whole flat patch flickers together. Allow only a small
    // perceptual error beyond the local hull, with caps that keep history from crossing a nearby surface.
    const float luminance_clip = min(CLIP_MAX_LUMINANCE_STOPS, EDIT_LOW / max(lightness * (0.693147f / 3.f), 1e-5f));
    const float chroma_clip = min(CLIP_MAX_CHROMA, EDIT_LOW / max(lightness, LIGHTNESS_FLOOR));
    const float3 clip_margin = float3(luminance_clip, chroma_clip, chroma_clip);
    history = clamp(history, low - clip_margin, high + clip_margin);
    const float blend = 1.f - (1.f - rate) * confidence;  // this frame's weight
    const float4 blended = float4(history + blend * (current.xyz - history), current.w);
    result = (all(isfinite(blended)) ? blended : current);
  }
  current_texture[id.xy] = result;  // a reset frame writes C itself: the new history starts here
}
