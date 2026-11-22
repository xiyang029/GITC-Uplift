#include "sources/nr_gpu_timing.hpp"

#include <cstring>
#include <new>

#include "sources/after_dlss_source.hpp"
#include "nr/log.hpp"

namespace uplift::sources {
namespace {

constexpr auto SAMPLE_INTERVAL = std::chrono::milliseconds(100);
constexpr auto REQUEST_LIFETIME = std::chrono::milliseconds(250);
constexpr auto SAMPLE_MAX_AGE = std::chrono::seconds(1);
constexpr size_t TIMESTAMP_COUNT = 5u;
constexpr size_t TIMESTAMP_BYTES = TIMESTAMP_COUNT * sizeof(uint64_t);

uint64_t TimestampFrequency(ID3D12CommandQueue* queue) {
  uint64_t frequency = 0u;
  return queue && SUCCEEDED(queue->GetTimestampFrequency(&frequency)) ? frequency : 0u;
}

bool CreateQueries(ID3D12Device* device, NrTimingSlot& slot) {
  const D3D12_QUERY_HEAP_DESC queries = {.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = TIMESTAMP_COUNT};
  const D3D12_HEAP_PROPERTIES heap = {.Type = D3D12_HEAP_TYPE_READBACK, .CreationNodeMask = 1u, .VisibleNodeMask = 1u};
  const D3D12_RESOURCE_DESC buffer = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = TIMESTAMP_BYTES,
      .Height = 1u, .DepthOrArraySize = 1u, .MipLevels = 1u, .SampleDesc = {1u, 0u}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  return SUCCEEDED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&slot.queries)))
      && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&slot.readback)));
}

std::optional<std::array<uint64_t, TIMESTAMP_COUNT>> ReadTicks(ID3D12Resource* readback) {
  constexpr D3D12_RANGE read = {.Begin = 0u, .End = TIMESTAMP_BYTES};
  constexpr D3D12_RANGE written = {};
  void* mapped = nullptr;
  if (FAILED(readback->Map(0u, &read, &mapped))) return std::nullopt;
  std::array<uint64_t, TIMESTAMP_COUNT> ticks;
  std::memcpy(ticks.data(), mapped, sizeof(ticks));
  readback->Unmap(0u, &written);
  return ticks;
}

}  // namespace

void NrGpuTiming::Watch(std::chrono::steady_clock::time_point now, std::bitset<nr::SESSION_STAGES> stages) {
  requested_ &= stages;
  if (unavailable_ || now < next_refresh_) return;
  next_refresh_ = now + SAMPLE_INTERVAL;
  Poll();
  if (now >= next_log_) {
    next_log_ = now + std::chrono::seconds(1);
    for (size_t stage = 0; stage < latest_.size(); ++stage) {
      const auto& sample = latest_[stage];
      if (!stages[stage] || !sample || now - sample->recorded > SAMPLE_MAX_AGE) continue;
      const auto& b = sample->breakdown;
      nr::Logf(nr::LogLevel::TRACE, "NR GPU stage {}: copy {:.3f}, encode/meter {:.3f}, evaluate {:.3f}, compose/look {:.3f}, total {:.3f} ms ({} passes)",
               stage, b.copy_ms, b.encode_ms, b.evaluate_ms, b.compose_ms, b.total_ms, sample->passes);
    }
  }
  requested_ = stages;
  request_until_ = now + REQUEST_LIFETIME;
}

NrTimingSlot* NrGpuTiming::Begin(ID3D12GraphicsCommandList* list, uint8_t stage, ID3D12CommandQueue* queue, DlssFrameHost* host) {
  if (stage >= requested_.size() || !requested_[stage]) return nullptr;
  const auto now = std::chrono::steady_clock::now();
  if (now >= request_until_) {
    requested_.reset();
    return nullptr;
  }
  requested_.reset(stage);
  if ((!queue && !host) || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return nullptr;
  const uint64_t frequency = TimestampFrequency(queue);
  if (queue && !frequency) return nullptr;
  for (auto& slot : slots_) {
    // A list can execute again after we collected its first sample. Recheck the mark and
    // take a fresh baseline so a later discarded list cannot pass off that resolve as its own.
    if (slot && (slot->pending || slot.use_count() != 1 || !timeline_.IsExecutionComplete(slot->mark))) continue;
    try {
      if (!slot) {
        auto created = std::make_shared<NrTimingSlot>();
        if (!CreateQueries(device_, *created)) {
          unavailable_ = true;
          requested_.reset();
          return nullptr;
        }
        slot = std::move(created);
      }
      const auto previous = ReadTicks(slot->readback.Get());
      if (!previous) continue;
      slot->previous_start = (*previous)[0];
      if (host) host->HoldTiming(slot);
    } catch (const std::bad_alloc&) {
      return nullptr;
    }
    slot->mark = timeline_.MarkNow();
    slot->stage = stage;
    slot->frequency = frequency;
    slot->recorded = now;
    slot->passes = 0u;
    slot->pending = true;
    slot->checkpoints.reset();
    slot->checkpoints.set(0u);
    list->EndQuery(slot->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0u);
    return slot.get();
  }
  return nullptr;
}

void NrGpuTiming::End(NrTimingSlot* slot, ID3D12GraphicsCommandList* list, uint32_t passes) {
  if (!slot) return;
  Checkpoint(slot, list, 3u);
  slot->passes = passes;
}

void NrGpuTiming::Checkpoint(NrTimingSlot* slot, ID3D12GraphicsCommandList* list, uint32_t index) {
  if (!slot || index >= TIMESTAMP_COUNT) return;
  list->EndQuery(slot->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
  slot->checkpoints.set(index);
}

void NrGpuTiming::Finish(NrTimingSlot* slot, ID3D12GraphicsCommandList* list) {
  if (!slot) return;
  Checkpoint(slot, list, 4u);
  // Failed/skipped recordings can have no encode or evaluation endpoint. Never resolve unwritten queries.
  if (slot->checkpoints.all() && slot->passes) {
    list->ResolveQueryData(slot->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0u, TIMESTAMP_COUNT, slot->readback.Get(), 0u);
  } else {
    slot->passes = 0u;
  }
}

void NrGpuTiming::SetSubmissionQueue(std::span<const std::shared_ptr<NrTimingSlot>> slots, ID3D12CommandQueue* queue) {
  if (slots.empty()) return;
  const uint64_t frequency = TimestampFrequency(queue);
  for (const auto& slot : slots) slot->frequency = frequency;
}

void NrGpuTiming::Poll() {
  for (auto& slot : slots_) {
    if (!slot || !slot->pending || !timeline_.IsExecutionComplete(slot->mark)) continue;
    if (!slot->passes || !slot->frequency) {
      slot->pending = false;
      continue;
    }
    const auto ticks = ReadTicks(slot->readback.Get());
    if (!ticks) continue;
    const auto start = (*ticks)[0];
    const auto end = (*ticks)[4];
    slot->pending = false;
    auto& latest = latest_[slot->stage];
    if (start != slot->previous_start && end > start && (!latest || latest->recorded < slot->recorded)) {
      bool ordered = true;
      for (size_t i = 1u; i < ticks->size(); ++i) ordered &= (*ticks)[i] >= (*ticks)[i-1u];
      if (!ordered) continue;
      const auto elapsed = [&](size_t a, size_t b) { return 1000. * double((*ticks)[b]-(*ticks)[a]) / double(slot->frequency); };
      latest = NrGpuSample{elapsed(2u,3u), slot->passes, slot->recorded,
                           {elapsed(0u,1u),elapsed(1u,2u),elapsed(2u,3u),elapsed(3u,4u),elapsed(0u,4u)}};
    }
  }
}

std::optional<double> NrGpuTiming::Latest(uint8_t stage, uint32_t passes, std::chrono::steady_clock::time_point now) const {
  if (stage >= latest_.size()) return std::nullopt;
  const auto& sample = latest_[stage];
  if (!sample || sample->passes != passes || now - sample->recorded > SAMPLE_MAX_AGE) return std::nullopt;
  return sample->milliseconds;
}

std::optional<NrGpuBreakdown> NrGpuTiming::LatestBreakdown(uint8_t stage, uint32_t passes, std::chrono::steady_clock::time_point now) const {
  if (!Latest(stage, passes, now)) return std::nullopt;
  return latest_[stage]->breakdown;
}

}  // namespace uplift::sources
