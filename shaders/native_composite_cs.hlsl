// Preserve the game's full-resolution detail while carrying native NR's work-resolution changes back.
#include "foveation.hlsli"

cbuffer Constants : register(b0) {
  uint width, height;
  uint work_width, work_height;
  uint origin_x, origin_y;
  uint foveation_x, foveation_y;
  float foveation_gmax_x, foveation_gmax_y;
};
Texture2D<float4> original_texture : register(t0);
Texture2D<float4> baseline_texture : register(t1);
Texture2D<float4> nr_output_texture : register(t2);
RWTexture2D<float4> target_texture : register(u0);

float4 SampleBaseline(float2 position) {
  const float2 clamped = clamp(position, 0.f, float2(work_width - 1u, work_height - 1u));
  const int2 lo = int2(floor(clamped));
  const int2 hi = min(lo + 1, int2(work_width - 1u, work_height - 1u));
  const float2 f = frac(clamped);
  const float4 top = lerp(baseline_texture.Load(int3(lo, 0)), baseline_texture.Load(int3(hi.x, lo.y, 0)), f.x);
  const float4 bottom = lerp(baseline_texture.Load(int3(lo.x, hi.y, 0)), baseline_texture.Load(int3(hi, 0)), f.x);
  return lerp(top, bottom, f.y);
}

float4 SampleNrOutput(float2 position) {
  const float2 clamped = clamp(position, 0.f, float2(work_width - 1u, work_height - 1u));
  const int2 lo = int2(floor(clamped));
  const int2 hi = min(lo + 1, int2(work_width - 1u, work_height - 1u));
  const float2 f = frac(clamped);
  const float4 top = lerp(nr_output_texture.Load(int3(lo, 0)), nr_output_texture.Load(int3(hi.x, lo.y, 0)), f.x);
  const float4 bottom = lerp(nr_output_texture.Load(int3(lo.x, hi.y, 0)), nr_output_texture.Load(int3(hi, 0)), f.x);
  return lerp(top, bottom, f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  const uint2 pixel = id.xy;
  const float2 work_position = FovFullToWorkPos(pixel, uint2(width, height), uint2(work_width, work_height),
      uint2(foveation_x, foveation_y), float2(foveation_gmax_x, foveation_gmax_y));
  const float3 baseline = SampleBaseline(work_position).rgb;
  const float3 nr_output = SampleNrOutput(work_position).rgb;
  const float4 original = original_texture.Load(int3(pixel, 0));
  float3 delta = nr_output - baseline;
  if (!all(isfinite(delta))) delta = 0.f;
  target_texture[uint2(origin_x, origin_y) + pixel] = float4(original.rgb + delta, original.a);
}
