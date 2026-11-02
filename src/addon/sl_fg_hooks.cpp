#include "addon/sl_fg_hooks.hpp"

#include <MinHook.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>

#include "addon/environment.hpp"  // Utf8FromPath, for the hooked module's path in the log
#include "addon/fg_preset.hpp"
#include "addon/frame_rate.hpp"
#include "ngx_hooks/minhook_once.hpp"
#include "nr/log.hpp"

namespace uplift::addon::slfg {
namespace {

// ---- The Streamline ABI this file speaks (sl_dlss_g.h, the pinned SDK's own headers) ----
// The structures, enums, GUIDs and signatures below are the SDK's, not copies of it: the game's
// own sl::DLSSGOptions/sl::DLSSGState/sl::ViewportHandle are read and rebuilt exactly as NVIDIA
// declares them, including DLSSGOptions' own field defaults (bReserved15 = eInvalid). Every
// unknown structVersion is refused (forwarded unchanged), never guessed at.

using SetOptionsFn = PFun_slDLSSGSetOptions*;
using GetStateFn = PFun_slDLSSGGetState*;
using GetFeatureFunctionFn = PFun_slGetFeatureFunction*;
// ---- The panel's choices ----

std::atomic<uint32_t> g_force_multiplier{0u};   // 0 = follow the game; 2..kMaxMultiplier = force
// The provider reports numFramesToGenerateMax 5 (6x) on this stack; the panel offers up to 6x.
constexpr uint32_t kMaxMultiplier = 6u;
std::atomic<bool> g_dynamic_enabled{false};
// XMX owns the multiplier through its own model: every NVIDIA-only DLSS-G option below stands down.
std::atomic<bool> g_xmx_blocked{false};
std::atomic<uint32_t> g_dynamic_target_fps{0u};  // 0 = the display's refresh rate
std::atomic<uint32_t> g_advertised_max{0u};      // GetState ceiling; 0 = do not touch
std::atomic<uint32_t> g_provider_max_generated{0u};  // the runtime's own numFramesToGenerateMax; 0 = not seen
std::atomic<bool> g_dynamic_d3d12{false};
// ---- Retry latches ----

std::atomic<uint32_t> g_force_failed_for{UINT32_MAX};
std::atomic<bool> g_dynamic_support_seen{false};
std::atomic<bool> g_dynamic_supported{false};
std::atomic<bool> g_dynamic_applied{false};
std::atomic<bool> g_dynamic_declined{false};
std::atomic<bool> g_dynamic_fell_back{false};
std::atomic<uint32_t> g_dynamic_set_failures{0u};
std::atomic<uint32_t> g_dynamic_probe_failures{0u};
std::atomic<uint32_t> g_state_logs{0u};

// ---- The Streamline owner gate (Dynamic MFG) ----
// Dynamic MFG is driven only when the module that owns the real slDLSSGSetOptions points at
// NVIDIA's own Streamline tree: a second hooking add-on sitting in between must not have Dynamic
// MFG driven through it. The capability gate itself stays the provider's own
// bIsDynamicMFGSupported (g_dynamic_support_seen / g_dynamic_supported).
std::atomic<bool> g_streamline_owner_seen{false};

void ObserveStreamlineFunctionOwner(const void* function) {
  HMODULE owner = nullptr;
  if (function == nullptr
      || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(function), &owner) == FALSE
      || owner == nullptr) {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: the wrapped function's owning module could not be resolved");
    return;
  }
  char path_utf8[MAX_PATH * 4] = {};
  wchar_t path[MAX_PATH * 4] = {};
  const DWORD length = GetModuleFileNameW(owner, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return;
  WideCharToMultiByte(CP_UTF8, 0, path, -1, path_utf8, sizeof(path_utf8), nullptr, nullptr);
  _wcslwr_s(path, std::size(path));
  // "sl_dlss_g" covers sl.dlss_g.dll, the \models\sl_dlss_g_0\ tree and the OTA
  // \models\sl_dlss_g_override_*\ tree the driver swaps in (seen in the wild, 2026-10).
  const bool streamline_owner = wcsstr(path, L"sl.dlss_g") != nullptr
                                || wcsstr(path, L"sl_dlss_g") != nullptr
                                || wcsstr(path, L"sl.interposer") != nullptr;
  static std::atomic<bool> owner_logged{false};
  if (!owner_logged.exchange(true, std::memory_order_relaxed)) {
    // Named with the full path: this is where an OptiScaler-style shim (its own sl.interposer)
    // or another add-on in the chain becomes visible as the owner.
    nr::Logf(nr::LogLevel::INFO, "sl-fg: the wrapped function's owner is {} ({} a Streamline owner)", path_utf8,
             streamline_owner ? "is" : "is NOT");
  }
  if (!streamline_owner) return;
  g_streamline_owner_seen.store(true, std::memory_order_release);
}

bool IsTransientDynamicFailure(sl::Result result) {
  switch (result) {
    case sl::Result::eErrorDeviceNotCreated:
    case sl::Result::eErrorNGXFailed:
    case sl::Result::eErrorNotInitialized:
    case sl::Result::eErrorInitNotCalled:
    case sl::Result::eErrorFeatureManagerInvalidState:
    case sl::Result::eErrorInvalidState:
      return true;
    default:
      return false;
  }
}

// ---- The addon-owned options copy (hdr_compat.hpp's BuildAdvancedOptions) ----

bool BuildForwardedOptions(const sl::DLSSGOptions& source, sl::DLSSGOptions& destination,
                           uint32_t generated_frames, bool override_generated, bool enable_dynamic,
                           float dynamicTargetFrameRate) {
  const size_t version = source.structVersion;
  if (version < sl::kStructVersion1 || version > sl::kStructVersion5) return false;
  if (dynamicTargetFrameRate < 0.0f || dynamicTargetFrameRate > 1000.0f) return false;

  destination = sl::DLSSGOptions{};
  destination.next = source.next;
  // The interposer validates the structure GUID on entry: a zeroed structType is refused with
  // eErrorMissingInputParameter no matter what else the copy says. Always carry the game's own.
  destination.structType = source.structType;
  // Generating more than one frame needs the newer options fields (the OTA override provider
  // refuses a v2 request with eErrorMissingInputParameter for 3x/4x): promote to v5 whenever the
  // request is modified, with the added fields at their defaults. eDynamic needs v5 anyway.
  const bool modified = enable_dynamic || (override_generated && generated_frames > 0u);
  const size_t required_version = modified ? sl::kStructVersion5 : sl::kStructVersion1;
  destination.structVersion = version > required_version ? version : required_version;

  destination.mode = enable_dynamic && source.mode != sl::DLSSGMode::eOff ? sl::DLSSGMode::eDynamic : source.mode;
  destination.numFramesToGenerate = override_generated ? generated_frames : source.numFramesToGenerate;
  destination.flags = source.flags;
  destination.dynamicResWidth = source.dynamicResWidth;
  destination.dynamicResHeight = source.dynamicResHeight;
  destination.numBackBuffers = source.numBackBuffers;
  destination.mvecDepthWidth = source.mvecDepthWidth;
  destination.mvecDepthHeight = source.mvecDepthHeight;
  destination.colorWidth = source.colorWidth;
  destination.colorHeight = source.colorHeight;
  destination.colorBufferFormat = source.colorBufferFormat;
  destination.mvecBufferFormat = source.mvecBufferFormat;
  destination.depthBufferFormat = source.depthBufferFormat;
  destination.hudLessBufferFormat = source.hudLessBufferFormat;
  destination.uiBufferFormat = source.uiBufferFormat;
  destination.onErrorCallback = source.onErrorCallback;
  if (version >= sl::kStructVersion2) destination.bReserved15 = source.bReserved15;
  if (version >= sl::kStructVersion3) destination.queueParallelismMode = source.queueParallelismMode;
  destination.enableUserInterfaceRecomposition =
      version >= sl::kStructVersion4 ? source.enableUserInterfaceRecomposition : sl::Boolean::eFalse;
  if (version >= sl::kStructVersion5) destination.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
  if (enable_dynamic) destination.dynamicTargetFrameRate = dynamicTargetFrameRate;
  return true;
}

// ---- Diagnostics ----
// Every wrapper decision logs once per distinct shape: these lines are how a live test says WHY
// an override did or did not land (called at all? which ABI? which branch? what result?).

void LogSlResult(const char* what, sl::Result result) {
  // Edge log: one line per distinct (tag, result) pair, for the process's life. `what` is always a
  // literal here, so its address identifies the branch and a user-space pointer leaves the top bits
  // free.
  //
  // A single "last key" is not enough: two branches that alternate -- Dynamic MFG submits eDynamic
  // and the priority passthrough on every frame -- keep replacing each other's key, so every call
  // looked new and a per-frame game (Phantom Blade Zero) wrote a line per frame, forever. Remembering
  // every pair seen turns it into the edge log it was meant to be.
  const uint64_t key = reinterpret_cast<uint64_t>(what) | (static_cast<uint64_t>(static_cast<uint32_t>(result)) << 48);
  static std::atomic<uint64_t> seen[64]{};
  for (auto& slot : seen) {
    uint64_t known = slot.load(std::memory_order_relaxed);
    if (known == key) return;
    if (known == 0u && slot.compare_exchange_strong(known, key, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::INFO, "sl-fg: {} -> sl result {}", what, static_cast<uint32_t>(result));
      return;
    }
  }
  // 64 distinct pairs: the table is full, so say nothing rather than log every frame.
}

// ---- The wrappers ----

std::atomic<SetOptionsFn> g_real_set_options{nullptr};
std::atomic<GetStateFn> g_real_get_state{nullptr};
std::atomic<void*> g_real_get_feature_function{nullptr};
std::atomic<bool> g_override_applied_logged{false};

// What the runtime last accepted, for the panel's live row (addon/sl_fg_hooks.hpp's LiveState):
// presented frames per real frame, whether the panel's multiplier is the one in force, and whether
// Dynamic MFG owns the choice instead.
std::atomic<uint32_t> g_live_multiplier{1u};
std::atomic<bool> g_live_panel_multiplier{false};  // the accepted count is the panel's, not the game's
std::atomic<bool> g_live_dynamic{false};

void RecordAccepted(uint32_t generated_frames, bool panel_chosen) {
  g_live_multiplier.store(generated_frames + 1u, std::memory_order_relaxed);
  g_live_panel_multiplier.store(panel_chosen, std::memory_order_relaxed);
}

// ---- The live replay ----
// A panel change would otherwise wait for the game's own next slDLSSGSetOptions call, which many
// games only make from their settings menu. The last accepted enabled submission is cached and
// re-submitted from the add-on's Present poll (the game's present thread), once per change; a
// refused replay fails safe back to waiting for the game's own next call.
SRWLOCK g_replay_lock = SRWLOCK_INIT;
sl::ViewportHandle g_replay_viewport{};
sl::DLSSGOptions g_replay_options{};
bool g_replay_valid = false;
std::atomic<bool> g_replay_pending{false};

// ---- The FG model preset's live switch (addon/fg_preset.hpp) ----
// The provider builds its frame-generation model from a configuration it caches at the first FG create,
// and only rebuilds it when that cache is empty -- which the add-on can only empty while no FG feature
// lives (NotifyFgQuiescent). A game that keeps frame generation on therefore never reaches the new
// preset by itself. This controller drives it there: submit Off (without eRetainResourcesWhenOff, so the
// feature is really released), wait for the release to empty the provider's cache, then submit the
// game's own options again -- the create that follows reads the panel's preset. A drain that does not
// land within kDrainTimeoutMs gives up and restores the game's request, so the player never loses frame
// generation to a refused switch.
constexpr uint64_t kDrainTimeoutMs = 4000u;
std::atomic<uint32_t> g_drain{0u};  // 0 = idle, 1 = draining
std::atomic<uint64_t> g_drain_started_ms{0u};
std::atomic<uint64_t> g_drain_resets{0u};  // the provider's reset count when this drain began

bool Draining() {
  return g_drain.load(std::memory_order_acquire) != 0u;
}

// The last accepted enabled submission, for the switch's own submissions: false when the game has not
// made one (nothing to drain from, nothing to resume).
bool LastEnabledRequest(sl::ViewportHandle& viewport, sl::DLSSGOptions& options) {
  AcquireSRWLockShared(&g_replay_lock);
  const bool valid = g_replay_valid;
  if (valid) {
    viewport = g_replay_viewport;
    options = g_replay_options;
  }
  ReleaseSRWLockShared(&g_replay_lock);
  return valid;
}

// The drain's Off submission: the game's own last enabled request with mode Off and the resource
// retention bit cleared, so the provider releases the feature instead of keeping it alive.
bool SubmitDrainOff() {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  sl::ViewportHandle viewport{};
  sl::DLSSGOptions options{};
  if (real == nullptr || !LastEnabledRequest(viewport, options) || options.mode == sl::DLSSGMode::eOff) return false;
  options.mode = sl::DLSSGMode::eOff;
  constexpr auto kRetain = static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff);
  options.flags = static_cast<sl::DLSSGFlags>(static_cast<uint32_t>(options.flags) & ~kRetain);
  const sl::Result result = real(viewport, options);
  LogSlResult("submitted Off for the FG preset switch (resources not retained)", result);
  return result == sl::Result::eOk;
}

// Puts the game's own request back, which is what resumes frame generation after the drain. It goes
// through the panel's wrapper, not the raw function: the multiplier and option rows a player set must
// still stand after the switch -- the drain only borrows the submission to release the model.
sl::Result HookedSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options);

void ResumeGameRequest() {
  sl::ViewportHandle viewport{};
  sl::DLSSGOptions options{};
  if (!LastEnabledRequest(viewport, options)) return;
  LogSlResult("resubmitted the game's own frame-generation options after the FG preset switch",
              HookedSetOptions(viewport, options));
}

void CacheReplaySubmission(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  if (options.mode == sl::DLSSGMode::eOff) return;
  AcquireSRWLockExclusive(&g_replay_lock);
  g_replay_viewport = viewport;
  g_replay_options = options;
  g_replay_valid = true;
  ReleaseSRWLockExclusive(&g_replay_lock);
}

bool DynamicReady(bool game_enabled) {
  // XMX: NVIDIA's Dynamic MFG asks the provider to own the multiplier, but in this mode XeFG owns
  // the generation and its count comes from the ported model, so the row stands down.
  if (g_xmx_blocked.load(std::memory_order_relaxed)) return false;
  return game_enabled && g_dynamic_enabled.load(std::memory_order_relaxed) && g_dynamic_d3d12.load(std::memory_order_relaxed)
         && g_streamline_owner_seen.load(std::memory_order_acquire) && g_dynamic_support_seen.load(std::memory_order_acquire)
         && g_dynamic_supported.load(std::memory_order_relaxed)
         && !g_dynamic_declined.load(std::memory_order_relaxed);
}

// Forwards the game's own request unchanged, keeping the panel's live row honest: the panel's own
// count is not in force here, so the row must not keep showing a number nothing applied.
sl::Result ForwardGameRequest(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options, SetOptionsFn real) {
  const sl::Result result = real(viewport, options);
  if (result == sl::Result::eOk) {
    RecordAccepted(options.numFramesToGenerate, false);
    g_live_dynamic.store(false, std::memory_order_relaxed);
  }
  return result;
}

// Submits `options` (an addon-owned copy when generated_frames is overridden or dynamic is on),
// owning the Dynamic retry ladder. Fixed-multiplier retries and native fallback live in
// HookedSetOptions, so a rejected fixed override is never masked as a success here.
sl::Result CallSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options,
                        uint32_t generated_frames = 0u, bool override_generated = false) {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return sl::Result::eErrorNotInitialized;

  const bool game_enabled = options.mode != sl::DLSSGMode::eOff;
  const bool dynamic = DynamicReady(game_enabled);

  if (!dynamic && !override_generated) return real(viewport, options);

  const float target = static_cast<float>(g_dynamic_target_fps.load(std::memory_order_relaxed));
  sl::DLSSGOptions forwarded{};
  if (!BuildForwardedOptions(options, forwarded, generated_frames, override_generated, dynamic, target)) {
    if (override_generated) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true, std::memory_order_relaxed)) {
        nr::Logf(nr::LogLevel::WARN, "the FG multiplier override refuses an unknown DLSSGOptions ABI; forwarding the game's request unchanged");
      }
    }
    if (dynamic) {
      g_dynamic_applied.store(false, std::memory_order_relaxed);
      g_dynamic_declined.store(true, std::memory_order_release);
      if (!g_dynamic_fell_back.exchange(true, std::memory_order_relaxed)) {
        nr::Logf(nr::LogLevel::WARN, "Dynamic MFG was not submitted: the game supplied an unknown DLSSGOptions ABI; fixed mode remains active");
      }
    }
    return real(viewport, options);
  }

  const sl::Result result = real(viewport, forwarded);
  if (dynamic) LogSlResult("dynamic submission (eDynamic)", result);
  if (result == sl::Result::eOk) {
    // What is in force now, for the panel's row: the accepted count, whether it is the panel's own, and
    // whether Dynamic MFG owns the choice (its multiplier is the provider's, so no number is claimed).
    g_live_dynamic.store(dynamic, std::memory_order_relaxed);
    RecordAccepted(dynamic ? 0u : forwarded.numFramesToGenerate, override_generated);
    if (dynamic) {
      g_dynamic_set_failures.store(0u, std::memory_order_relaxed);
      g_dynamic_fell_back.store(false, std::memory_order_relaxed);
      if (!g_dynamic_applied.exchange(true, std::memory_order_acq_rel)) {
        nr::Logf(nr::LogLevel::INFO, "NVIDIA Dynamic MFG accepted (target {}); multiplier selection and pacing remain provider-controlled",
                 target == 0.0f ? "active-display refresh" : std::to_string(static_cast<unsigned>(target)) + " FPS");
      }
    }
    return result;
  }

  if (dynamic) {
    // One bounded retry without the dynamic mode: if plain options also fail, the failure is the
    // game's own request, not eDynamic.
    sl::DLSSGOptions plain{};
    if (BuildForwardedOptions(options, plain, generated_frames, override_generated, false, 0.0f)) {
      const sl::Result retry = real(viewport, plain);
      if (retry == sl::Result::eOk) {
        if (!g_dynamic_fell_back.exchange(true, std::memory_order_relaxed)) {
          nr::Logf(nr::LogLevel::WARN, "NVIDIA Dynamic MFG was rejected with sl result {}; fixed mode was restored and accepted",
                   static_cast<uint32_t>(result));
        }
        return retry;
      }
    }
    constexpr uint32_t kMaxTransientDynamicFailures = 4u;
    const uint32_t failures = g_dynamic_set_failures.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool retry_later = IsTransientDynamicFailure(result) && failures < kMaxTransientDynamicFailures;
    g_dynamic_declined.store(!retry_later, std::memory_order_release);
    g_dynamic_applied.store(false, std::memory_order_release);
    if (!g_dynamic_fell_back.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::WARN, "NVIDIA Dynamic MFG was rejected with sl result {}; restoring the game's fixed mode. {}",
               static_cast<uint32_t>(result),
               retry_later ? "The next game-side SetOptions call will retry because this result can be transient."
                           : "Toggle frame generation or the addon option to retry.");
    }
    if (override_generated) return result;  // HookedSetOptions owns the fixed-override fallback
    return ForwardGameRequest(viewport, options, real);
  }

  return result;
}

sl::Result HookedSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return sl::Result::eErrorNotInitialized;

  const bool game_enabled = options.mode != sl::DLSSGMode::eOff;
  // A game that submits frame generation on every frame cannot be allowed to cancel the drain by
  // doing so: Phantom Blade Zero re-submits its enabled options each frame, and stepping back on each
  // one ended the drain after 9 ms -- far too short for the provider to release its cached model, so
  // the panel's preset never applied there while every game that submits once (Naraka, and the rest)
  // switched normally. Withhold the game's enabled submissions while draining instead, and hand its
  // own request back the moment the drain ends (ResumeGameRequest already does exactly that).
  if (game_enabled && Draining()) {
    // Cached first: the resume after the drain re-submits the game's newest shape, not the one that
    // happened to be current when the drain began.
    CacheReplaySubmission(viewport, options);
    nr::Logf(nr::LogLevel::TRACE, "sl-fg: the game's enabled submission is held for the running FG preset drain");
    return sl::Result::eOk;
  }
  if (!game_enabled) {
    // A game-side off/on cycle is a safe retry boundary after a rejection.
    g_dynamic_declined.store(false, std::memory_order_release);
    g_dynamic_applied.store(false, std::memory_order_relaxed);
  }

  const bool dynamic_ready = DynamicReady(game_enabled);
  const uint32_t multiplier = g_force_multiplier.load(std::memory_order_relaxed);
  const uint32_t requested = options.numFramesToGenerate;
  if (game_enabled) CacheReplaySubmission(viewport, options);
  // The request line is an edge log too: The Witcher 3 submits the same off shape every frame, and a
  // line per frame buried a session in 21k copies of it. Every field the line prints is in the key.
  const uint64_t shape = (static_cast<uint64_t>(static_cast<uint32_t>(options.mode) & 0xffu))
                         | (static_cast<uint64_t>(options.structVersion & 0xffu) << 8)
                         | (static_cast<uint64_t>(requested & 0xffu) << 16)
                         | (static_cast<uint64_t>(static_cast<uint32_t>(options.hudLessBufferFormat) & 0xffu) << 24)
                         | (static_cast<uint64_t>(static_cast<uint32_t>(options.uiBufferFormat) & 0xffu) << 32)
                         | (static_cast<uint64_t>(multiplier & 0xffu) << 40)
                         | (static_cast<uint64_t>(g_dynamic_enabled.load(std::memory_order_relaxed) ? 1u : 0u) << 48)
                         | (static_cast<uint64_t>(dynamic_ready ? 1u : 0u) << 49);
  // Every shape seen, not just the last one: the off/on shapes a menu toggle alternates between would
  // otherwise each count as a change on every submit, which is the same spam the last-key scheme let
  // through in LogSlResult.
  static std::atomic<uint64_t> known_shapes[16]{};
  bool shape_changed = false;
  for (auto& slot : known_shapes) {
    uint64_t known = slot.load(std::memory_order_relaxed);
    if (known == shape) break;
    if (known == 0u && slot.compare_exchange_strong(known, shape, std::memory_order_relaxed)) {
      shape_changed = true;
      break;
    }
  }
  if (shape_changed) {
    nr::Logf(nr::LogLevel::INFO,
             "sl-fg: slDLSSGSetOptions(mode {}, v{}, numFramesToGenerate {} = {}x, hudless fmt {}, ui fmt {}); panel: multiplier {}, dynamic {} -> {}",
             static_cast<uint32_t>(options.mode), options.structVersion, requested, requested + 1u,
             options.hudLessBufferFormat, options.uiBufferFormat, multiplier,
             g_dynamic_enabled.load(std::memory_order_relaxed) ? "on" : "off",
             dynamic_ready ? "eligible" : "not eligible");
    if (!dynamic_ready && g_dynamic_enabled.load(std::memory_order_relaxed)) {
      // Which gate held it back: exactly what a live test needs to know.
      nr::Logf(nr::LogLevel::INFO,
               "sl-fg: dynamic gates: game_enabled={} enabled={} d3d12={} sl_owner={} support_seen={} supported={} declined={}",
               game_enabled, g_dynamic_enabled.load(std::memory_order_relaxed),
               g_dynamic_d3d12.load(std::memory_order_relaxed),
               g_streamline_owner_seen.load(std::memory_order_acquire),
               g_dynamic_support_seen.load(std::memory_order_acquire), g_dynamic_supported.load(std::memory_order_relaxed),
               g_dynamic_declined.load(std::memory_order_relaxed));
    }
  }

  // Fixed-multiplier force policy (force_policy.hpp): Off is never rewritten; Dynamic owns the
  // selection while it is eligible; otherwise 2..6 replaces the game's count with multiplier-1.
  if (!game_enabled || dynamic_ready) {
    const sl::Result result = !game_enabled ? real(viewport, options) : CallSetOptions(viewport, options);
    LogSlResult(!game_enabled ? "forwarded the game's Off request unchanged" : "dynamic-priority passthrough", result);
    return result;
  }

  if (multiplier < 2u || multiplier > kMaxMultiplier) {
    const sl::Result result = ForwardGameRequest(viewport, options, real);
    LogSlResult("no panel multiplier selected; forwarded unchanged", result);
    return result;
  }

  const uint32_t desired = multiplier - 1u;
  if (desired == requested) {
    const sl::Result result = ForwardGameRequest(viewport, options, real);
    LogSlResult("the game's request already equals the panel's multiplier; forwarded", result);
    return result;
  }

  if (options.structVersion < sl::kStructVersion1 || options.structVersion > sl::kStructVersion5) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::WARN, "the FG multiplier override refuses an unknown DLSSGOptions ABI; forwarding the game's request unchanged");
    }
    const sl::Result result = real(viewport, options);
    LogSlResult("unknown DLSSGOptions ABI; forwarded unchanged", result);
    return result;
  }

  sl::Result result = CallSetOptions(viewport, options, desired, true);
  LogSlResult("override submission (attempt 1)", result);
  if (result != sl::Result::eOk) {
    // eErrorFeatureManagerInvalidState can simply mean DLSS-G was not ready yet. Retry once
    // with the SAME overridden count before giving the count up as refused (MFGAdaUnlock's
    // lesson: dropping straight back would silently discard a working 4x).
    const sl::Result retry = CallSetOptions(viewport, options, desired, true);
    LogSlResult("override submission (retry)", retry);
    if (retry == sl::Result::eOk) {
      nr::Logf(nr::LogLevel::INFO, "slDLSSGSetOptions returned {} on the first attempt but accepted the override on retry; that first failure was feature-manager state",
               static_cast<uint32_t>(result));
      result = retry;
    } else {
      if (g_force_failed_for.exchange(desired, std::memory_order_relaxed) != desired) {
        nr::Logf(nr::LogLevel::WARN, "slDLSSGSetOptions refused numFramesToGenerate={} twice (results {} then {}); falling back to the game's own request of {}. The count itself is being refused",
                 desired, static_cast<uint32_t>(result), static_cast<uint32_t>(retry), requested);
      }
      // Never leave the player without FG: put the game's own request through.
      const sl::Result fallback = real(viewport, options);
      LogSlResult("fallback to the game's own request after a refused override", fallback);
      return fallback;
    }
  }

  if (!g_override_applied_logged.exchange(true, std::memory_order_relaxed)) {
    nr::Logf(nr::LogLevel::INFO, "overriding DLSS-G numFramesToGenerate from {} ({}x) to {} ({}x); accepted",
             requested, requested + 1u, desired, multiplier);
  }
  return result;
}

sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state, const sl::DLSSGOptions* options) {
  // Another addon or a reloaded wrapper can hand out a pointer that calls back into this
  // wrapper; re-entering would overflow the stack.
  thread_local bool call_active = false;
  if (call_active) return sl::Result::eErrorInvalidState;

  const auto real = g_real_get_state.load(std::memory_order_acquire);
  if (real == nullptr) return sl::Result::eErrorNotInitialized;

  const size_t caller_version = state.structVersion;
  sl::Result result;
  sl::DLSSGState extended{};
  constexpr uint32_t kMaxDynamicProbeFailures = 8u;
  // Games older than the v4 state ABI cannot see bIsDynamicMFGSupported; query v4 into
  // addon-owned storage and copy back only what the caller's ABI guarantees.
  const bool probe_dynamic_state =
      caller_version >= sl::kStructVersion1 && caller_version < sl::kStructVersion4
      && g_dynamic_probe_failures.load(std::memory_order_relaxed) < kMaxDynamicProbeFailures;
  // Some games poll GetState every frame: log the first few calls only.
  const uint32_t logged = g_state_logs.fetch_add(1u, std::memory_order_relaxed);
  if (logged < 10u) {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: slDLSSGGetState(v{}), probing v4 for Dynamic support: {}", caller_version,
             probe_dynamic_state ? "yes" : "no");
  }
  const sl::DLSSGState* observed = &state;
  if (probe_dynamic_state) {
    // Query v4 into addon-owned storage: the header must name the v4 ABI (a zero-initialized
    // structVersion would query nothing and no dynamic flag would ever come back).
    extended.next = state.next;
    extended.structType = state.structType;
    extended.structVersion = sl::kStructVersion4;
    call_active = true;
    result = real(viewport, extended, options);
    call_active = false;
    if (result == sl::Result::eOk) {
      g_dynamic_probe_failures.store(0u, std::memory_order_relaxed);
      observed = &extended;
      state.estimatedVRAMUsageInBytes = extended.estimatedVRAMUsageInBytes;
      state.status = extended.status;
      state.minWidthOrHeight = extended.minWidthOrHeight;
      state.numFramesActuallyPresented = extended.numFramesActuallyPresented;
      if (caller_version >= sl::kStructVersion2) {
        state.numFramesToGenerateMax = extended.numFramesToGenerateMax;
        state.bReserved4 = extended.bReserved4;
        state.bIsVsyncSupportAvailable = extended.bIsVsyncSupportAvailable;
        state.inputsProcessingCompletionFence = extended.inputsProcessingCompletionFence;
        state.lastPresentInputsProcessingCompletionFenceValue = extended.lastPresentInputsProcessingCompletionFenceValue;
      }
    } else {
      const bool structurally_unsupported = result == sl::Result::eErrorInvalidParameter
                                            || result == sl::Result::eErrorUnsupportedInterface
                                            || result == sl::Result::eErrorFeatureNotSupported;
      g_dynamic_probe_failures.store(structurally_unsupported ? kMaxDynamicProbeFailures : g_dynamic_probe_failures.load(std::memory_order_relaxed) + 1u,
                                     std::memory_order_relaxed);
      call_active = true;
      result = real(viewport, state, options);
      call_active = false;
    }
  } else {
    call_active = true;
    result = real(viewport, state, options);
    call_active = false;
  }

  if (logged < 10u) {
    const int dynamic_flag = observed->structVersion >= sl::kStructVersion4 ? static_cast<int>(observed->bIsDynamicMFGSupported) : -1;
    nr::Logf(nr::LogLevel::INFO, "sl-fg: slDLSSGGetState -> result {}, max generated {}, dynamic flag {} (state v{})",
             static_cast<uint32_t>(result), observed->numFramesToGenerateMax, dynamic_flag, observed->structVersion);
  }
  // The presented batch, for the frame-rate row: one sample per real frame, the last one of the frame
  // winning (several GetState calls read the same batch).
  if (result == sl::Result::eOk && observed->structVersion >= sl::kStructVersion1) {
    framerate::OnPresentedBatch(observed->numFramesActuallyPresented);
  }
  if (result == sl::Result::eOk) {
    if (observed->structVersion >= sl::kStructVersion4
        && (observed->bIsDynamicMFGSupported == sl::Boolean::eTrue || observed->bIsDynamicMFGSupported == sl::Boolean::eFalse)) {
      const bool supported = observed->bIsDynamicMFGSupported == sl::Boolean::eTrue;
      const bool previous = g_dynamic_supported.exchange(supported, std::memory_order_acq_rel);
      const bool was_seen = g_dynamic_support_seen.exchange(true, std::memory_order_release);
      if (!was_seen || previous != supported) {
        nr::Logf(nr::LogLevel::INFO, "slDLSSGGetState {} NVIDIA Dynamic MFG support",
                 supported ? "confirms" : "reports no");
      }
    }
  }

  // The runtime's own ceiling, before this add-on advertises more (max-so-far: a runtime may report
  // less until DLSS-G is enabled). The FG row's 5x/6x and the advertisement below stop above it.
  if (result == sl::Result::eOk && (observed != &state || caller_version >= sl::kStructVersion2)) {
    const uint32_t provider_max = observed->numFramesToGenerateMax;
    uint32_t known = g_provider_max_generated.load(std::memory_order_relaxed);
    while (known < provider_max
           && !g_provider_max_generated.compare_exchange_weak(known, provider_max, std::memory_order_relaxed)) {
    }
  }

  // Raise the ceiling for games that build their native 2x/3x/4x selector from
  // numFramesToGenerateMax (STALKER 2), never above what the runtime itself reports.
  if (result != sl::Result::eOk || caller_version < sl::kStructVersion2) return result;
  const uint32_t wanted = g_advertised_max.load(std::memory_order_relaxed);
  const uint32_t provider_max = g_provider_max_generated.load(std::memory_order_relaxed);
  const uint32_t advertised = (provider_max != 0u && wanted > provider_max) ? provider_max : wanted;
  if (advertised < 2u || state.numFramesToGenerateMax >= advertised) return result;
  state.numFramesToGenerateMax = advertised;
  return result;
}

sl::Result HookedGetFeatureFunction(sl::Feature feature, const char* function_name, void*& function) {
  const auto real = reinterpret_cast<GetFeatureFunctionFn>(g_real_get_feature_function.load(std::memory_order_acquire));
  if (real == nullptr) return sl::Result::eErrorInvalidParameter;
  const sl::Result result = real(feature, function_name, function);
  // Log every DLSS-G / Reflex function the game asks for: if the wrapper never fires, these lines
  // show what names this integration actually queries (OptiScaler and other shims included).
  if (function_name != nullptr && (feature == sl::kFeatureDLSS_G || strstr(function_name, "Reflex") != nullptr)) {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: slGetFeatureFunction(feature {}, '{}') -> {} (result {})",
             static_cast<uint32_t>(feature), function_name, function != nullptr ? "a pointer" : "null",
             static_cast<uint32_t>(result));
  }
  if (result != sl::Result::eOk || function_name == nullptr || function == nullptr) return result;
  if (feature == sl::kFeatureDLSS_G && strcmp(function_name, "slDLSSGSetOptions") == 0
      && function != reinterpret_cast<void*>(&HookedSetOptions)) {
    ObserveStreamlineFunctionOwner(function);
    g_real_set_options.store(reinterpret_cast<SetOptionsFn>(function), std::memory_order_release);
    function = reinterpret_cast<void*>(&HookedSetOptions);
    nr::Logf(nr::LogLevel::INFO, "wrapped slDLSSGSetOptions; the FG multiplier override is live");
  } else if (feature == sl::kFeatureDLSS_G && strcmp(function_name, "slDLSSGGetState") == 0
             && function != reinterpret_cast<void*>(&HookedGetState)) {
    ObserveStreamlineFunctionOwner(function);
    g_real_get_state.store(reinterpret_cast<GetStateFn>(function), std::memory_order_release);
    function = reinterpret_cast<void*>(&HookedGetState);
    nr::Logf(nr::LogLevel::INFO, "wrapped slDLSSGGetState; the multiplier-menu ceiling and Dynamic MFG probing are live");
  }
  return result;
}

// ---- Installation ----
// The game asks slGetFeatureFunction for slDLSSGSetOptions once, during DLSS-G setup, so the
// hook must land before that -- from every poll this add-on gets, plus a short watchdog once
// sl.interposer.dll appears (fg_preset.cpp's pattern).

std::atomic<uint32_t> g_install{0u};  // 0 = not tried, 1 = hooked, 2 = failed, 3/4 = in progress / watchdog started

void InstallSlFgHooks() {
  uint32_t state = g_install.load(std::memory_order_relaxed);
  if (state == 1u || state == 2u) return;
  if (!g_install.compare_exchange_strong(state, 3u, std::memory_order_acq_rel)) return;

  const HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  const auto target = reinterpret_cast<GetFeatureFunctionFn>(
      interposer == nullptr ? nullptr : GetProcAddress(interposer, "slGetFeatureFunction"));
  if (target == nullptr) {
    g_install.store(0u, std::memory_order_release);  // not mapped yet: keep polling
    return;
  }
  // Streamline stays loaded for the process; pin it like every other hook target.
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(target), &pinned);

  if (const MH_STATUS status = hooks::EnsureMinHook(); status != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "the FG multiplier hooks could not start MinHook: {}", MH_StatusToString(status));
    g_install.store(2u, std::memory_order_release);
    return;
  }

  void* original = nullptr;
  if (MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&HookedGetFeatureFunction), &original) != MH_OK
      || MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "the FG multiplier hooks could not hook slGetFeatureFunction; the FG multiplier rows stay off in this game");
    g_install.store(2u, std::memory_order_release);
    return;
  }
  g_real_get_feature_function.store(original, std::memory_order_release);
  hooks::HoldMinHook();
  g_install.store(1u, std::memory_order_release);
  // The full path, not just "sl.interposer.dll": a process can hold more than one copy (the game's
  // own and a loader's), and GetModuleHandleW names whichever the loader mapped under that name. When
  // the FG rows stay dead while this line says the hook installed, the next question is which copy the
  // game actually calls -- and this is the only place that answers it.
  wchar_t hooked_path[512] = {};
  GetModuleFileNameW(interposer, hooked_path, static_cast<DWORD>(std::size(hooked_path)));
  nr::Logf(nr::LogLevel::INFO, "the FG multiplier hooks hooked slGetFeatureFunction at {} in {}", reinterpret_cast<void*>(target),
           Utf8FromPath(hooked_path));
}

// Probes the provider's Dynamic-MFG support flag directly: many games call GetState once, before
// enabling FG, so the wrapper's in-transit probe alone never sees the flag. Bounded to a handful
// of attempts; called from the Present poll while Dynamic is wanted but unsupported-unseen.
void ProbeDynamicSupport(const sl::ViewportHandle& viewport) {
  static std::atomic<uint32_t> active_probes{0u};
  if (active_probes.load(std::memory_order_relaxed) >= 8u) return;
  const auto real = g_real_get_state.load(std::memory_order_acquire);
  if (real == nullptr) return;
  sl::DLSSGState probe{};
  probe.structType = sl::DLSSGState::s_structType;
  probe.structVersion = sl::kStructVersion4;
  thread_local bool call_active = false;
  if (call_active) return;
  call_active = true;
  const sl::Result result = real(viewport, probe, nullptr);
  call_active = false;
  active_probes.fetch_add(1u, std::memory_order_relaxed);
  const bool flag_valid = probe.structVersion >= sl::kStructVersion4
                          && (probe.bIsDynamicMFGSupported == sl::Boolean::eTrue
                              || probe.bIsDynamicMFGSupported == sl::Boolean::eFalse);
  if (result == sl::Result::eOk && flag_valid) {
    const bool supported = probe.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    g_dynamic_supported.store(supported, std::memory_order_release);
    g_dynamic_support_seen.store(true, std::memory_order_release);
    nr::Logf(nr::LogLevel::INFO, "sl-fg: active Dynamic support probe: {} (provider max generated {})",
             supported ? "supported" : "not supported", probe.numFramesToGenerateMax);
  } else {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: active Dynamic support probe: result {}, state v{}, flag {}",
             static_cast<uint32_t>(result), probe.structVersion, static_cast<int>(probe.bIsDynamicMFGSupported));
  }
}

}  // namespace

void SetForceMultiplier(uint32_t multiplier) {
  const uint32_t clamped = multiplier >= 2u && multiplier <= kMaxMultiplier ? multiplier : 0u;
  const uint32_t previous = g_force_multiplier.exchange(clamped, std::memory_order_relaxed);
  if (previous == clamped) return;  // the choice is already in force: re-selecting it is not a change
  g_force_failed_for.store(UINT32_MAX, std::memory_order_relaxed);
  g_replay_pending.store(true, std::memory_order_release);  // the Present poll re-submits it live
}

void SetDynamicMfg(bool enabled, uint32_t target_fps) {
  const uint32_t clamped_target = target_fps > 600u ? 0u : target_fps;
  const bool was = g_dynamic_enabled.exchange(enabled, std::memory_order_relaxed);
  const uint32_t previous_target = g_dynamic_target_fps.exchange(clamped_target, std::memory_order_relaxed);
  // A new target only matters while Dynamic MFG is on: with it off the replay would re-submit the
  // game's own request unchanged.
  if (was == enabled && (previous_target == clamped_target || !enabled)) return;
  g_replay_pending.store(true, std::memory_order_release);
  if (was != enabled) {
    // A toggle is a safe retry boundary: clear every declined/fallback latch.
    g_dynamic_applied.store(false, std::memory_order_relaxed);
    g_dynamic_fell_back.store(false, std::memory_order_relaxed);
    g_dynamic_declined.store(false, std::memory_order_release);
    g_dynamic_set_failures.store(0u, std::memory_order_relaxed);
    g_dynamic_probe_failures.store(0u, std::memory_order_relaxed);
  }
}

void SetAdvertisedMaxGenerated(uint32_t generated_frames) {
  g_advertised_max.store(generated_frames, std::memory_order_relaxed);
}

void SetXmxMode(bool xmx) {
  const bool was = g_xmx_blocked.exchange(xmx, std::memory_order_relaxed);
  if (was == xmx) return;
  g_replay_pending.store(true, std::memory_order_release);
  nr::Logf(nr::LogLevel::INFO, "sl-fg: XMX mode {}; NVIDIA's Dynamic MFG stands down", xmx ? "on" : "off");
}

uint32_t GeneratedFramesCeiling() {
  return g_provider_max_generated.load(std::memory_order_relaxed);
}

void NotifyDynamicD3D12(bool d3d12) {
  g_dynamic_d3d12.store(d3d12, std::memory_order_relaxed);
}

// ---- The FG model preset's live switch, driven from the Present poll ----
// Called every present (cheap: two relaxed loads while nothing is wanted). It starts a drain when the
// panel's FG preset differs from what the provider last served, and ends it once the provider's cache
// was reset (the next create rebuilds the model) or the deadline passes.
void FgPresetTick() {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return;  // no Streamline: the FG preset lands at the next start, as before
  const uint64_t now = GetTickCount64();
  if (Draining()) {
    const bool reset = addon::FgPresetResets() != g_drain_resets.load(std::memory_order_relaxed);
    if (reset || (now - g_drain_started_ms.load(std::memory_order_relaxed)) >= kDrainTimeoutMs) {
      // Either the reset landed (the provider's next create reads the panel's preset) or it did not in
      // time: both end the same way, with the game's own frame generation back on.
      g_drain.store(0u, std::memory_order_release);
      ResumeGameRequest();
    }
    return;
  }
  // Only ask while a drain is possible at all: the pending query consumes the request, and a game that
  // never enabled frame generation has nothing to submit Off from.
  sl::ViewportHandle viewport{};
  sl::DLSSGOptions options{};
  if (!LastEnabledRequest(viewport, options)) return;
  if (!addon::FgPresetSwitchPending()) return;
  if (!SubmitDrainOff()) {
    // No enabled request to drain from, or the runtime refused: the preset lands at the next start.
    ResumeGameRequest();
    return;
  }
  g_drain_started_ms.store(now, std::memory_order_relaxed);
  g_drain_resets.store(addon::FgPresetResets(), std::memory_order_relaxed);
  g_drain.store(1u, std::memory_order_release);
  nr::Logf(nr::LogLevel::INFO,
           "the FG preset switch turned frame generation off for one drain; it resumes as soon as the provider has released its cached model");
}

LiveState ReadLiveState() {
  return {
      .multiplier = g_live_multiplier.load(std::memory_order_relaxed),
      .panel_multiplier = g_live_panel_multiplier.load(std::memory_order_relaxed),
      .dynamic = g_live_dynamic.load(std::memory_order_relaxed),
      .streamline = (g_real_set_options.load(std::memory_order_acquire) != nullptr),
      .switching = Draining(),
      .dynamic_wanted = g_dynamic_enabled.load(std::memory_order_relaxed),
      .dynamic_ready = DynamicReady(true),
      .dynamic_supported = g_dynamic_supported.load(std::memory_order_relaxed),
      .dynamic_support_known = g_dynamic_support_seen.load(std::memory_order_acquire),
      .dynamic_declined = g_dynamic_declined.load(std::memory_order_relaxed),
      .d3d12 = g_dynamic_d3d12.load(std::memory_order_relaxed),
  };
}

void EnsureSlFgHooks() {
  uint32_t state = g_install.load(std::memory_order_relaxed);
  if (state != 0u) return;
  if (GetModuleHandleW(L"sl.interposer.dll") != nullptr) {
    InstallSlFgHooks();
    return;
  }
  if (!g_install.compare_exchange_strong(state, 4u, std::memory_order_acq_rel)) return;
  std::thread([] {
    while (GetModuleHandleW(L"sl.interposer.dll") == nullptr) Sleep(25);
    InstallSlFgHooks();
  }).detach();
}

// Re-submits the cached enabled submission through the full wrapper logic so a panel change lands
// without the game's own next call. Called from the add-on's Present poll (the game's present
// thread). A refusal inside the wrapper fails safe exactly as a game-side call would.
void RefreshFromPanel() {
  if (!g_replay_pending.exchange(false, std::memory_order_acq_rel)) return;
  sl::ViewportHandle viewport{};
  sl::DLSSGOptions options{};
  bool valid = false;
  AcquireSRWLockShared(&g_replay_lock);
  if (g_replay_valid) {
    viewport = g_replay_viewport;
    options = g_replay_options;
    valid = true;
  }
  ReleaseSRWLockShared(&g_replay_lock);
  if (!valid) return;  // no enabled SetOptions yet: the change applies when the game makes one
  const bool dynamic = g_dynamic_enabled.load(std::memory_order_relaxed);
  // Replaying also on the way back to Game: the wrapper forwards the game's own cached request
  // unchanged, which is what restores its multiplier after a forced value stood in.
  if (dynamic && !g_dynamic_support_seen.load(std::memory_order_acquire)) {
    ProbeDynamicSupport(viewport);  // the game's own GetState may predate FG being enabled
  }
  HookedSetOptions(viewport, options);
}

}  // namespace uplift::addon::slfg
