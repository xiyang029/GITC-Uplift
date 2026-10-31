#include "nr_boost_common.hlsli"
#include "vk_common.hlsli"

// Compose: t0 the game's vectors now, t1 the motion accumulated up to this point -> u0 that plus this frame's, at texels
// of our own texture and in the vectors' own units. The carry reads it to find the last frame the network ran, which is
// further back than one frame whenever the cycle skips more than every other one; the network reads it as its own
// motion, because that is the evaluation its history is behind by.
Texture2D<float4> motion_texture : register(t0);
Texture2D<float4> held_motion : register(t1);
UPLIFT_IMAGE_FORMAT("rg32f") RWTexture2D<float2> composed_motion : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= motion_extent.x || id.y >= motion_extent.y) return;
  if (id.x < motion_size.x && id.y < motion_size.y && motion_size.x != 0u && motion_size.y != 0u) {
    const float2 now = motion_texture.Load(int3(motion_origin + int2(id.xy), 0)).xy;
    // Where this texel was on the previous evaluation. Everything here stays in the vectors' own rect, so only their
    // scale applies, not the rect's ratio to the canvas.
    const float2 q = float2(id.xy) + 0.5f + now * motion_scale;
    float2 before = 0.f;
    if (compose_reset == 0u && all(isfinite(q)) && all(q >= 0.f) && all(q < float2(motion_extent))) {
      before = held_motion.Load(int3(int2(q), 0)).xy;
    }
    composed_motion[id.xy] = now + before;
  } else {
    composed_motion[id.xy] = float2(0.f, 0.f);  // outside the subrect the network reads: no motion
  }
}
