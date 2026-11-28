#pragma once

#include <sl_core_api.h>

#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>

namespace uplift::addon::slnr {

[[nodiscard]] inline bool IsViewport(const sl::BaseStructure& input) {
  return input.structType == sl::ViewportHandle::s_structType && input.structVersion == sl::kStructVersion1;
}

[[nodiscard]] inline bool FreedNrViewport(sl::Feature feature, const sl::BaseStructure& viewport, sl::Result result) {
  return feature == sl::kFeatureDLSS_NR && result == sl::Result::eOk && IsViewport(viewport);
}

[[nodiscard]] inline const sl::ViewportHandle* FindEvaluateViewport(const sl::BaseStructure* const* inputs, uint32_t count) {
  if (!inputs) return nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    for (auto* input = inputs[i]; input; input = input->next) {
      if (IsViewport(*input)) return static_cast<const sl::ViewportHandle*>(input);
    }
  }
  return nullptr;
}

// A rejected provider call does not change the last accepted preset. Back off retries
// of the same panel choice; a new choice can be submitted immediately.
struct PresetSubmission {
  using Clock = std::chrono::steady_clock;
  std::optional<uint32_t> accepted;
  std::optional<uint32_t> refused;
  std::optional<uint32_t> failed;
  Clock::time_point retry_at{};
  uint32_t effective = 0u;

  [[nodiscard]] bool ShouldReplay(uint32_t wanted, Clock::time_point now) const {
    if (failed == wanted) return now >= retry_at;
    // If a different choice failed, the provider may have changed partial state;
    // reassert the new choice even if it was the last accepted one.
    return failed.has_value() || accepted != wanted;
  }
  void Complete(uint32_t wanted, uint32_t submitted, bool restored, sl::Result result, Clock::time_point now) {
    if (result != sl::Result::eOk) {
      failed = wanted;
      retry_at = now + std::chrono::seconds(1);
      return;
    }
    if (restored) refused = wanted;
    else if (accepted != wanted) refused.reset();
    accepted = wanted;
    effective = submitted;
    failed.reset();
  }
};

// Stores value copies only. Callers serialize access alongside the provider pointer.
// A Submit holds a snapshot alive if a reentrant free detaches it from this cache.
template<class Options>
class ViewportOptions {
 public:
  std::shared_ptr<Options> Find(uint32_t id) const {
    const auto found = values_.find(id);
    return found == values_.end() ? nullptr : found->second;
  }
  std::shared_ptr<Options> GetOrInsert(uint32_t id) {
    if (auto cached = Find(id)) return cached;
    if (values_.size() >= 8u) return nullptr;
    return values_.emplace(id, std::make_shared<Options>()).first->second;
  }
  void Erase(uint32_t id) { values_.erase(id); }
  void Clear() { values_.clear(); }

 private:
  std::unordered_map<uint32_t, std::shared_ptr<Options>> values_;
};

}  // namespace uplift::addon::slnr
