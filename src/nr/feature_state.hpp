#pragma once

#include <format>
#include <string>
#include <string_view>

#include "nr/log.hpp"
#include "nr/snippet.hpp"
#include "nr/types.hpp"

namespace uplift::nr {

[[nodiscard]] inline std::string FeatureDiagnostic(uint32_t pass, bool created, std::string_view network, bool evaluated,
                                                  uint64_t enabled_evaluations, bool enabled, uint32_t style, uint32_t mask_mode) {
  if (!created) return {};
  if (network.empty()) network = "unconfirmed (no native creation message)";
  if (!evaluated) return std::format("NR pass {} network: {} | awaiting evaluation", pass, network);
  return std::format("NR pass {} network: {} | enabled calls recorded: {} | {} | style {} | native mask mode {}",
                     pass, network, enabled_evaluations, enabled ? "enabled" : "bypassed / zero intensity", style, mask_mode);
}

// The caller has already proved GPU completion. Snippet routes ReleaseFeature to its bound API.
inline NVSDK_NGX_Result ReleaseFeatureState(const Snippet& snippet, NVSDK_NGX_Handle*& handle, Size& capacity) {
  if (handle == nullptr) return NVSDK_NGX_Result_Success;
  const NVSDK_NGX_Result result = snippet.ReleaseFeature(handle);
  if (NVSDK_NGX_FAILED(result)) Logf(LogLevel::ERR, "ReleaseFeature failed: {:#010x}", static_cast<uint32_t>(result));
  handle = nullptr;
  capacity = {};
  return result;
}

}  // namespace uplift::nr
