#include "nr_boost_common.hlsli"
#include "vk_common.hlsli"

// Fit: t0 the network's input, t1 its output -> u0 the sums of each cell and bin, 6 texels each: weight, x (3),
// x x' (6), x y' (9), y (3). Every texel adds to the two bins nearest its luminance, one 8x8 group per cell.
Texture2D<float4> network_input : register(t0);
Texture2D<float4> network_output : register(t1);
UPLIFT_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> boost_sums : register(u0);

groupshared float sums[64 * 22];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex) {
  const uint per = max(cell / 8u, 1u);
  for (uint b = 0u; b < bins; ++b) {
    float s[22];
    [unroll] for (uint k = 0u; k < 22u; ++k) s[k] = 0.f;
    for (uint py = 0u; py < per; ++py) {
      for (uint px = 0u; px < per; ++px) {
        const uint2 p = group.xy * cell + local.xy + 8u * uint2(px, py);
        if (p.x < extent.x && p.y < extent.y) {
          const float3 x = network_input.Load(int3(p, 0)).rgb;
          const float3 y = network_output.Load(int3(p, 0)).rgb;
          const float w = saturate(1.f - abs(BoostBin(x) - float(b)));
          s[0] += w;
          s[1] += w * x.r; s[2] += w * x.g; s[3] += w * x.b;
          s[4] += w * x.r * x.r; s[5] += w * x.r * x.g; s[6] += w * x.r * x.b;
          s[7] += w * x.g * x.g; s[8] += w * x.g * x.b; s[9] += w * x.b * x.b;
          s[10] += w * x.r * y.r; s[11] += w * x.r * y.g; s[12] += w * x.r * y.b;
          s[13] += w * x.g * y.r; s[14] += w * x.g * y.g; s[15] += w * x.g * y.b;
          s[16] += w * x.b * y.r; s[17] += w * x.b * y.g; s[18] += w * x.b * y.b;
          s[19] += w * y.r; s[20] += w * y.g; s[21] += w * y.b;
        }
      }
    }
    [unroll] for (uint k2 = 0u; k2 < 22u; ++k2) sums[index * 22u + k2] = s[k2];
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride >>= 1) {
      if (index < stride) {
        [unroll] for (uint k3 = 0u; k3 < 22u; ++k3) sums[index * 22u + k3] += sums[(index + stride) * 22u + k3];
      }
      GroupMemoryBarrierWithGroupSync();
    }
    if (index < 6u) {
      float4 v;
      v.x = sums[index * 4u];
      v.y = sums[index * 4u + 1u];
      v.z = (index * 4u + 2u < 22u ? sums[index * 4u + 2u] : 0.f);
      v.w = (index * 4u + 3u < 22u ? sums[index * 4u + 3u] : 0.f);
      boost_sums[uint2(group.x * 6u + index, group.y * bins + b)] = v;
    }
    GroupMemoryBarrierWithGroupSync();
  }
}
