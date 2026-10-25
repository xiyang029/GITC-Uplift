#include "addon/dlss_ratio.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "nr/log.hpp"

namespace uplift::addon {
namespace {

using OptimalSettingsFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);

// The panel's per-mode render scales, whole percent (100 = follow the game), indexed by ui::DlssPresetMode
// (DLAA, Ultra Quality, Quality, Balanced, Performance, Ultra Performance). Stored as ratios.
std::atomic<float> g_scales[6] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
// The callback the block carried before the plant (the snippet's, or none): the planted callback
// rescales its answer. The capability blocks live for the process, so the pointers stay valid.
std::mutex g_mutex;
std::unordered_map<const NVSDK_NGX_Parameter*, OptimalSettingsFn> g_originals;

// ui::DlssPresetMode's index for a PerfQualityValue (nvsdk_ngx_defs.h: MaxPerf 0, Balanced 1,
// MaxQuality 2, UltraPerformance 3, UltraQuality 4, DLAA 5). 4 (Performance) for anything unknown.
unsigned ModeOf(int perf_quality) {
  switch (perf_quality) {
    case 0: return 4u;  // MaxPerf
    case 1: return 3u;  // Balanced
    case 2: return 2u;  // MaxQuality
    case 3: return 5u;  // UltraPerformance
    case 4: return 1u;  // UltraQuality
    case 5: return 0u;  // DLAA
    default: return 4u;
  }
}

// DLSS's own render ratios as denominators (the OptimalSettings helper's table), by ui::DlssPresetMode.
// Only used when the block carried no snippet callback to ask first.
float DefaultDenominator(unsigned mode) {
  constexpr float DENOMINATORS[6] = {1.f, 1.3f, 1.5f, 1.7f, 2.f, 3.f};
  return DENOMINATORS[mode];
}

// Rescales one dynamic-resolution bound the snippet answered, by how far the override moved the optimal
// size; untouched when the snippet never set it (a game then applies its own bounds).
void RescaleBound(NVSDK_NGX_Parameter* block, const char* key, double factor) {
  unsigned bound = 0u;
  if (block->Get(key, &bound) != NVSDK_NGX_Result_Success || bound == 0u) return;
  block->Set(key, (unsigned)std::llround((double)bound * factor));
}

// The callback the game's OptimalSettings helper lands in. The helper has already written the game's
// selected (output) size into Width/Height and its quality mode into PerfQualityValue; the answer goes
// back in OutWidth/OutHeight (the recommended render size) and the dynamic-resolution bounds.
NVSDK_NGX_Result NVSDK_CONV OptimalSettingsCallback(NVSDK_NGX_Parameter* block) {
  unsigned width = 0u, height = 0u;
  int perf_quality = -1;
  if (block == nullptr
      || block->Get(NVSDK_NGX_Parameter_Width, &width) != NVSDK_NGX_Result_Success
      || block->Get(NVSDK_NGX_Parameter_Height, &height) != NVSDK_NGX_Result_Success
      || block->Get(NVSDK_NGX_Parameter_PerfQualityValue, &perf_quality) != NVSDK_NGX_Result_Success
      || width == 0u || height == 0u) {
    return NVSDK_NGX_Result_Fail;
  }
  OptimalSettingsFn original = nullptr;
  {
    const std::lock_guard lock(g_mutex);
    const auto found = g_originals.find(block);
    if (found != g_originals.end()) original = found->second;
  }
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
  unsigned snippet_w = 0u, snippet_h = 0u;
  if (original != nullptr) {
    result = original(block);
    if (NVSDK_NGX_FAILED(result)) return result;
    block->Get(NVSDK_NGX_Parameter_OutWidth, &snippet_w);
    block->Get(NVSDK_NGX_Parameter_OutHeight, &snippet_h);
  }
  const unsigned mode = ModeOf(perf_quality);
  float scale = g_scales[mode].load(std::memory_order_relaxed);
  const bool overridden = (scale < 0.995f);  // the panel set a ratio for this mode
  if (!overridden) {
    if (original != nullptr && snippet_w != 0u) {
      // Follow the snippet's own answer; the first pass-through answer is logged once, later ones are noise.
      static std::atomic<bool> pass_through_logged{false};
      if (!pass_through_logged.exchange(true)) {
        nr::Logf(nr::LogLevel::INFO, "the DLSS render scale override follows the game's query: {}x{} -> {}x{}", width, height, snippet_w, snippet_h);
      }
      return result;
    }
    scale = 1.f / DefaultDenominator(mode);  // no snippet answer: DLSS's own table
  }
  // The override's answer: the game's selected size times the render scale.
  const unsigned optimal_w = (unsigned)(width * scale);
  const unsigned optimal_h = (unsigned)(height * scale);
  if (original != nullptr && snippet_w != 0u && snippet_h != 0u) {
    const double factor = (double)optimal_w / (double)snippet_w;
    RescaleBound(block, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, factor);
    RescaleBound(block, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, factor);
    RescaleBound(block, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, factor);
    RescaleBound(block, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, factor);
  } else {
    // No snippet answer to rescale: DLSS's DRS range (0.5-1.0) as the bounds.
    block->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, std::min(optimal_w, width / 2u));
    block->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, std::min(optimal_h, height / 2u));
    block->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, width);
    block->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, height);
    block->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
  }
  block->Set(NVSDK_NGX_Parameter_OutWidth, optimal_w);
  block->Set(NVSDK_NGX_Parameter_OutHeight, optimal_h);
  block->Set(NVSDK_NGX_Parameter_Scale, scale);
  block->Set(NVSDK_NGX_Parameter_SuperSampling_ScaleFactor, scale);
  // Every answer that carries the panel's ratio is logged: the game re-queries rarely, and these lines
  // are the whole evidence that the override reached it (the first pass-through answer is logged above).
  nr::Logf(nr::LogLevel::INFO, "the DLSS render scale override answered: {}x{} -> {}x{} (scale {:.2f}, {})", width, height, optimal_w, optimal_h,
           scale, (original != nullptr ? "rescaled the snippet's answer" : "DLSS's own table"));
  return NVSDK_NGX_Result_Success;
}

}  // namespace

void SetDlssRenderScales(const std::array<uint32_t, 6>& scales_percent) {
  for (size_t mode = 0u; mode < 6u; ++mode) {
    g_scales[mode].store(scales_percent[mode] / 100.f, std::memory_order_relaxed);
  }
}

void PlantDlssRatio(ngx_hooks::NgxApi api, NVSDK_NGX_Parameter* block) {
  if (block == nullptr) return;
  void* current = nullptr;
  block->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &current);
  {
    const std::lock_guard lock(g_mutex);
    if (current == reinterpret_cast<void*>(&OptimalSettingsCallback)) return;  // this block is ours already
    g_originals.insert_or_assign(block, reinterpret_cast<OptimalSettingsFn>(current));
    block->Set(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, reinterpret_cast<void*>(&OptimalSettingsCallback));
    block->Set(NVSDK_NGX_EParameter_DLSSOptimalSettingsCallback, reinterpret_cast<void*>(&OptimalSettingsCallback));
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    nr::Logf(nr::LogLevel::INFO, "the DLSS render scale override planted its OptimalSettings callback ({}; the snippet's callback {})",
             (api == ngx_hooks::NgxApi::D3D12 ? "Direct3D 12" : api == ngx_hooks::NgxApi::D3D11 ? "Direct3D 11" : "Vulkan"),
             (current != nullptr ? "was replaced" : "was missing (an old runtime); DLSS's own table fills in"));
  }
}

}  // namespace uplift::addon
