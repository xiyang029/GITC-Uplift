#include "vk/nr_timeline.hpp"

#include <Windows.h>

#include <algorithm>
#include <format>
#include <utility>

#include "nr/log.hpp"
#include "nr/vk_handles.hpp"
#include "text/nr_messages.hpp"

namespace uplift::vk {

NrCompletion::NrCompletion(const NrFunctions& functions, VkDevice device, QueueSignal signal)
    : functions_(functions), device_(device), signal_(std::move(signal)) {}

NrCompletion::~NrCompletion() {
  FreeAll();
}

std::unique_ptr<NrCompletion> NrCompletion::Create(const NrFunctions& functions, VkDevice device, QueueSignal signal, std::string* error) {
  if (device == VK_NULL_HANDLE || !signal) {
    *error = "NrCompletion needs a device and a signal function";
    return nullptr;
  }
  std::unique_ptr<NrCompletion> completion(new NrCompletion(functions, device, std::move(signal)));
  const VkSemaphoreTypeCreateInfo type = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0u,
  };
  const VkSemaphoreCreateInfo create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
  if (const VkResult result = functions.vkCreateSemaphore(device, &create, nullptr, &completion->semaphore_); result != VK_SUCCESS) {
    *error = std::format("vkCreateSemaphore (timeline) returned VkResult {}", static_cast<int>(result));
    return nullptr;
  }
  NrCompletion* const self = completion.get();
  completion->timeline_ = std::make_unique<nr::Timeline>(
      [self]() -> uint64_t {
        // Freed or idle means no work remains in flight. A lost device reports maximum completion, as a removed D3D12 fence does,
        // so marks never wait on a semaphore that can no longer be signalled.
        if (self->freed_.load(std::memory_order_acquire) || self->idle_.load(std::memory_order_acquire) || self->lost_.load(std::memory_order_acquire)) {
          return UINT64_MAX;
        }
        uint64_t value = 0u;
        const VkResult result = self->functions_.vkGetSemaphoreCounterValue(self->device_, self->semaphore_, &value);
        if (result == VK_ERROR_DEVICE_LOST) {
          self->lost_.store(true, std::memory_order_release);
          return UINT64_MAX;
        }
        return (result == VK_SUCCESS ? value : 0u);
      },
      [self](ID3D12CommandQueue* queue, uint64_t value) {
        if (self->freed_.load(std::memory_order_acquire) || self->lost_.load(std::memory_order_acquire) || self->idle_.load(std::memory_order_acquire)) return;
        if (!self->signal_(nr::VkQueueOf(queue), self->semaphore_, value)) {
          if (!self->signal_failed_logged_.exchange(true)) {
            Logf(nr::LogLevel::WARN, "the Vulkan frame semaphore's signal could not be submitted (frame {}); marks wait for the next one", value);
          }
          return;
        }
        // The highest value whose signal was submitted: Wait never waits past it (no wait on a signal that was never submitted).
        uint64_t seen = self->submitted_.load();
        while (seen < value && !self->submitted_.compare_exchange_weak(seen, value)) {
        }
      },
      [self](uint64_t value, std::chrono::milliseconds timeout) {
        if (self->freed_.load(std::memory_order_acquire) || self->idle_.load(std::memory_order_acquire) || self->lost_.load(std::memory_order_acquire)) {
          return true;
        }
        if (value > self->submitted_.load()) return false;
        const VkSemaphoreWaitInfo wait = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1u,
            .pSemaphores = &self->semaphore_,
            .pValues = &value,
        };
        const auto nanoseconds = static_cast<uint64_t>(std::max<int64_t>(timeout.count(), 0)) * 1'000'000ull;
        const VkResult result = self->functions_.vkWaitSemaphores(self->device_, &wait, nanoseconds);
        if (result == VK_ERROR_DEVICE_LOST) {
          self->lost_.store(true, std::memory_order_release);
        }
        return result == VK_SUCCESS;
      });
  completion->timeline_->SetTokenProbe([self](uint64_t token) { return self->EventSet(token); });
  return completion;
}

bool NrCompletion::EventSet(uint64_t token) {
  if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire) || idle_.load(std::memory_order_acquire)) return true;
  const std::scoped_lock lock(mutex_);
  const auto found = entries_.find(token);
  // No entry: the event was not made yet (the recording is still going on) or could not be made. It remains outstanding.
  if (found == entries_.end() || found->second.event == VK_NULL_HANDLE) return false;
  const VkResult status = functions_.vkGetEventStatus(device_, found->second.event);
  if (status == VK_ERROR_DEVICE_LOST) {
    lost_.store(true, std::memory_order_release);
    return true;
  }
  return status == VK_EVENT_SET;
}

bool NrCompletion::PollDeviceLost() {
  if (lost_.load(std::memory_order_acquire)) return true;
  if (freed_.load(std::memory_order_acquire)) return false;
  uint64_t value = 0u;
  if (functions_.vkGetSemaphoreCounterValue(device_, semaphore_, &value) == VK_ERROR_DEVICE_LOST) {
    lost_.store(true, std::memory_order_release);
  }
  return lost_.load(std::memory_order_acquire);
}

VkEvent NrCompletion::EventFor(uint64_t token, std::chrono::steady_clock::time_point now) {
  const std::scoped_lock lock(mutex_);
  if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
  // One event per token, whoever asks twice.
  if (const auto found = entries_.find(token); found != entries_.end() && found->second.event != VK_NULL_HANDLE) return found->second.event;
  VkEvent event = VK_NULL_HANDLE;
  if (!pool_.empty()) {
    event = pool_.back();
    pool_.pop_back();
  } else {
    const VkEventCreateInfo create = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    if (const VkResult result = functions_.vkCreateEvent(device_, &create, nullptr, &event); result != VK_SUCCESS) {
      Logf(nr::LogLevel::WARN, "vkCreateEvent returned VkResult {}; the token stays outstanding until the command buffer is recycled", static_cast<int>(result));
      event = VK_NULL_HANDLE;
    }
  }
  Entry& entry = entries_[token];
  entry.event = event;
  entry.issued = now;
  entry.issue_frame = timeline_->CurrentFrame();
  if (auto motion = entry.motion.lock()) motion->event_ = event;
  return event;
}

std::shared_ptr<NrCompletion::MotionProducer> NrCompletion::TrackMotion(uint64_t token) {
  const std::scoped_lock lock(mutex_);
  auto& entry = entries_[token];
  if (auto motion = entry.motion.lock()) return motion;
  auto motion = std::make_shared<MotionProducer>();
  entry.motion = motion;
  return motion;
}

bool NrCompletion::OrderedBefore(const MotionProducer& motion, VkQueue queue) const {
  const std::scoped_lock lock(mutex_);
  if (queue == VK_NULL_HANDLE || motion.queue_ != queue || freed_.load() || lost_.load()) return false;
  // ReShade's callback is before the native submit. The same calling thread cannot be in both the
  // submit and the present; on other threads require that the recorded event actually ran.
  return motion.completed_ || motion.thread_ == GetCurrentThreadId()
      || (motion.event_ != VK_NULL_HANDLE && functions_.vkGetEventStatus(device_, motion.event_) == VK_EVENT_SET);
}

void NrCompletion::Executed(std::span<const uint64_t> tokens, VkQueue native_queue) {
  const std::scoped_lock lock(mutex_);
  for (const uint64_t token : tokens) {
    if (const auto found = entries_.find(token); found != entries_.end()) {
      found->second.executed = true;
      if (auto motion = found->second.motion.lock()) {
        motion->queue_ = native_queue;
        motion->thread_ = GetCurrentThreadId();
      }
    }
  }
}

void NrCompletion::Reexecuted(std::span<const uint64_t> tokens, uint64_t first_token, void* queue, VkQueue native_queue) {
  if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire)) return;
  // The executable buffer may have SIMULTANEOUS_USE. Its event may still be SET from an earlier run,
  // and resetting it from the host can race the GPU. Only recycle proves all executions ended.
  Executed(tokens, native_queue);
  {
    const std::scoped_lock lock(mutex_);
    for (const uint64_t token : tokens) {
      if (const auto found = entries_.find(token); found != entries_.end()) {
        if (const auto motion = found->second.motion.lock()) {
          // The queue still holds: this execution is submitted there, so everything recorded before it, the copies included, is ordered before any
          // later work on that queue -- OrderedBefore's same-thread rule covers the present, which runs after this submit on this thread. Only the
          // event stops being a proof: it may still be SET from the earlier run and can be neither reset nor re-recorded here. Clearing the queue
          // instead (the old behaviour) dropped every motion copy of a replayed list for good, since Recycle can only prove the replay token's
          // event, and that token has none.
          motion->event_ = VK_NULL_HANDLE;
          motion->thread_ = GetCurrentThreadId();
        }
      }
    }
  }
  const auto now = std::chrono::steady_clock::now();
  const uint64_t fresh = timeline_->Resubmit(first_token, nr::AsQueue(queue), GetCurrentThreadId(), now);
  const std::scoped_lock lock(mutex_);
  if (!freed_.load(std::memory_order_acquire)) {
    entries_[fresh] = {.issued = now, .issue_frame = timeline_->CurrentFrame(), .executed = true, .owner = first_token};
  }
}

void NrCompletion::Recycle(std::span<const uint64_t> tokens) {
  std::vector<uint64_t> dropped(tokens.begin(), tokens.end());
  {
    const std::scoped_lock lock(mutex_);
    if (!freed_.load(std::memory_order_acquire)) {
      std::vector<VkEvent> events;
      for (const uint64_t token : tokens) {
        const auto found = entries_.find(token);
        if (found != entries_.end() && found->second.event != VK_NULL_HANDLE && std::ranges::find(events, found->second.event) == events.end()) {
          events.push_back(found->second.event);
        }
      }
      // Retire this recording and all its resubmission tokens.
      std::erase_if(entries_, [&](const auto& entry) {
        const bool listed = std::ranges::find(tokens, entry.first) != tokens.end();
        const bool aliased = entry.second.owner != 0u && std::ranges::find(tokens, entry.second.owner) != tokens.end();
        if (!listed && !aliased) return false;
        if (const auto motion = entry.second.motion.lock()) {
          motion->completed_ = entry.second.executed && entry.second.event != VK_NULL_HANDLE && !lost_.load()
              && functions_.vkGetEventStatus(device_, entry.second.event) == VK_EVENT_SET;
          motion->event_ = VK_NULL_HANDLE;
          motion->thread_ = 0u;
        }
        if (!listed) {
          dropped.push_back(entry.first);
        }
        return true;
      });
      const bool lost = lost_.load(std::memory_order_acquire);
      for (const VkEvent event : events) {
        if (lost) {
          pool_.push_back(event);  // never used again after a loss: Abandon leaves the pool to the device
        } else if (functions_.vkResetEvent(device_, event) == VK_SUCCESS) {
          pool_.push_back(event);
        } else {
          functions_.vkDestroyEvent(device_, event, nullptr);  // a SET event must never serve a later token
        }
      }
    }
  }
  // Without the lock: Drop takes the Timeline's token lock, under which the probe takes this one.
  timeline_->Drop(dropped);
}

void NrCompletion::DiagnoseStale(std::chrono::steady_clock::time_point now, uint64_t frame) {
  if (stale_logged_) return;
  bool stale = false;
  {
    const std::scoped_lock lock(mutex_);
    stale = std::ranges::any_of(entries_, [&](const auto& item) {
      const Entry& entry = item.second;
      return (!entry.executed || entry.event == VK_NULL_HANDLE) && now - entry.issued >= STALE_AGE
          && frame >= entry.issue_frame + STALE_FRAMES;
    });
  }
  if (stale) {
    stale_logged_ = true;
    nr::Logf(nr::LogLevel::INFO, uplift::text::VK_STALE_TOKEN_LINE, STALE_AGE.count());
  }
}

void NrCompletion::DeviceIdle() {
  idle_.store(true, std::memory_order_release);
}

bool NrCompletion::WaitSubmitted(std::chrono::milliseconds cap) {
  const auto deadline = std::chrono::steady_clock::now() + cap;
  while (true) {
    bool pending = false;
    {
      const std::scoped_lock lock(mutex_);
      if (freed_.load(std::memory_order_acquire) || lost_.load(std::memory_order_acquire) || idle_.load(std::memory_order_acquire)) return true;
      for (const auto& [token, entry] : entries_) {
        if (!entry.executed) continue;  // never submitted: it cannot be running
        // A submitted recording whose event could not be made cannot tell its end: only the cap ends the wait for it.
        const VkResult status = (entry.event == VK_NULL_HANDLE ? VK_EVENT_RESET : functions_.vkGetEventStatus(device_, entry.event));
        if (status == VK_ERROR_DEVICE_LOST) {
          lost_.store(true, std::memory_order_release);
          return true;
        }
        if (status != VK_EVENT_SET) {
          pending = true;
          break;
        }
      }
    }
    if (!pending) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    Sleep(1u);
  }
}

void NrCompletion::FreeAll() {
  const std::scoped_lock lock(mutex_);
  if (freed_.exchange(true)) return;
  std::vector<VkEvent> events = std::move(pool_);
  for (const auto& [token, entry] : entries_) {
    if (entry.event != VK_NULL_HANDLE) {
      events.push_back(entry.event);
    }
  }
  entries_.clear();
  std::ranges::sort(events);
  const auto duplicates = std::ranges::unique(events);
  events.erase(duplicates.begin(), duplicates.end());
  for (const VkEvent event : events) {
    functions_.vkDestroyEvent(device_, event, nullptr);
  }
  if (semaphore_ != VK_NULL_HANDLE) {
    functions_.vkDestroySemaphore(device_, semaphore_, nullptr);
    semaphore_ = VK_NULL_HANDLE;
  }
}

void NrCompletion::Abandon() {
  const std::scoped_lock lock(mutex_);
  if (freed_.exchange(true)) return;  // from here every query says complete, and FreeAll (the destructor's too) destroys nothing
  entries_.clear();
  pool_.clear();
  semaphore_ = VK_NULL_HANDLE;
}

}  // namespace uplift::vk
