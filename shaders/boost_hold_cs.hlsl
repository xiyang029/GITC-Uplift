#include "nr_boost_common.hlsli"
#include "vk_common.hlsli"

// Hold: t0 the network's input, t1 its output, t2 the model -> what the model does not explain (the fine detail) in
// colour, and that input's luminance in alpha, for the next evaluation's Carry.
Texture2D<float4> network_input : register(t0);
Texture2D<float4> network_output : register(t1);
Texture2D<float4> boost_model_in : register(t2);
UPLIFT_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> boost_change : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= extent.x || id.y >= extent.y) return;
  const float3 x = network_input.Load(int3(id.xy, 0)).rgb;
  boost_change[id.xy] = float4(network_output.Load(int3(id.xy, 0)).rgb - BoostLook(boost_model_in, x, float2(id.xy) + 0.5f), BoostLuma(x));
}
