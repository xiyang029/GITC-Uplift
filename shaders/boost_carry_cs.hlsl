#include "nr_boost_common.hlsli"
#include "vk_common.hlsli"

// Carry: t0 this evaluation's network input, t1 the held detail, t2 the model, t3 the motion accumulated up to the last
// network frame. The model applied to this input says what the low frequencies are now; the held detail
// is what the network had to add on top of that same model when it last ran, and it rides along scaled by how well this
// frame's input still matches where it came from. Vectors too long to reproject, or a motion field that disagrees with
// itself where they land, fade the detail out, so a wrong one costs some softness instead of a displaced picture.
Texture2D<float4> network_input : register(t0);
Texture2D<float4> held_detail : register(t1);
Texture2D<float4> boost_model_in : register(t2);
Texture2D<float4> motion_texture : register(t3);
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> carried_output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= extent.x || id.y >= extent.y) return;
  const float3 now = network_input.Load(int3(id.xy, 0)).rgb;
  const float2 source = BoostSource(id.xy, motion_texture);
  const bool inside = BoostInside(source);
  const float2 q = inside ? source : float2(id.xy) + 0.5f;
  // 3x3, two texels apart: the same reach as a full 5x5 window for 9 samples instead of 25, so an edge that moved still
  // reads as a mismatch. This loop is most of what a skipped frame costs. It stops as soon as the difference has faded
  // the carry out anyway -- BoostFade is 1 at unstable_above, so what the rest of the window would have measured cannot
  // matter.
  float d = 0.f;
  for (int y = -2; y <= 2; y += 2) {
    for (int x = -2; x <= 2; x += 2) {
      const int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(extent) - 1);
      const float moved = BoostLuma(network_input.Load(int3(p, 0)).rgb);
      d = max(d, abs(moved - BoostHeldLuma(held_detail, q + float2(x, y))));
      if (d >= unstable_above) break;
    }
    if (d >= unstable_above) break;
  }
  float keep = inside ? 1.f - BoostFade(d, stable_below, unstable_above) : 0.f;
  // The vectors themselves: how far they reach, and whether the field agrees with them where they land. Both are the
  // cheap stand-in for the occlusion test NR would need a depth buffer for, and both mean "do not trust this texel".
  // They now span the whole cycle rather than one frame, so their thresholds are the caller's, scaled by that cycle.
  const float2 move = source - (float2(id.xy) + 0.5f);
  keep *= 1.f - BoostFade(length(move), mv_stable_below, mv_unstable_above);
  if (keep > 0.f) {
    // The field, over the four texels the sample lands between rather than the one it rounds to: a vector that disagrees
    // with its neighbours right where it lands is as much a reason not to trust it as the texel it came from was.
    const float2 corner_base = floor(q - 0.5f);
    float gap = 0.f;
    [unroll] for (uint corner = 0u; corner < 4u; ++corner) {
      const float2 step = float2((corner & 1u) != 0u ? 1.f : 0.f, (corner & 2u) != 0u ? 1.f : 0.f);
      const uint2 at = clamp(uint2(corner_base + step), uint2(0u, 0u), extent - 1u);
      gap = max(gap, length(BoostMotionAt(at, motion_texture) - move));
    }
    keep *= 1.f - BoostFade(gap, mv_field_above * 0.25f, mv_field_above);
  }
  // Only the detail rides along, never last frame's picture: what its network explained is the model's job on this
  // frame's input anyway, and a vector that lands a texel or two off then costs a little softness where it is wrong
  // rather than leaving the frame before behind.
  const float3 detail = BoostBilinear(held_detail, q).rgb;
  const float3 model_now = BoostLook(boost_model_in, now, q);
  carried_output[id.xy] = float4(model_now + detail * keep, 1.f);
}
