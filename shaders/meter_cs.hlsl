#include "change_common.hlsli"
#include "vk_common.hlsli"

// The exposure meter and governor, one thread group before the encode. The state
// texture's x is the encode's and decode's governed final multiplier; y is the independent meter used by Auto's validity check.
// The state is 2x1. With `probe` (Input exposure = Auto while the game passes an exposure) the second texel holds
// the game's exposure as the encode reads it, beside the meter's target, for the add-on's readback.
cbuffer MeterConstants : register(b0) {
  uint encoding;
  uint primaries;
  float input_scale;
  uint source_x;         // the region the encode reads
  uint source_y;
  uint source_width;
  uint source_height;
  uint snap;             // jump to the target: new exposure state or a changed metering basis
  uint smooth;           // ExposureAdapt: 0 Off (the target every frame), 1 Smooth
  float brighter_rate;   // stops per second toward a darker exposure
  float darker_rate;     // stops per second toward a brighter exposure
  float frame_seconds;   // Δt
  uint probe;            // write texel (1, 0)
  uint has_game_texture;  // the game's ExposureTexture is bound at t2
  float game_factor;     // DLSS.Exposure.Scale ÷ DLSS.Pre.Exposure
  float game_weight;     // Auto with `probe`: the game's share, in stops, of the multiplier at (0, 0).x (1 − AutoExposureBlend); 0 the meter alone
};

// On Vulkan's After DLSS the source is DLSS's output, read in place from its storage view (u2), as the encode reads it.
#if UPLIFT_VK_SOURCE_STORAGE
UPLIFT_IMAGE_FORMAT(UPLIFT_VK_TARGET_FORMAT) RWTexture2D<float4> source_texture : register(u2);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture[uint2(pixel)]
#else
Texture2D<float4> source_texture : register(t0);
#define UPLIFT_LOAD_SOURCE(pixel) source_texture.Load(int3((pixel), 0))
#endif
UPLIFT_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> state_texture : register(u0);  // (final multiplier, independent meter EV, last anchor, set)
Texture2D<float> exposure_texture : register(t2);  // the game's exposure, as the encode binds it

static const uint BINS = 96u;
static const float LOW_STOPS = -16.f;
static const float BIN_STOPS = 0.25f;
static const uint GRID_WIDTH = 64u;
static const uint GRID_HEIGHT = 36u;
static const uint THREADS = 256u;
static const float TARGET_LIMIT = 10.f;
static const float DEADBAND = 0.1f;
static const float MIDDLE_GREY = 0.18f;

groupshared uint g_bins[BINS];

float GovernExposure(float previous, float target, bool reset) {
  if (reset) return target;
  const float delta = target - previous;
  if (abs(delta) <= DEADBAND) return previous;
  const float seconds = clamp(frame_seconds, 0.001f, 0.1f);
  return previous + clamp(delta, -brighter_rate * seconds, darker_rate * seconds);
}


[numthreads(256, 1, 1)]
void main(uint3 group_thread : SV_GroupThreadID) {
  if (group_thread.x < BINS) {
    g_bins[group_thread.x] = 0u;
  }
  GroupMemoryBarrierWithGroupSync();
  const uint2 region = uint2(source_width, source_height);
  for (uint cell = group_thread.x; cell < GRID_WIDTH * GRID_HEIGHT; cell += THREADS) {
    const float2 grid = (float2(cell % GRID_WIDTH, cell / GRID_WIDTH) + 0.5f) / float2(GRID_WIDTH, GRID_HEIGHT);
    const uint2 texel = uint2(source_x, source_y) + min(uint2(grid * float2(region)), region - 1u);
    const float3 light = LinearLight(encoding, primaries, UPLIFT_LOAD_SOURCE(texel).rgb, input_scale);
    const float stops = log2(max(Luminance(light), 0.f) + exp2(LOW_STOPS));
    const uint bin = min(uint(max((stops - LOW_STOPS) / BIN_STOPS, 0.f)), BINS - 1u);
    InterlockedAdd(g_bins[bin], 1u);
  }
  GroupMemoryBarrierWithGroupSync();
  if (group_thread.x != 0u) return;
  uint total = 0u;
  for (uint counted = 0u; counted < BINS; ++counted) {
    total += g_bins[counted];
  }
  const float low = 0.4f * float(total);
  const float high = 0.9f * float(total);
  float below = 0.f;
  float sum = 0.f;
  for (uint bin_index = 0u; bin_index < BINS; ++bin_index) {
    const float count = float(g_bins[bin_index]);
    sum += max(min(below + count, high) - max(below, low), 0.f) * (LOW_STOPS + (float(bin_index) + 0.5f) * BIN_STOPS);
    below += count;
  }
  const float anchor = (total > 0u ? sum / (high - low) : log2(MIDDLE_GREY));
  const float target = clamp(log2(MIDDLE_GREY) - anchor, -TARGET_LIMIT, TARGET_LIMIT);
  const float4 previous = state_texture[uint2(0, 0)];
  const bool was_set = (previous.w > 0.5f && previous.x > 0.f && all(isfinite(previous)));
  // Keep the independent meter for Auto's validity window. The applied exposure follows the blended raw target once,
  // so game exposure changes cannot bypass the governor and the meter is not smoothed twice.
  const bool reset = (snap != 0u || !was_set || smooth == 0u);
  const float exposure = GovernExposure(previous.y, target, reset);
  float applied_target = target;
  if (probe != 0u) {
    const float texel = (has_game_texture != 0u ? exposure_texture.Load(int3(0, 0, 0)) : 1.f);
    const float game = GameExposure(has_game_texture, texel, game_factor);
    state_texture[uint2(1, 0)] = float4(texel, game_factor, game, target);
    applied_target += saturate(game_weight) * (log2(game) - target);
  }
  // At Auto's 0% endpoint the encode binds the game directly. Mirror that value in x so a later fallback starts
  // from the exposure actually used, while y keeps metering independently. Scene cuts use the ordinary rate limits.
  const bool direct_game = (probe != 0u && game_weight >= 1.f);
  const float applied = GovernExposure(log2(max(previous.x, 1e-20f)), applied_target, reset || direct_game);
  state_texture[uint2(0, 0)] = float4(exp2(applied), exposure, anchor, 1.f);
}
