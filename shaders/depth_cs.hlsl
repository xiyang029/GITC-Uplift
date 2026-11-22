cbuffer DepthConstants : register(b0) {
  uint2 origin;
  uint2 size;
};
Texture2D<float> source_depth : register(t0);
RWTexture2D<float> target_depth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (all(id.xy < size)) target_depth[id.xy] = source_depth.Load(int3(origin + id.xy, 0));
}
