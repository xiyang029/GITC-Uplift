#pragma once

#include <array>
#include <bitset>
#include <memory>
#include <optional>

#include "nr/session_status.hpp"
#include "nr/timeline.hpp"

namespace uplift::sources {

class DlssFrameHost;

// A game list may execute again without a reset. Keep its queries and destination separate
// from other recordings until the list releases its reference and the timeline passes its work.
struct NrTimingSlot {
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  nr::Mark mark;
  uint64_t frequency = 0u, previous_start = 0u;
  std::chrono::steady_clock::time_point recorded;
  uint8_t stage = 0u;
  uint32_t passes = 0u;
  bool pending = false;
};

struct NrGpuSample {
  double milliseconds;
  uint32_t passes;
  std::chrono::steady_clock::time_point recorded;
};

// Owned Present lists use their frame fence. DLSS lists use the existing submission token and
// the submitting queue's timestamp frequency. Poll only reads completed queries, without waiting.
class NrGpuTiming {
 public:
  NrGpuTiming(ID3D12Device* device, const nr::Timeline& timeline) : device_(device), timeline_(timeline) {}
  // Called by the visible overlay. Each refresh requests at most one recording per stage.
  void Watch(std::chrono::steady_clock::time_point now, std::bitset<nr::SESSION_STAGES> stages);
  NrTimingSlot* Begin(ID3D12GraphicsCommandList* list, uint8_t stage, ID3D12CommandQueue* queue, DlssFrameHost* host);
  void End(NrTimingSlot* slot, ID3D12GraphicsCommandList* list, uint32_t passes);
  // A null queue invalidates an uncollected sample when its list is executed again.
  static void SetSubmissionQueue(std::span<const std::shared_ptr<NrTimingSlot>> slots, ID3D12CommandQueue* queue);
  [[nodiscard]] std::optional<double> Latest(uint8_t stage, uint32_t passes, std::chrono::steady_clock::time_point now) const;

 private:
  void Poll();

  ID3D12Device* device_;
  const nr::Timeline& timeline_;
  std::array<std::shared_ptr<NrTimingSlot>, 12> slots_;
  std::bitset<nr::SESSION_STAGES> requested_;
  std::chrono::steady_clock::time_point next_refresh_{}, request_until_{};
  std::array<std::optional<NrGpuSample>, nr::SESSION_STAGES> latest_;
  bool unavailable_ = false;
};

}  // namespace uplift::sources
