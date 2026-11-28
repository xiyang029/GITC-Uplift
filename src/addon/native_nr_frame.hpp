#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace uplift::addon {

enum class NativeNrPath : uint8_t { AFTER, REPLACE, DISABLED_FALLBACK };

// Scoped to one Present interval. Failed or disabled native calls alone never claim the image.
struct NativeNrFrame {
  bool native_applied = false;
  // Successful original calls can block After/Replace only for their own output.
  // No resource ownership is retained; these are identity hints for one Present interval.
  std::array<const void*, 8> game_outputs{};
  bool ordinary_dlss_applied = false;
  bool deferred_to_native = false;

  void MarkGameOutput(const void* output) {
    if (output == nullptr || GameOwns(output)) return;
    const auto free = std::ranges::find(game_outputs, nullptr);
    if (free != game_outputs.end()) *free = output;
  }
  [[nodiscard]] bool GameOwns(const void* output = nullptr) const {
    if (output == nullptr) return game_outputs.front() != nullptr;
    return std::ranges::find(game_outputs, output) != game_outputs.end();
  }
  [[nodiscard]] bool NativeOwns() const { return native_applied || GameOwns(); }
  [[nodiscard]] bool CanApplyNative(const void* output) const {
    // One Uplift temporal chain per primary frame, irrespective of the number of native evaluations.
    return !native_applied && !GameOwns(output) && !ordinary_dlss_applied;
  }
  [[nodiscard]] bool ReservesGameListRoute(bool native_mode, bool live) const {
    return NativeOwns() || (native_mode && (live || deferred_to_native));
  }
  [[nodiscard]] bool NeedsPresentFallback(bool native_mode, bool live) const {
    return ReservesGameListRoute(native_mode, live) && !NativeOwns() && !ordinary_dlss_applied;
  }
  [[nodiscard]] bool DeferOrdinaryDlss(bool native_mode, bool live) {
    if (!ReservesGameListRoute(native_mode, live)) return false;
    deferred_to_native = true;
    return true;
  }
  [[nodiscard]] NativeNrFrame Close() { return std::exchange(*this, {}); }
};

}  // namespace uplift::addon
