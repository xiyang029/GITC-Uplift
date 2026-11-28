// Native NR auxiliary inputs follow exactly the colour grid's inverse spatial mapping.
// No colour transfer or alpha rewrite: these are the game's stored values.
#include "foveation.hlsli"
cbuffer Constants : register(b0) {
  uint region_x, region_y, region_width, region_height;
  uint width, height;
  uint foveation_x, foveation_y;
  float foveation_gmax_x, foveation_gmax_y;
  uint bilinear;
};
Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  const float2 uv = (float2(id.xy) + 0.5f) / float2(width, height);
  const float2 position = FovUnwarp(uv, uint2(foveation_x, foveation_y), float2(foveation_gmax_x, foveation_gmax_y));
  const float2 sample_position = float2(region_x, region_y) + saturate(position) * float2(region_width, region_height) - 0.5f;
  if (bilinear == 0u) {
    const uint2 pixel = min(uint2(max(sample_position + 0.5f, 0.f)), uint2(region_x + region_width - 1u, region_y + region_height - 1u));
    const float4 value = source.Load(int3(pixel, 0));
    target[id.xy] = value;
  } else {
    const float2 clamped = clamp(sample_position, float2(region_x, region_y),
        float2(region_x + region_width - 1u, region_y + region_height - 1u));
    const int2 lo = int2(floor(clamped));
    const int2 hi = min(lo + 1, int2(region_x + region_width - 1u, region_y + region_height - 1u));
    const float2 f = frac(clamped);
    const float4 a = lerp(source.Load(int3(lo, 0)), source.Load(int3(hi.x, lo.y, 0)), f.x);
    const float4 b = lerp(source.Load(int3(lo.x, hi.y, 0)), source.Load(int3(hi, 0)), f.x);
    const float4 value = lerp(a, b, f.y);
    target[id.xy] = value;
  }
}
