#include "vk_common.hlsli"

cbuffer DepthConstants : register(b0) {
  uint region_x;      // the game's depth: this region of t0
  uint region_y;
  uint region_width;
  uint region_height;
};

Texture2D<float> depth_source : register(t0);
UPLIFT_IMAGE_FORMAT("r32f") RWTexture2D<float> depth_target : register(u0);

// 2026-11-06 (Depth input): a 64-bit depth-stencil cannot reach DLSSNR.Depth by a copy -- Direct3D 12 copies a whole subresource, and no
// copy can carry one plane of it -- so its depth plane is read through a view and written to the R32_FLOAT texture the depth copy ring
// holds. One texel for one: NR reads the depths themselves, never a resampled or inverted one, so the depth's own convention
// (DepthInverted, which the game states) still describes what lands here.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= region_width || id.y >= region_height) return;
  // The copy is the region alone, at its own origin (what the copy path's CopyTextureRegion leaves in it), so the region's offset falls on the read.
  depth_target[id.xy] = depth_source.Load(int3(uint2(region_x, region_y) + id.xy, 0)).x;
}
