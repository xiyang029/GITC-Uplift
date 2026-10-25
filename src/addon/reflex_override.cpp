#include "addon/reflex_override.hpp"

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "addon/nvapi_query.hpp"
#include "ngx_hooks/minhook_once.hpp"
#include "nr/log.hpp"

namespace uplift::addon::reflex {
namespace {

using SetSleepModeFn = nvapi::Status(__cdecl*)(IUnknown*, nvapi::SleepModeParams*);

std::atomic<uint32_t> g_desired_mode{0u};  // ReflexModeOverride; 0 = follow the game
std::atomic<uint32_t> g_desired_cap_fps{0u};  // 0 = leave the game's own limiter alone
std::atomic<SetSleepModeFn> g_original{nullptr};

// Call counting and the last logged values: the SetSleepMode log lines below fire on the first
// few calls and then only when the game's own request changes.
std::atomic<uint32_t> g_calls_seen{0u};
std::atomic<uint32_t> g_last_logged_mode{UINT32_MAX};
std::atomic<uint32_t> g_last_logged_interval{UINT32_MAX};

// 1e6 / fps, rounded to nearest: a cap of N FPS is an interval of 1e6/N microseconds. Rounding
// down would overrun the cap, so round to nearest like MFGAdaUnlock's TargetFpsToFrameLimitUs.
uint32_t CapToIntervalUs(uint32_t fps) {
  if (fps == 0u) return 0u;
  return static_cast<uint32_t>((1'000'000ull + fps / 2ull) / fps);
}

// Submits a copy of `params` when a copy is needed; the rewrites below work on a byte buffer so
// the game's own block is only ever handed back untouched on a passthrough.


// The driver's real params versions: 0x1002c (44 bytes, the shipped Reflex V1) and 0x10038
// (56 bytes, the newer SDK header's spelling with bUseMinQueueTime and 30 reserved bytes). The
// first four fields -- version, low-latency mode, boost, minimum interval -- sit at the same
// offsets in both, and only those are rewritten.
constexpr uint32_t kSleepModeParamsV1Size = 0x2cu;
constexpr uint32_t kSleepModeParamsV1BSize = 0x38u;
bool KnownSleepModeVersion(uint32_t version) {
  const uint32_t size = version & 0xffffu;
  return (version & 0xffff0000u) != 0u && (size == kSleepModeParamsV1Size || size == kSleepModeParamsV1BSize);
}

// The cached game submission: the device and the game's own params from the last observed call,
// for a live panel change to re-submit (SetSleepMode is a driver call, not a render submit; the
// replay goes through the trampoline from the add-on's Present poll).
std::atomic<IUnknown*> g_seen_device{nullptr};
std::atomic<uint32_t> g_seen_version{0u};
uint8_t g_seen_params[64];  // written and read only inside the detour (the game's calling thread)
std::atomic<bool> g_refresh_pending{false};

// ---- The enforced cap (NvAPI_D3D_Sleep) ----
// Some Reflex integrations accept an externally submitted minimumIntervalUs and then ignore it;
// RTSS-style limiters work there because they enforce their own wait. The sleep hook does the
// same: after the game's sleep returns, hold the frame to the panel's interval. The sleep runs
// once per rendered (source) frame, so with frame generation the interval divides by the live
// multiplier -- the cap is a final output-FPS ceiling.

using SleepFn = nvapi::Status(__cdecl*)(IUnknown*);
std::atomic<SleepFn> g_sleep_original{nullptr};
std::atomic<uint32_t> g_live_fg_multiplier{1u};  // refreshed from GetSleepStatus (2 s cadence)
uint64_t g_cap_last_qpc = 0;                     // the sleep-detour thread only
std::atomic<bool> g_cap_enforced_logged{false};

uint64_t QpcNow() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return static_cast<uint64_t>(now.QuadPart);
}

uint64_t QpcFrequency() {
  static const uint64_t frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
  }();
  return frequency;
}

// A high-resolution waitable timer (Windows 10 1803+): ~50-100 us accuracy without burning a
// core. It is the only wait primitive the enforced cap uses; without it the hook stands down and
// the driver's own enforcement of the submitted interval is all that is left.
HANDLE HighResolutionTimer() {
  static const HANDLE timer = [] {
    const HANDLE created = CreateWaitableTimerExW(nullptr, nullptr,
                                                  CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                  TIMER_ALL_ACCESS);
    static std::once_flag logged;
    std::call_once(logged, [created] {
      if (created != nullptr) {
        nr::Logf(nr::LogLevel::INFO, "reflex: the enforced cap uses a high-resolution waitable timer (~0.1 ms accuracy)");
      } else {
        nr::Logf(nr::LogLevel::WARN,
                 "reflex: high-resolution timers are unavailable ({}); the enforced cap stands down and only a driver that honours the submitted interval limits frames",
                 GetLastError());
      }
    });
    return created;
  }();
  return timer;
}

// Waits until `target` (QPC): a high-resolution timer down to ~150 us remaining, then spin.
// Returns false when this system cannot provide the precise wait (no timer, or a failed timer
// call), so the caller resyncs instead of inventing a pacing anchor it never reached.
bool WaitUntilQpc(uint64_t target) {
  while (true) {
    const uint64_t now = QpcNow();
    if (now >= target) return true;
    const uint64_t remaining = target - now;
    const uint64_t frequency = QpcFrequency();
    if (remaining <= frequency / 6666ull) {  // <= ~150 us: spin
      YieldProcessor();
      continue;
    }
    const HANDLE timer = HighResolutionTimer();
    if (timer == nullptr) return false;
    // Relative due time in 100 ns units, negative; leave ~150 us for the spin phase.
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>((remaining - frequency / 6666ull) * 10000ull / frequency);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) == FALSE) return false;
    WaitForSingleObject(timer, static_cast<DWORD>(remaining * 1500ull / frequency + 2u));
  }
}

// The cap's mode: trust the driver's own enforcement of the submitted interval first (the
// lowest-latency pacing there is), and only take over the wait when the measured frame intervals
// prove the driver is not honouring it -- twenty consecutive frames running faster than 98 % of
// the target. A cap of 0 resets to observing.
enum class CapMode : uint32_t { kObserve = 0, kEnforce = 1 };
std::atomic<uint32_t> g_cap_mode{static_cast<uint32_t>(CapMode::kObserve)};
uint32_t g_cap_fast_frames = 0u;  // the sleep-detour thread only
uint64_t g_cap_last_call_qpc = 0;  // when the previous sleep call arrived (measurement anchor)

nvapi::Status __cdecl SleepDetour(IUnknown* device) {
  const SleepFn original = g_sleep_original.load(std::memory_order_acquire);
  if (original == nullptr) return nvapi::kUnavailable;
  const nvapi::Status result = original(device);

  const uint32_t cap_fps = g_desired_cap_fps.load(std::memory_order_relaxed);
  const uint64_t now = QpcNow();
  if (cap_fps == 0u) {
    g_cap_mode.store(static_cast<uint32_t>(CapMode::kObserve), std::memory_order_relaxed);
    g_cap_fast_frames = 0u;
    g_cap_last_qpc = now;  // keep the anchor fresh so a later cap does not wait for ages
    return result;
  }
  const uint32_t multiplier = g_live_fg_multiplier.load(std::memory_order_relaxed);
  const uint64_t source_interval_qpc =
      (CapToIntervalUs(cap_fps) * QpcFrequency() * (multiplier == 0u ? 1u : multiplier)) / 1'000'000ull;

  if (g_cap_mode.load(std::memory_order_relaxed) == static_cast<uint32_t>(CapMode::kObserve)) {
    // Measure the driver's own pacing: the interval between sleep calls against the target.
    if (g_cap_last_call_qpc != 0u) {
      const uint64_t actual = now - g_cap_last_call_qpc;
      if (actual + source_interval_qpc / 50u < source_interval_qpc) {
        if (++g_cap_fast_frames >= 20u) {
          g_cap_mode.store(static_cast<uint32_t>(CapMode::kEnforce), std::memory_order_relaxed);
          g_cap_last_qpc = now;
          nr::Logf(nr::LogLevel::INFO,
                   "reflex: the driver is not honouring the submitted {} FPS interval; taking over the wait in the sleep hook (FG multiplier {})",
                   cap_fps, multiplier);
        }
      } else {
        g_cap_fast_frames = 0u;
      }
    }
    g_cap_last_call_qpc = now;
    return result;
  }

  const uint64_t target = g_cap_last_qpc + source_interval_qpc;
  if (now < target) {
    if (target - now < 10'000'000'000ull) {  // never wait on a stale anchor
      if (WaitUntilQpc(target)) {
        g_cap_last_qpc = target;
        if (!g_cap_enforced_logged.exchange(true, std::memory_order_relaxed)) {
          nr::Logf(nr::LogLevel::INFO, "reflex: enforcing the {} FPS cap in the sleep hook (FG multiplier {})", cap_fps, multiplier);
        }
      } else {
        g_cap_last_qpc = now;  // no precise wait on this system: resync, do not fake the anchor
      }
    } else {
      g_cap_last_qpc = now;
    }
  } else if (now - target > 2u * source_interval_qpc) {
    // A frame ran very long: resync instead of burst-catching-up a stale rhythm.
    g_cap_last_qpc = now;
  } else {
    // A mild overrun keeps the pacing clock: the next frame's target stays where the rhythm
    // put it, so one slow frame does not shift every following frame later with it.
    g_cap_last_qpc = target;
  }
  return result;
}

nvapi::Status __cdecl SetSleepModeDetour(IUnknown* device, nvapi::SleepModeParams* params) {
  const SetSleepModeFn original = g_original.load(std::memory_order_acquire);
  if (original == nullptr || params == nullptr) return original == nullptr ? nvapi::kUnavailable : original(device, params);

  const uint32_t mode = g_desired_mode.load(std::memory_order_relaxed);
  const uint32_t cap_fps = g_desired_cap_fps.load(std::memory_order_relaxed);
  const uint32_t call_number = g_calls_seen.fetch_add(1u, std::memory_order_relaxed) + 1u;
  // Log the first few calls and then only changes: these lines are how a live test tells whether
  // the game's Reflex path reaches NvAPI_D3D_SetSleepMode at all (an OptiScaler-style shim may
  // never call it), and what was rewritten.
  const uint32_t last_logged_mode = g_last_logged_mode.load(std::memory_order_relaxed);
  const uint32_t last_logged_interval = g_last_logged_interval.load(std::memory_order_relaxed);
  const bool log_this_call = call_number <= 5u
                             || params->b_low_latency_mode != last_logged_mode
                             || params->minimum_interval_us != last_logged_interval;
  if (log_this_call) {
    nr::Logf(nr::LogLevel::INFO, "reflex: SetSleepMode call #{}: native mode {} boost {} interval {} us; panel: mode {} cap {} FPS",
             call_number, params->b_low_latency_mode, params->b_low_latency_boost, params->minimum_interval_us, mode, cap_fps);
    g_last_logged_mode.store(params->b_low_latency_mode, std::memory_order_relaxed);
    g_last_logged_interval.store(params->minimum_interval_us, std::memory_order_relaxed);
  }

  // Anything this rewrite does not own -- a params version other than the two known V1 spellings,
  // or Game mode with no cap -- passes through untouched.
  if (!KnownSleepModeVersion(params->version)) {
    if (log_this_call) {
      nr::Logf(nr::LogLevel::INFO, "reflex: params version {:#x} is unknown; passed through", params->version);
    }
    return original(device, params);
  }
  const uint32_t params_size = params->version & 0xffffu;

  // Cache the submission for a live panel change to re-submit later (RefreshFromPanel).
  if (params_size <= sizeof(g_seen_params)) {
    memcpy(g_seen_params, params, params_size);
    g_seen_version.store(params->version, std::memory_order_release);
    IUnknown* expected = g_seen_device.load(std::memory_order_relaxed);
    if (expected != device) {
      if (device != nullptr) device->AddRef();
      g_seen_device.store(device, std::memory_order_release);
      if (expected != nullptr) expected->Release();
    }
  }

  auto* fields = params;  // prefix layout shared by both V1 spellings

  const bool rewrite_mode = mode != static_cast<uint32_t>(ReflexModeOverride::GAME);
  // The cap is a final output-FPS ceiling: the driver's minimumIntervalUs acts on the source
  // (rendered) frame rate, so the submitted interval multiplies by the live FG multiplier.
  const uint32_t multiplier = g_live_fg_multiplier.load(std::memory_order_relaxed);
  const uint32_t cap_us = CapToIntervalUs(cap_fps) * (multiplier == 0u ? 1u : multiplier);
  // Only ever tighten: the stricter (larger) interval wins, so the game's own limiter is safe.
  const uint32_t limit_us =
      cap_us == 0u ? fields->minimum_interval_us : (fields->minimum_interval_us > cap_us ? fields->minimum_interval_us : cap_us);
  const bool rewrite_limit = limit_us != fields->minimum_interval_us;

  if (!rewrite_mode && !rewrite_limit) {
    const nvapi::Status result = original(device, params);
    if (log_this_call) nr::Logf(nr::LogLevel::INFO, "reflex: nothing to rewrite (Game mode, no tighter cap); passed through -> {}", result);
    g_refresh_pending.store(false, std::memory_order_relaxed);
    return result;
  }

  // Rewrite in place in a copy (only the shared prefix is touched; the reserved tail of either
  // spelling passes through byte-for-byte).
  uint8_t buffer[64];
  memcpy(buffer, params, params_size);
  auto* rewritten = reinterpret_cast<nvapi::SleepModeParams*>(buffer);
  if (rewrite_mode) {
    rewritten->b_low_latency_mode = (mode == static_cast<uint32_t>(ReflexModeOverride::ON)
                                         || mode == static_cast<uint32_t>(ReflexModeOverride::BOOST))
                                        ? 1u
                                        : 0u;  // OFF
    rewritten->b_low_latency_boost =
        mode == static_cast<uint32_t>(ReflexModeOverride::BOOST) ? 1u
        : mode == static_cast<uint32_t>(ReflexModeOverride::OFF) ? 0u
                                                                 : fields->b_low_latency_boost;
  }
  if (rewrite_limit) rewritten->minimum_interval_us = limit_us;

  nvapi::Status result = original(device, rewritten);
  if (result != nvapi::kOk) {
    // One bounded retry with the game's own request: never leave the player without Reflex
    // because the rewritten mode was refused (a rejected SetSleepMode can disable latency
    // markers entirely on some driver versions).
    result = original(device, params);
    if (result != nvapi::kOk) {
      static std::atomic_bool warned{false};
      if (!warned.exchange(true, std::memory_order_relaxed)) {
        nr::Logf(nr::LogLevel::WARN, "the Reflex override's rewrite was refused (NvAPI status {}); forwarding the game's own settings", result);
      }
      return result;
    }
  } else {
    if (log_this_call) {
      nr::Logf(nr::LogLevel::INFO, "reflex: rewritten submission accepted: mode {} boost {} interval {} us -> {}",
               rewritten->b_low_latency_mode, rewritten->b_low_latency_boost, rewritten->minimum_interval_us, result);
    }
  }
  g_refresh_pending.store(false, std::memory_order_relaxed);
  return result;
}

// Re-submits the panel's choice using the last observed (device, params) so a panel change lands
// without waiting for the game's own next call. Called from the add-on's Present poll.
std::atomic<uint32_t> g_install{0u};  // 0 = not tried, 1 = hooked, 2 = failed, 3/4 = in progress / watchdog started

void InstallReflexHook() {
  uint32_t state = g_install.load(std::memory_order_relaxed);
  if (state == 1u || state == 2u) return;
  if (!g_install.compare_exchange_strong(state, 3u, std::memory_order_acq_rel)) return;

  if (nvapi::Module() == nullptr || nvapi::QueryInterface() == nullptr) {
    g_install.store(0u, std::memory_order_release);  // not loaded yet: keep polling
    return;
  }
  const auto target = nvapi::Interface<SetSleepModeFn>(nvapi::kSetSleepModeId);
  if (target == nullptr) {
    // The interface id is public SDK ABI; a null here means this driver has no Reflex at all.
    g_install.store(2u, std::memory_order_release);
    return;
  }
  // Pin the driver's NvAPI like every other hook target this project takes.
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(target), &pinned);

  if (const MH_STATUS status = hooks::EnsureMinHook(); status != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "the Reflex override could not start MinHook: {}", MH_StatusToString(status));
    g_install.store(2u, std::memory_order_release);
    return;
  }

  void* original = nullptr;
  if (MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&SetSleepModeDetour), &original) != MH_OK
      || MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "the Reflex override could not hook NvAPI_D3D_SetSleepMode; the panel's Reflex rows stay off");
    g_install.store(2u, std::memory_order_release);
    return;
  }
  g_original.store(reinterpret_cast<SetSleepModeFn>(original), std::memory_order_release);
  hooks::HoldMinHook();

  // The enforced cap: NvAPI_D3D_Sleep (0x852CD1D2), where the game waits each frame. This runs
  // even where the driver ignores an externally submitted interval.
  const auto sleep_target = nvapi::Interface<SleepFn>(0x852CD1D2u);
  if (sleep_target != nullptr) {
    void* sleep_original = nullptr;
    if (MH_CreateHook(reinterpret_cast<void*>(sleep_target), reinterpret_cast<void*>(&SleepDetour), &sleep_original) == MH_OK
        && MH_EnableHook(reinterpret_cast<void*>(sleep_target)) == MH_OK) {
      g_sleep_original.store(reinterpret_cast<SleepFn>(sleep_original), std::memory_order_release);
      nr::Logf(nr::LogLevel::INFO, "the Reflex override hooked NvAPI_D3D_Sleep at {}; the enforced cap is live",
               reinterpret_cast<void*>(sleep_target));
    } else {
      nr::Logf(nr::LogLevel::WARN, "the Reflex override could not hook NvAPI_D3D_Sleep; the FPS cap only applies where the driver honours the submitted interval");
    }
  }

  g_install.store(1u, std::memory_order_release);
  nr::Logf(nr::LogLevel::INFO, "the Reflex override hooked NvAPI_D3D_SetSleepMode at {}", reinterpret_cast<void*>(target));
}

}  // namespace

void SetDesired(ReflexModeOverride mode, uint32_t fps_cap) {
  g_desired_mode.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
  g_desired_cap_fps.store(fps_cap > 1000u ? 0u : fps_cap, std::memory_order_relaxed);
  g_refresh_pending.store(true, std::memory_order_release);  // RefreshFromPanel (the Present poll) submits it
}

void RefreshFromPanel() {
  if (!g_refresh_pending.exchange(false, std::memory_order_acq_rel)) return;
  const auto original = g_original.load(std::memory_order_acquire);
  IUnknown* device = g_seen_device.load(std::memory_order_acquire);
  const uint32_t version = g_seen_version.load(std::memory_order_acquire);
  if (original == nullptr || device == nullptr || version == 0u) {
    // Nothing has been submitted yet, so there is nothing to re-submit: the detour reads the
    // desired values itself on the game's first SetSleepMode call. Re-arming here would only
    // repeat this line every Present for games that never call Reflex.
    nr::Logf(nr::LogLevel::INFO, "reflex: panel change is waiting for the game's first SetSleepMode call");
    return;
  }
  const uint32_t size = version & 0xffffu;
  uint8_t buffer[64];
  memcpy(buffer, g_seen_params, size);
  auto* fields = reinterpret_cast<nvapi::SleepModeParams*>(buffer);
  const uint32_t mode = g_desired_mode.load(std::memory_order_relaxed);
  // Output-FPS semantics, like the detour above: the driver's interval works on the source rate.
  const uint32_t live_multiplier = g_live_fg_multiplier.load(std::memory_order_relaxed);
  const uint32_t cap_us = CapToIntervalUs(g_desired_cap_fps.load(std::memory_order_relaxed))
                          * (live_multiplier == 0u ? 1u : live_multiplier);
  if (mode != static_cast<uint32_t>(ReflexModeOverride::GAME)) {
    fields->b_low_latency_mode = (mode == static_cast<uint32_t>(ReflexModeOverride::ON)
                                      || mode == static_cast<uint32_t>(ReflexModeOverride::BOOST))
                                     ? 1u
                                     : 0u;
    fields->b_low_latency_boost =
        mode == static_cast<uint32_t>(ReflexModeOverride::BOOST) ? 1u
        : mode == static_cast<uint32_t>(ReflexModeOverride::OFF) ? 0u
                                                                 : fields->b_low_latency_boost;
  }
  if (cap_us != 0u && fields->minimum_interval_us < cap_us) fields->minimum_interval_us = cap_us;
  const nvapi::Status result = original(device, fields);
  if (result != nvapi::kOk) {
    nr::Logf(nr::LogLevel::INFO, "reflex: live refresh submitted mode {} interval {} us -> {}",
             fields->b_low_latency_mode, fields->minimum_interval_us, result);
  }
}

// Every two seconds: sample the driver's sleep status for the live FG multiplier (the enforced
// cap divides by it) and warn once when the game never calls the Reflex sleep at all -- the cap
// cannot bite there. The per-sample line itself was noise in the log.
void LogSleepStatusThrottled() {
  static std::atomic<uint64_t> last_log_ms{0u};
  const uint64_t now = GetTickCount64();
  uint64_t last = last_log_ms.load(std::memory_order_relaxed);
  if (now - last < 2000u) return;
  if (!last_log_ms.compare_exchange_strong(last, now, std::memory_order_acq_rel)) return;
  IUnknown* device = g_seen_device.load(std::memory_order_acquire);
  if (device == nullptr) return;
  const auto sample = nvapi::GetSleepStatus(device);
  if (sample.status != nvapi::kOk) {
    static std::atomic<bool> unavailable_logged{false};
    if (!unavailable_logged.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::INFO, "reflex: driver sleep status unavailable ({})", sample.status);
    }
    return;
  }
  // The enforced cap divides by the live multiplier: the sleep hook runs per source frame.
  if (sample.sleep.frame_generation_multiplier >= 1u && sample.sleep.frame_generation_multiplier <= 8u) {
    g_live_fg_multiplier.store(sample.sleep.frame_generation_multiplier, std::memory_order_relaxed);
  }
  // The cap only bites when something calls the Reflex sleep each frame; say so once instead of
  // letting a silently ineffective cap look like a broken feature.
  if (sample.sleep.game_sleep == 0u && g_desired_cap_fps.load(std::memory_order_relaxed) != 0u) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::WARN,
               "reflex: this game does not call the Reflex sleep each frame, so the FPS cap cannot take effect; use the game's own limiter instead");
    }
  }
}

void EnsureReflexHook() {
  uint32_t state = g_install.load(std::memory_order_relaxed);
  if (state != 0u) return;
  if (!g_install.compare_exchange_strong(state, 4u, std::memory_order_acq_rel)) return;
  // A short watchdog installs the moment nvapi64.dll appears, before any NvAPI caller
  // (fg_preset.cpp's EnsureFgPresetHook pattern).
  std::thread([] {
    for (;;) {
      const uint32_t current = g_install.load(std::memory_order_relaxed);
      if (current == 1u || current == 2u) return;
      if (nvapi::Module() != nullptr) break;
      Sleep(25);
    }
    InstallReflexHook();
    const uint32_t current = g_install.load(std::memory_order_relaxed);
    if (current == 3u) g_install.store(0u, std::memory_order_release);  // install declined: retry later
  }).detach();
}

}  // namespace uplift::addon::reflex
