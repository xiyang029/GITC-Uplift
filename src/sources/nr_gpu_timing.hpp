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
  std::bitset<5> checkpoints;
};

struct NrGpuBreakdown {
  double copy_ms = 0., encode_ms = 0., evaluate_ms = 0., compose_ms = 0., total_ms = 0.;
};

struct NrGpuSample {
  double milliseconds;
  uint32_t passes;
  std::chrono::steady_clock::time_point recorded;
  NrGpuBreakdown breakdown;
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
  void Checkpoint(NrTimingSlot* slot, ID3D12GraphicsCommandList* list, uint32_t index);
  void Finish(NrTimingSlot* slot, ID3D12GraphicsCommandList* list);
  class Recording {
   public:
    Recording(NrGpuTiming& timing, ID3D12GraphicsCommandList* list, uint8_t stage, ID3D12CommandQueue* queue, DlssFrameHost* host)
        : timing_(timing), list_(list), sample_(timing.Begin(list, stage, queue, host)) {}
    ~Recording() { timing_.Finish(sample_, list_); }
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    void Copied() { timing_.Checkpoint(sample_, list_, 1u); }
    void Encoded() { timing_.Checkpoint(sample_, list_, 2u); }
    NrTimingSlot* Sample() const { return sample_; }
   private:
    NrGpuTiming& timing_;
    ID3D12GraphicsCommandList* list_;
    NrTimingSlot* sample_;
  };
  // A null queue invalidates an uncollected sample when its list is executed again.
  static void SetSubmissionQueue(std::span<const std::shared_ptr<NrTimingSlot>> slots, ID3D12CommandQueue* queue);
  [[nodiscard]] std::optional<double> Latest(uint8_t stage, uint32_t passes, std::chrono::steady_clock::time_point now) const;
  [[nodiscard]] std::optional<NrGpuBreakdown> LatestBreakdown(uint8_t stage, uint32_t passes, std::chrono::steady_clock::time_point now) const;

 private:
  void Poll();

  ID3D12Device* device_;
  const nr::Timeline& timeline_;
  std::array<std::shared_ptr<NrTimingSlot>, 12> slots_;
  std::bitset<nr::SESSION_STAGES> requested_;
  std::chrono::steady_clock::time_point next_refresh_{}, request_until_{};
  std::chrono::steady_clock::time_point next_log_{};
  std::array<std::optional<NrGpuSample>, nr::SESSION_STAGES> latest_;
  bool unavailable_ = false;
};

}  // namespace uplift::sources
