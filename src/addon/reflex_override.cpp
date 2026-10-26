#include "addon/reflex_override.hpp"

#include <MinHook.h>

#include <atomic>
#include <format>
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

// Submits a copy of `params` when a copy is needed, so the game's own block is only ever handed
// back untouched on a passthrough.


// NV_SET_SLEEP_MODE_PARAMS_V1 has one official spelling (nvapi_query.hpp): NvBool is one byte, so
// the structure is 44 bytes and a caller's version reads 0x1002c. Anything else passes through
// untouched rather than being written by guesswork about a layout this project has never seen.
bool KnownSleepModeVersion(uint32_t version) {
  return (version & 0xffff0000u) != 0u && (version & 0xffffu) == sizeof(nvapi::SleepModeParams);
}

// The cached game submission: the device and the game's own params from the last observed call,
// for a live panel change to re-submit (SetSleepMode is a driver call, not a render submit; the
// replay goes through the trampoline from the add-on's Present poll).
std::atomic<IUnknown*> g_seen_device{nullptr};
std::atomic<uint32_t> g_seen_version{0u};
nvapi::SleepModeParams g_seen_params{};  // written only inside the detour (the game's calling thread)
std::atomic<bool> g_refresh_pending{false};

// The driver's display-path state from that same read, for the panel's status line (bFsVrr,
// bFullscreenIFlip, bCplVsyncOn). `valid` separates "the driver said off" from "no sample yet".
std::atomic<uint32_t> g_driver_vrr{0u};
std::atomic<uint32_t> g_driver_iflip{0u};
std::atomic<uint32_t> g_driver_cpl_vsync{0u};
std::atomic<uint32_t> g_driver_status_valid{0u};

// Edge triggers for the driver status line: the first sample always prints, later ones only when one
// of these differs from the last printed line (the interval with a tenth of slack, because the
// driver's own pacing wobbles by a few percent between samples).
std::atomic<uint32_t> g_status_seen{0u};
std::atomic<uint32_t> g_status_mode{0u};
std::atomic<uint32_t> g_status_sleep{0u};
std::atomic<uint32_t> g_status_fg{0u};
std::atomic<uint32_t> g_status_vrr{0u};
std::atomic<uint32_t> g_status_vsync{0u};
std::atomic<uint32_t> g_status_iflip{0u};
std::atomic<uint32_t> g_status_interval{0u};

// The panel's advanced SetSleepMode bits: forced to 1 while ticked, left to the game's own value
// otherwise (an unticked box must not clear what the game set).
std::atomic<bool> g_desired_use_markers{false};
std::atomic<bool> g_desired_use_min_queue_time{false};

// The newest completed latency frame (element 63) in microseconds. 0 means "no report yet", which
// is also exactly what a game that never marks its frames reports.
std::atomic<uint32_t> g_latency_pc_us{0u};
std::atomic<uint32_t> g_latency_input_us{0u};
std::atomic<uint32_t> g_latency_gpu_us{0u};
std::atomic<uint32_t> g_latency_fg_us{0u};

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
  const bool log_this_call = call_number <= 5u || params->bLowLatencyMode != last_logged_mode
                             || params->minimumIntervalUs != last_logged_interval;
  if (log_this_call) {
    nr::Logf(nr::LogLevel::INFO,
             "reflex: SetSleepMode call #{}: params version {:#x}, native mode {} boost {} interval {} us, markers {} min queue {}; panel: mode {} cap {} FPS",
             call_number, params->version, static_cast<uint32_t>(params->bLowLatencyMode),
             static_cast<uint32_t>(params->bLowLatencyBoost), params->minimumIntervalUs,
             static_cast<uint32_t>(params->bUseMarkersToOptimize),
             static_cast<uint32_t>(params->bUseMinQueueTime), mode, cap_fps);
    g_last_logged_mode.store(params->bLowLatencyMode, std::memory_order_relaxed);
    g_last_logged_interval.store(params->minimumIntervalUs, std::memory_order_relaxed);
  }

  // Anything this rewrite does not own -- a params version other than the official V1 spelling, or
  // Game mode with no cap -- passes through untouched.
  if (!KnownSleepModeVersion(params->version)) {
    if (log_this_call) {
      nr::Logf(nr::LogLevel::INFO, "reflex: params version {:#x} is unknown; passed through", params->version);
    }
    return original(device, params);
  }

  // Cache the submission for a live panel change to re-submit later (RefreshFromPanel).
  {
    g_seen_params = *params;
    g_seen_version.store(params->version, std::memory_order_release);
    IUnknown* expected = g_seen_device.load(std::memory_order_relaxed);
    if (expected != device) {
      if (device != nullptr) device->AddRef();
      g_seen_device.store(device, std::memory_order_release);
      if (expected != nullptr) expected->Release();
    }
  }

  const bool rewrite_mode = mode != static_cast<uint32_t>(ReflexModeOverride::GAME);
  // The cap is a final output-FPS ceiling and the driver's minimumIntervalUs is the output frame
  // interval: a 200 FPS cap with 2x generation submits 5000 us, and the driver's own read-back then
  // reports twice that (the source interval) while the rendered output still runs at 200.
  const uint32_t cap_us = CapToIntervalUs(cap_fps);
  // Only ever tighten: the stricter (larger) interval wins, so the game's own limiter is safe.
  const uint32_t native_interval = params->minimumIntervalUs;
  const uint32_t limit_us = cap_us == 0u || native_interval > cap_us ? native_interval : cap_us;
  const bool rewrite_limit = limit_us != native_interval;
  // The two advanced bits are opt-in: ticked they are forced on, unticked the game's value stands.
  const bool force_markers = g_desired_use_markers.load(std::memory_order_relaxed);
  const bool force_min_queue = g_desired_use_min_queue_time.load(std::memory_order_relaxed);
  const bool rewrite_advanced = force_markers || force_min_queue;

  if (!rewrite_mode && !rewrite_limit && !rewrite_advanced) {
    const nvapi::Status result = original(device, params);
    if (log_this_call) nr::Logf(nr::LogLevel::INFO, "reflex: nothing to rewrite (Game mode, no tighter cap); passed through -> {}", static_cast<int32_t>(result));
    g_refresh_pending.store(false, std::memory_order_relaxed);
    return result;
  }

  // Rewrite a copy, leaving every field this override does not own exactly as the game sent it.
  nvapi::SleepModeParams rewritten = *params;
  if (rewrite_mode) {
    rewritten.bLowLatencyMode = (mode == static_cast<uint32_t>(ReflexModeOverride::ON)
                                    || mode == static_cast<uint32_t>(ReflexModeOverride::BOOST))
                                       ? 1u
                                       : 0u;  // OFF
    rewritten.bLowLatencyBoost = mode == static_cast<uint32_t>(ReflexModeOverride::BOOST) ? 1u
                                    : mode == static_cast<uint32_t>(ReflexModeOverride::OFF) ? 0u
                                                                                            : params->bLowLatencyBoost;
  }
  if (rewrite_limit) rewritten.minimumIntervalUs = limit_us;
  if (force_markers) rewritten.bUseMarkersToOptimize = 1u;
  if (force_min_queue) rewritten.bUseMinQueueTime = 1u;

  nvapi::Status result = original(device, &rewritten);
  if (result != nvapi::kOk) {
    // One bounded retry with the game's own request: never leave the player without Reflex
    // because the rewritten mode was refused (a rejected SetSleepMode can disable latency
    // markers entirely on some driver versions).
    result = original(device, params);
    if (result != nvapi::kOk) {
      static std::atomic_bool warned{false};
      if (!warned.exchange(true, std::memory_order_relaxed)) {
        nr::Logf(nr::LogLevel::WARN, "the Reflex override's rewrite was refused (NvAPI status {}); forwarding the game's own settings", static_cast<int32_t>(result));
      }
      return result;
    }
  } else {
    if (log_this_call) {
      nr::Logf(nr::LogLevel::INFO, "reflex: rewritten submission accepted: mode {} boost {} interval {} us",
               static_cast<uint32_t>(rewritten.bLowLatencyMode),
               static_cast<uint32_t>(rewritten.bLowLatencyBoost), rewritten.minimumIntervalUs);
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

  g_install.store(1u, std::memory_order_release);
  nr::Logf(nr::LogLevel::INFO, "the Reflex override hooked NvAPI_D3D_SetSleepMode at {}", reinterpret_cast<void*>(target));
}

}  // namespace

void SetDesired(ReflexModeOverride mode, uint32_t fps_cap) {
  const uint32_t new_mode = static_cast<uint32_t>(mode);
  const uint32_t new_cap = fps_cap > 1000u ? 0u : fps_cap;
  // Every settings change re-pushes all live choices (PushFgRuntimeChoices), so an unrelated row's
  // drag would otherwise re-submit SetSleepMode on each frame; only a different choice needs one.
  const uint32_t previous_mode = g_desired_mode.exchange(new_mode, std::memory_order_relaxed);
  const uint32_t previous_cap = g_desired_cap_fps.exchange(new_cap, std::memory_order_relaxed);
  if (previous_mode == new_mode && previous_cap == new_cap) return;
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
  nvapi::SleepModeParams fields = g_seen_params;
  const uint32_t mode = g_desired_mode.load(std::memory_order_relaxed);
  // Output-FPS semantics, like the detour above: the interval is the output frame interval.
  const uint32_t cap_us = CapToIntervalUs(g_desired_cap_fps.load(std::memory_order_relaxed));
  if (mode != static_cast<uint32_t>(ReflexModeOverride::GAME)) {
    fields.bLowLatencyMode = (mode == static_cast<uint32_t>(ReflexModeOverride::ON)
                                 || mode == static_cast<uint32_t>(ReflexModeOverride::BOOST))
                                    ? 1u
                                    : 0u;
    fields.bLowLatencyBoost = mode == static_cast<uint32_t>(ReflexModeOverride::BOOST) ? 1u
                                 : mode == static_cast<uint32_t>(ReflexModeOverride::OFF) ? 0u
                                                                                         : fields.bLowLatencyBoost;
  }
  if (cap_us != 0u && fields.minimumIntervalUs < cap_us) fields.minimumIntervalUs = cap_us;
  if (g_desired_use_markers.load(std::memory_order_relaxed)) fields.bUseMarkersToOptimize = 1u;
  if (g_desired_use_min_queue_time.load(std::memory_order_relaxed)) fields.bUseMinQueueTime = 1u;
  const nvapi::Status result = original(device, &fields);
  // Once per panel save: the values submitted live and the driver's verdict.
  nr::Logf(nr::LogLevel::INFO,
           "reflex: live refresh submitted version {:#x}: mode {} boost {} interval {} us, markers {} min queue {} -> {}",
           version, static_cast<uint32_t>(fields.bLowLatencyMode),
           static_cast<uint32_t>(fields.bLowLatencyBoost), fields.minimumIntervalUs,
           static_cast<uint32_t>(fields.bUseMarkersToOptimize),
           static_cast<uint32_t>(fields.bUseMinQueueTime), static_cast<int32_t>(result));
}

// Every two seconds: sample the driver's sleep status for the panel's readouts (VRR, fullscreen
// i-flip) and its status log, and warn once when the game never calls the Reflex sleep at all --
// the cap cannot bite there.
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
      nr::Logf(nr::LogLevel::INFO, "reflex: driver sleep status unavailable ({})", static_cast<int32_t>(sample.status));
    }
    return;
  }
  g_driver_vrr.store(sample.sleep.bFsVrr, std::memory_order_relaxed);
  g_driver_iflip.store(sample.sleep.bFullscreenIFlip, std::memory_order_relaxed);
  g_driver_cpl_vsync.store(sample.sleep.bCplVsyncOn, std::memory_order_relaxed);
  g_driver_status_valid.store(1u, std::memory_order_relaxed);
  // The status line is an edge log: the first sample prints, later ones only when something moved --
  // the discrete bits outright, the held interval once it moves by more than a tenth. A steady cap
  // the driver honours therefore goes quiet, while one the driver ignores keeps moving and keeps
  // being reported; the panel's readout covers "what is it right now".
  const uint32_t interval_now = sample.sleep.sleepIntervalUs;
  const uint32_t interval_was = g_status_interval.load(std::memory_order_relaxed);
  const uint32_t interval_delta =
      interval_now > interval_was ? interval_now - interval_was : interval_was - interval_now;
  const bool status_changed =
      g_status_seen.load(std::memory_order_relaxed) == 0u
      || sample.sleep.bLowLatencyMode != g_status_mode.load(std::memory_order_relaxed)
      || sample.sleep.bUseGameSleep != g_status_sleep.load(std::memory_order_relaxed)
      || sample.sleep.fgMultiplier != g_status_fg.load(std::memory_order_relaxed)
      || sample.sleep.bFsVrr != g_status_vrr.load(std::memory_order_relaxed)
      || sample.sleep.bCplVsyncOn != g_status_vsync.load(std::memory_order_relaxed)
      || sample.sleep.bFullscreenIFlip != g_status_iflip.load(std::memory_order_relaxed)
      || interval_delta * 10u > interval_was;
  // Still only while the panel overrides something: untouched, the driver's own numbers are noise.
  if (status_changed
      && (g_desired_mode.load(std::memory_order_relaxed) != static_cast<uint32_t>(ReflexModeOverride::GAME)
          || g_desired_cap_fps.load(std::memory_order_relaxed) != 0u
          || g_desired_use_markers.load(std::memory_order_relaxed)
          || g_desired_use_min_queue_time.load(std::memory_order_relaxed))) {
    g_status_seen.store(1u, std::memory_order_relaxed);
    g_status_mode.store(sample.sleep.bLowLatencyMode, std::memory_order_relaxed);
    g_status_sleep.store(sample.sleep.bUseGameSleep, std::memory_order_relaxed);
    g_status_fg.store(sample.sleep.fgMultiplier, std::memory_order_relaxed);
    g_status_vrr.store(sample.sleep.bFsVrr, std::memory_order_relaxed);
    g_status_vsync.store(sample.sleep.bCplVsyncOn, std::memory_order_relaxed);
    g_status_iflip.store(sample.sleep.bFullscreenIFlip, std::memory_order_relaxed);
    g_status_interval.store(interval_now, std::memory_order_relaxed);
    nr::Logf(nr::LogLevel::INFO,
             "reflex: driver status: mode {} interval {} us, game sleep {}, FG x{}, vrr {}, panel vsync {}, "
             "fullscreen i-flip {}",
             static_cast<uint32_t>(sample.sleep.bLowLatencyMode), interval_now,
             static_cast<uint32_t>(sample.sleep.bUseGameSleep),
             static_cast<uint32_t>(sample.sleep.fgMultiplier),
             static_cast<uint32_t>(sample.sleep.bFsVrr), static_cast<uint32_t>(sample.sleep.bCplVsyncOn),
             static_cast<uint32_t>(sample.sleep.bFullscreenIFlip));
  }
  // The cap only bites when something calls the Reflex sleep each frame; say so once instead of
  // letting a silently ineffective cap look like a broken feature.
  if (sample.sleep.bUseGameSleep == 0u && g_desired_cap_fps.load(std::memory_order_relaxed) != 0u) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::WARN,
               "reflex: this game does not call the Reflex sleep each frame, so the FPS cap cannot take effect; use the game's own limiter or RTSS's Reflex mode instead");
    }
  }
}

void SetAdvanced(bool use_markers, bool use_min_queue_time) {
  const bool previous_markers = g_desired_use_markers.exchange(use_markers, std::memory_order_relaxed);
  const bool previous_queue = g_desired_use_min_queue_time.exchange(use_min_queue_time, std::memory_order_relaxed);
  if (previous_markers == use_markers && previous_queue == use_min_queue_time) return;
  g_refresh_pending.store(true, std::memory_order_release);
}

void PollLatency() {
  static std::atomic<uint64_t> last_ms{0u};
  const uint64_t now = GetTickCount64();
  uint64_t last = last_ms.load(std::memory_order_relaxed);
  if (now - last < 1000u) return;
  if (!last_ms.compare_exchange_strong(last, now, std::memory_order_acq_rel)) return;
  IUnknown* device = g_seen_device.load(std::memory_order_acquire);
  if (device == nullptr) return;
  // 15 KB: a function-local static, so the report never sits on a Present thread's stack.
  static nvapi::LatencyResult report{};
  const nvapi::Status status = nvapi::GetLatency(device, &report);
  if (status != nvapi::kOk) {
    static std::atomic<bool> unavailable_logged{false};
    if (!unavailable_logged.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::INFO, "reflex: the latency report is unavailable ({})", static_cast<int32_t>(status));
    }
    return;
  }
  // The newest completed frame; every frame stays zeroed until the game has marked enough of them
  // (NvAPI_D3D_GetLatency's own contract), so a zero frame means "the game does not mark".
  const auto& frame = report.frameReport[63];
  if (frame.frameID == 0u || frame.presentEndTime <= frame.simStartTime) {
    g_latency_pc_us.store(0u, std::memory_order_relaxed);
    static std::atomic<bool> unmarked_logged{false};
    if (!unmarked_logged.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::INFO,
               "reflex: the latency report is still empty; this game does not mark its frames, so there is no latency to show");
    }
    return;
  }
  const uint64_t pc_us = frame.presentEndTime - frame.simStartTime;
  const uint64_t input_us = frame.presentEndTime > frame.inputSampleTime ? frame.presentEndTime - frame.inputSampleTime : 0u;
  g_latency_pc_us.store(static_cast<uint32_t>(pc_us), std::memory_order_relaxed);
  g_latency_input_us.store(static_cast<uint32_t>(input_us), std::memory_order_relaxed);
  g_latency_gpu_us.store(frame.gpuFrameTimeUs, std::memory_order_relaxed);
  g_latency_fg_us.store(frame.aiFrameTimeUs, std::memory_order_relaxed);
  // Once, with the raw numbers: they are what confirms the report's units against the microseconds
  // every other NvU32 field in this structure is documented in.
  static std::atomic<bool> raw_logged{false};
  if (!raw_logged.exchange(true, std::memory_order_relaxed)) {
    nr::Logf(nr::LogLevel::INFO,
             "reflex: latency raw sample: frame {} sim {} present {} input {} qpc, gpu {} us, FG {} us",
             frame.frameID, frame.simStartTime, frame.presentEndTime, frame.inputSampleTime,
             frame.gpuFrameTimeUs, frame.aiFrameTimeUs);
  }
}

std::string LatencyLine() {
  const uint32_t pc_us = g_latency_pc_us.load(std::memory_order_relaxed);
  if (pc_us == 0u) return {};  // no frame report yet, or the game never marks its frames
  return std::format("Latency: {:.1f} ms PC, {:.1f} ms input, {:.1f} ms GPU, {:.1f} ms FG",
                     static_cast<double>(pc_us) / 1000.0,
                     static_cast<double>(g_latency_input_us.load(std::memory_order_relaxed)) / 1000.0,
                     static_cast<double>(g_latency_gpu_us.load(std::memory_order_relaxed)) / 1000.0,
                     static_cast<double>(g_latency_fg_us.load(std::memory_order_relaxed)) / 1000.0);
}

std::string DriverLine() {
  if (g_driver_status_valid.load(std::memory_order_relaxed) == 0u) return {};
  return std::format("Driver: VRR {}, fullscreen i-flip {}, panel vsync {}",
                     g_driver_vrr.load(std::memory_order_relaxed) != 0u ? "on" : "off",
                     g_driver_iflip.load(std::memory_order_relaxed) != 0u ? "on" : "off",
                     g_driver_cpl_vsync.load(std::memory_order_relaxed) != 0u ? "on" : "off");
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
