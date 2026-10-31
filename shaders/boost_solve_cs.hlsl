#include "nr_boost_common.hlsli"
#include "vk_common.hlsli"

// Solve: t0 the sums -> u0 the model, 3 texels per cell and bin (output r, g, b: the weights of the input's r, g, b
// and an offset). The sums are blurred [1 2 1] over the neighbouring cells first; lambda pulls each map toward the
// plain offset, so flat or empty cells and bins keep the tone change without inventing contrast.
Texture2D<float4> boost_sums_in : register(t0);
UPLIFT_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> boost_model_out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= cells.x || id.y >= cells.y * bins) return;
  const int2 c = int2(id.x, id.y / bins);
  const uint b = id.y % bins;
  float s[24];
  [unroll] for (uint k = 0u; k < 24u; ++k) s[k] = 0.f;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      const int2 n = c + int2(dx, dy);
      if (all(n >= 0) && all(n < int2(cells))) {
        const float w = float((2 - abs(dx)) * (2 - abs(dy)));
        [unroll] for (uint m = 0u; m < 6u; ++m) {
          const float4 v = boost_sums_in.Load(int3(n.x * 6 + int(m), n.y * int(bins) + int(b), 0)) * w;
          s[m * 4u] += v.x; s[m * 4u + 1u] += v.y; s[m * 4u + 2u] += v.z; s[m * 4u + 3u] += v.w;
        }
      }
    }
  }
  float4 model[3] = {float4(1, 0, 0, 0), float4(0, 1, 0, 0), float4(0, 0, 1, 0)};
  const float n = s[0];
  if (n > 1e-3f) {
    const float3 mx = float3(s[1], s[2], s[3]) / n;
    const float3 my = float3(s[19], s[20], s[21]) / n;
    float3x3 cxx = float3x3(s[4], s[5], s[6], s[5], s[7], s[8], s[6], s[8], s[9]) / n;
    float3x3 cxy = float3x3(s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18]) / n;
    [unroll] for (uint i = 0u; i < 3u; ++i) {
      [unroll] for (uint j = 0u; j < 3u; ++j) {
        cxx[i][j] -= mx[i] * mx[j];
        cxy[i][j] -= mx[i] * my[j];
        if (i == j) {
          cxx[i][j] += lambda;
          cxy[i][j] += lambda;
        }
      }
    }
    const float3 r0 = cxx[0], r1 = cxx[1], r2 = cxx[2];
    const float3 k0 = cross(r1, r2), k1 = cross(r2, r0), k2 = cross(r0, r1);
    const float det = dot(r0, k0);
    if (det > 1e-12f) {
      const float3x3 inverse = transpose(float3x3(k0, k1, k2)) / det;
      const float3x3 a = mul(inverse, cxy);
      [unroll] for (uint o = 0u; o < 3u; ++o) {
        const float3 weights = float3(a[0][o], a[1][o], a[2][o]);
        model[o] = float4(weights, my[o] - dot(mx, weights));
      }
    }
  }
  [unroll] for (uint o2 = 0u; o2 < 3u; ++o2) boost_model_out[uint2(id.x * 3u + o2, id.y)] = model[o2];
}
