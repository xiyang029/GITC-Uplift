#pragma once

#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "nr/timeline.hpp"

namespace uplift::nr {

// Frame skip (MFG-Unlocker's Boost, ported): what the GPU really spends on NR's work, measured with timestamp queries
// instead of inferred from the frame rate. One pair of stamps per frame, resolved into a readback slot of its own and read
// back once the timeline says that frame is done, so a skipped frame's saving is a number rather than a guess. Cheap enough
// to leave on: two queries, one resolve and one Map per frame, and the total is only read when a report is asked for.
//
// Eight channels share the heap. NR's own work tells the frames the network runs apart from the ones it skips; the chain
// channel wraps everything Uplift records for a frame, so the add-on's own cost is told apart from the network's; copy,
// carry and compose split a skipped frame's own work up, which is what says whether making it cheaper is worth
// anything; and fit, solve and hold split a network frame's look model up, which is what says whether it is worth
// fitting less often. Channels nest -- each keeps an open slot of its own -- so one channel's pair can sit inside
// another's.
class GpuTimer {
 public:
  // NR and CHAIN say what a frame costs; the other six split one side of it up -- copy, carry and compose a skipped
  // frame, fit, solve and hold a network one.
  enum class Channel : uint32_t { NR = 0u, CHAIN = 1u, CARRY = 2u, COMPOSE = 3u, FIT = 4u, SOLVE = 5u, HOLD = 6u, COUNT = 7u };

  GpuTimer() = default;
  ~GpuTimer() {
    // A readback queued on the timeline never touches a timer that is gone: the device tears the add-on down while
    // releases may still be pending.
    if (alive_) {
      alive_->store(false);
    }
  }
  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  // Frames measured since the last Take, and their average, in milliseconds of GPU time a frame.
  struct Metrics {
    uint32_t frames = 0u;
    double ms = 0.0;
  };
  // What the frames measured since the last Take averaged.
  struct Report {
    Metrics network;  // NR's own: the frames the network ran
    Metrics skipped;  // NR's own: the ones it did not
    Metrics chain;    // everything Uplift recorded for the frame, NR included
    Metrics carry;    // within a skipped frame: the model carried along the cycle's motion, and
    Metrics compose;  // ... the cycle's motion composed for the network's own frame
    Metrics fit;      // within a network frame: the sums the fit makes, and
    Metrics solve;    // ... the model solved from them, and
    Metrics hold;     // ... the detail that model does not explain
  };

  // A heap or buffer that cannot be made leaves the timer inert (Ready() false): NR then runs exactly as it did before.
  bool Initialize(ID3D12Device* device, Timeline* timeline) {
    if (device == nullptr || timeline == nullptr) {
      return false;
    }
    D3D12_QUERY_HEAP_DESC heap = {};
    heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap.Count = SLOTS * 2u;
    if (FAILED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&heap_)))) {
      return false;
    }
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC description = {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = SLOTS * 2u * sizeof(uint64_t);
    description.Height = 1u;
    description.DepthOrArraySize = 1u;
    description.MipLevels = 1u;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.SampleDesc.Count = 1u;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback_)))) {
      heap_.Reset();
      return false;
    }
    timeline_ = timeline;
    return true;
  }
  [[nodiscard]] bool Ready() const { return readback_ != nullptr; }

  // The queue the stamps are timed on: its timestamp frequency is what turns ticks into milliseconds. NR's work is submitted
  // to one queue, so the present's is the one to name. Inert for a queue that has no frequency of its own.
  void NoteQueue(ID3D12CommandQueue* queue) {
    if (queue == nullptr || queue == queue_) {
      return;
    }
    uint64_t frequency = 0u;
    if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0u) {
      return;
    }
    queue_ = queue;
    frequency_ = frequency;
  }

  // Both on the same list, Begin before End. `skipped` is whether this frame's NR work was the carried kind, which is what
  // the report compares the network's own frames against.
  void Begin(ID3D12GraphicsCommandList* list, Channel channel = Channel::NR) {
    if (!Ready() || list == nullptr || channel >= Channel::COUNT) {
      return;
    }
    const uint32_t which = static_cast<uint32_t>(channel);
    const uint32_t local = channels_[which].next;
    channels_[which].next = (local + 1u) % SLOTS_PER_CHANNEL;
    channels_[which].open = local;
    list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, SlotOf(which, local) * 2u);
  }
  void End(ID3D12GraphicsCommandList* list, bool skipped, Channel channel = Channel::NR) {
    if (!Ready() || list == nullptr || channel >= Channel::COUNT) {
      return;
    }
    const uint32_t which = static_cast<uint32_t>(channel);
    if (channels_[which].open == EMPTY) {
      return;  // nothing was open: a Begin the device refused, or an End without one
    }
    const uint32_t slot = SlotOf(which, channels_[which].open);
    channels_[which].open = EMPTY;
    slots_[slot] = {.channel = channel, .skipped = skipped};
    list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u + 1u);
    list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u, 2u, readback_.Get(), slot * 2u * sizeof(uint64_t));
    // Read back only once the timeline says that frame is done: Map before that would return the slot's previous contents.
    const std::shared_ptr<std::atomic<bool>> alive = alive_;
    timeline_->ReleaseAfter(timeline_->MarkNow(), [this, slot, alive] {
      if (alive->load()) {
        Collect(slot);
      }
    });
  }

  // The average since the last call, which resets it. Empty until something was measured.
  [[nodiscard]] std::optional<Report> Take() {
    const auto average = [](const Sum& sum) {
      return Metrics{.frames = sum.frames, .ms = (sum.frames > 0u ? sum.ms / static_cast<double>(sum.frames) : 0.0)};
    };
    const Report report = {.network = average(network_), .skipped = average(skipped_), .chain = average(chain_),
                           .carry = average(carry_),   .compose = average(compose_),
                           .fit = average(fit_),       .solve = average(solve_),       .hold = average(hold_)};
    if (report.network.frames == 0u && report.skipped.frames == 0u && report.chain.frames == 0u) {
      return std::nullopt;
    }
    network_ = {};
    skipped_ = {};
    chain_ = {};
    carry_ = {};
    compose_ = {};
    fit_ = {};
    solve_ = {};
    hold_ = {};
    return report;
  }

 private:
  // Enough slots that a GPU several frames behind still reads its own stamps rather than a later frame's, per channel.
  static constexpr uint32_t SLOTS_PER_CHANNEL = 16u;
  static constexpr uint32_t SLOTS = SLOTS_PER_CHANNEL * static_cast<uint32_t>(Channel::COUNT);
  static constexpr uint32_t EMPTY = SLOTS_PER_CHANNEL;  // an open slot, when a channel has none

  struct Sum {
    double ms = 0.0;
    uint32_t frames = 0u;
  };
  struct Slot {
    Channel channel = Channel::NR;
    bool skipped = false;
  };
  // One channel's state: the slot its next Begin takes, and the one an End is still waiting for.
  struct Open {
    uint32_t next = 0u;
    uint32_t open = EMPTY;
  };

  [[nodiscard]] static constexpr uint32_t SlotOf(uint32_t channel, uint32_t local) { return channel * SLOTS_PER_CHANNEL + local; }

  [[nodiscard]] Sum& Bucket(const Slot& which) {
    switch (which.channel) {
      case Channel::CHAIN: return chain_;
      case Channel::CARRY: return carry_;
      case Channel::COMPOSE: return compose_;
      case Channel::FIT: return fit_;
      case Channel::SOLVE: return solve_;
      case Channel::HOLD: return hold_;
      default: return which.skipped ? skipped_ : network_;
    }
  }

  void Collect(uint32_t slot) {
    if (frequency_ == 0u || readback_ == nullptr) {
      return;
    }
    void* mapped = nullptr;
    if (FAILED(readback_->Map(0u, nullptr, &mapped)) || mapped == nullptr) {
      return;
    }
    const auto* ticks = static_cast<const uint64_t*>(mapped);
    const uint64_t start = ticks[slot * 2u];
    const uint64_t finish = ticks[slot * 2u + 1u];
    readback_->Unmap(0u, nullptr);
    if (finish < start) {
      return;
    }
    const double ms = static_cast<double>(finish - start) * 1000.0 / static_cast<double>(frequency_);
    if (ms > 1000.0) {
      return;  // a pair whose resolve has not run yet: not a frame time by any measure
    }
    Sum& sum = Bucket(slots_[slot]);
    sum.ms += ms;
    ++sum.frames;
  }

  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  Timeline* timeline_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  uint64_t frequency_ = 0u;
  std::array<Open, static_cast<uint32_t>(Channel::COUNT)> channels_ = {};
  std::array<Slot, SLOTS> slots_ = {};
  Sum network_;  // NR's own frames, against
  Sum skipped_;
  Sum chain_;  // everything Uplift records for the frame, against
  Sum carry_;
  Sum compose_;
  Sum fit_;    // what a network frame spends on its look model, the three told apart
  Sum solve_;
  Sum hold_;
};

}  // namespace uplift::nr
