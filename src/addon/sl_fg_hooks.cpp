#include "addon/sl_fg_hooks.hpp"

#include <MinHook.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include <psapi.h>

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
// The resource-tag entry points: the header marks PFun_slSetTag itself deprecated, so the legacy
// signature is restated here rather than used through that alias.
using SetTagForFrameFn = sl::Result(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using SetTagFn = sl::Result(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);

// ---- The panel's choices ----

std::atomic<uint32_t> g_force_multiplier{0u};   // 0 = follow the game; 2..kMaxMultiplier = force
// The provider reports numFramesToGenerateMax 5 (6x) on this stack; the panel offers up to 6x.
constexpr uint32_t kMaxMultiplier = 6u;
std::atomic<bool> g_dynamic_enabled{false};
std::atomic<uint32_t> g_dynamic_target_fps{0u};  // 0 = the display's refresh rate
std::atomic<uint32_t> g_advertised_max{0u};      // GetState ceiling; 0 = do not touch
std::atomic<uint32_t> g_provider_max_generated{0u};  // the runtime's own numFramesToGenerateMax; 0 = not seen
std::atomic<bool> g_dynamic_d3d12{false};
// The two DLSS-G option fields (2026-10-26): 0 = Game (leave the game's value), 1 = On, 2 = Off.
std::atomic<uint32_t> g_ui_recomposition{0u};
std::atomic<uint32_t> g_menu_detection{0u};
// Which of the two buffers those options need the game to tag: bit 0 = kBufferTypeHUDLessColor,
// bit 1 = kBufferTypeUIColorAndAlpha. The options' hudLessBufferFormat/uiBufferFormat fields are
// optional declarations (the SDK guide never fills them), so only the tags themselves tell.
std::atomic<uint32_t> g_tag_mask{0u};

bool OptionOverridesSet() {
  return g_ui_recomposition.load(std::memory_order_relaxed) != 0u || g_menu_detection.load(std::memory_order_relaxed) != 0u;
}

const char* OptionOverrideName(uint32_t value) {
  switch (value) {
    case 1u: return "on";
    case 2u: return "off";
    default: return "game";
  }
}

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

// ---- The version stack gate (Dynamic MFG) ----
// NVIDIA releases Dynamic MFG on Streamline 2.14.1/2.14.16 with DLSS-G 310.9.1/310.9.16, but this
// project does not gate on those numbers: the authoritative gate is the provider's own
// bIsDynamicMFGSupported (see DynamicVersionStackReady). All that is needed here is the file
// version, as proof that the two modules were actually found.

struct SlVersion {
  uint16_t major = 0, minor = 0, patch = 0, revision = 0;
  bool valid = false;
};

bool IsMappedImage(HMODULE module) {
  if (module == nullptr) return false;
  MEMORY_BASIC_INFORMATION info{};
  return VirtualQuery(module, &info, sizeof(info)) == sizeof(info) && info.AllocationBase == module
         && info.Type == MEM_IMAGE && info.State == MEM_COMMIT;
}

SlVersion VersionFromModule(HMODULE module) {
  SlVersion result{};
  if (!IsMappedImage(module)) return result;
  wchar_t path[MAX_PATH * 4] = {};
  const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return result;
  DWORD ignored = 0;
  const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
  if (bytes == 0) return result;
  std::vector<unsigned char> storage(bytes);
  if (!GetFileVersionInfoW(path, 0, bytes, storage.data())) return result;
  VS_FIXEDFILEINFO* fixed_info = nullptr;
  UINT fixed_size = 0;
  if (!VerQueryValueW(storage.data(), L"\\", reinterpret_cast<void**>(&fixed_info), &fixed_size)
      || fixed_info == nullptr || fixed_size < sizeof(VS_FIXEDFILEINFO)
      || fixed_info->dwSignature != 0xFEEF04BDu) {
    return result;
  }
  result.major = HIWORD(fixed_info->dwFileVersionMS);
  result.minor = LOWORD(fixed_info->dwFileVersionMS);
  result.patch = HIWORD(fixed_info->dwFileVersionLS);
  result.revision = LOWORD(fixed_info->dwFileVersionLS);
  result.valid = true;
  return result;
}

// The Streamline wrapper's own version, taken from the module that owns the real function
// pointer. A second Streamline-hooking addon sitting in between must not have its file version
// mistaken for NVIDIA's, so only sl.dlss_g / sl.interposer paths count.
std::atomic<bool> g_streamline_version_seen{false};
std::atomic<bool> g_dlssg_version_seen{false};

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
  const SlVersion version = VersionFromModule(owner);
  if (!version.valid) {
    nr::Logf(nr::LogLevel::WARN, "sl-fg: the Streamline owner's file version could not be read ({})", path_utf8);
    return;
  }
  if (g_streamline_version_seen.exchange(true, std::memory_order_acq_rel)) return;  // keep the first observation
  nr::Logf(nr::LogLevel::INFO, "the FG multiplier hooks observed Streamline {}.{}.{}.{} (version readable; Dynamic MFG itself is gated on the provider's bIsDynamicMFGSupported)",
           version.major, version.minor, version.patch, version.revision);
}

// The DLSS-G provider (sl.dlss_g) module, found by name among the mapped images. Dynamic MFG's
// allowlist needs its file version; the scan runs at most until a valid version is seen.
HMODULE FindDlssgProvider() {
  std::vector<HMODULE> loaded(256u);
  while (true) {
    DWORD needed = 0u;
    if (K32EnumProcessModules(GetCurrentProcess(), loaded.data(), static_cast<DWORD>(loaded.size() * sizeof(HMODULE)), &needed) == FALSE) {
      return nullptr;
    }
    const size_t count = needed / sizeof(HMODULE);
    if (count <= loaded.size()) {
      loaded.resize(count);
      break;
    }
    loaded.resize(count);
  }
  for (const HMODULE candidate : loaded) {
    if (candidate == nullptr) continue;
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(candidate, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) continue;
    _wcslwr_s(path, std::size(path));
    if (wcsstr(path, L"sl.dlss_g") != nullptr || wcsstr(path, L"sl_dlss_g") != nullptr) {
      return candidate;
    }
  }
  return nullptr;
}

void ObserveDlssgProviderVersion() {
  if (g_dlssg_version_seen.load(std::memory_order_acquire)) return;
  const HMODULE provider = FindDlssgProvider();
  if (provider == nullptr) {
    // Bounded: also says what the scan looked for, in case this chain names its FG provider
    // differently (an OptiScaler-style shim would).
    static std::atomic<uint32_t> misses{0u};
    if (misses.fetch_add(1u, std::memory_order_relaxed) == 4u) {
      nr::Logf(nr::LogLevel::INFO, "sl-fg: no mapped module named sl.dlss_g* found for the DLSS-G provider version check");
    }
    return;
  }
  const SlVersion version = VersionFromModule(provider);
  if (!version.valid) return;
  g_dlssg_version_seen.store(true, std::memory_order_release);
  nr::Logf(nr::LogLevel::INFO, "the FG multiplier hooks observed the DLSS-G provider {}.{}.{}.{} (version readable; Dynamic MFG itself is gated on the provider's bIsDynamicMFGSupported)",
           version.major, version.minor, version.patch, version.revision);
}

bool DynamicVersionStackReady() {
  // The authoritative gate is the provider's own bIsDynamicMFGSupported from GetState: this project
  // ships no kernel patches whose safety exact versions would guard, and the OTA override tree
  // reports versions a fixed list would never anticipate. Both versions must merely be readable,
  // proving the modules were actually found.
  return g_streamline_version_seen.load(std::memory_order_acquire) && g_dlssg_version_seen.load(std::memory_order_acquire);
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
  const uint32_t ui_recomposition = g_ui_recomposition.load(std::memory_order_relaxed);
  const uint32_t menu_detection = g_menu_detection.load(std::memory_order_relaxed);
  // Generating more than one frame needs the newer options fields (the OTA override provider
  // refuses a v2 request with eErrorMissingInputParameter for 3x/4x): promote to v5 whenever the
  // request is modified, with the added fields at their defaults. eDynamic needs v5 anyway, and
  // the option overrides need at least v4 (UI recomposition's own field).
  const bool modified = enable_dynamic || (override_generated && generated_frames > 0u) || ui_recomposition != 0u || menu_detection != 0u;
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
  // The panel's DLSS-G option rows: an explicit On/Off replaces the game's value (the copy is
  // promoted to v5 above, so the v4 field is always there), Game leaves it as copied.
  if (ui_recomposition != 0u) {
    destination.enableUserInterfaceRecomposition = ui_recomposition == 1u ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  }
  if (menu_detection != 0u) {
    // SL_ENUM_OPERATORS_32 gives DLSSGFlags no complement, so the clear goes through the number.
    constexpr auto kMenuDetection = static_cast<uint32_t>(sl::DLSSGFlags::eEnableFullscreenMenuDetection);
    uint32_t flags = static_cast<uint32_t>(destination.flags);
    flags = menu_detection == 1u ? (flags | kMenuDetection) : (flags & ~kMenuDetection);
    destination.flags = static_cast<sl::DLSSGFlags>(flags);
  }
  return true;
}

// ---- Diagnostics ----
// Every wrapper decision logs once per distinct shape: these lines are how a live test says WHY
// an override did or did not land (called at all? which ABI? which branch? what result?).

void LogSlResult(const char* what, sl::Result result) {
  nr::Logf(nr::LogLevel::INFO, "sl-fg: {} -> sl result {}", what, static_cast<uint32_t>(result));
}

// ---- The wrappers ----

std::atomic<SetOptionsFn> g_real_set_options{nullptr};
std::atomic<GetStateFn> g_real_get_state{nullptr};
std::atomic<void*> g_real_get_feature_function{nullptr};
std::atomic<bool> g_override_applied_logged{false};

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

void CacheReplaySubmission(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  if (options.mode == sl::DLSSGMode::eOff) return;
  AcquireSRWLockExclusive(&g_replay_lock);
  g_replay_viewport = viewport;
  g_replay_options = options;
  g_replay_valid = true;
  ReleaseSRWLockExclusive(&g_replay_lock);
}

bool DynamicReady(bool game_enabled) {
  return game_enabled && g_dynamic_enabled.load(std::memory_order_relaxed) && g_dynamic_d3d12.load(std::memory_order_relaxed)
         && DynamicVersionStackReady() && g_dynamic_support_seen.load(std::memory_order_acquire)
         && g_dynamic_supported.load(std::memory_order_relaxed)
         && !g_dynamic_declined.load(std::memory_order_relaxed);
}

// Forwards the game's own request with only the panel's DLSS-G option rows applied: the branches
// that do not rewrite the count still have to carry them, or choosing UI recomposition on its own
// would never reach the runtime.
sl::Result ForwardGameRequest(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options, SetOptionsFn real) {
  if (!OptionOverridesSet()) return real(viewport, options);
  sl::DLSSGOptions forwarded{};
  if (!BuildForwardedOptions(options, forwarded, 0u, false, false, 0.0f)) return real(viewport, options);
  const sl::Result result = real(viewport, forwarded);
  // A provider that does not know the promoted ABI may refuse the copy outright: put the game's
  // own request through so an option override can never cost frame generation.
  if (result == sl::Result::eOk) return result;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true, std::memory_order_relaxed)) {
    nr::Logf(nr::LogLevel::WARN,
             "the panel's DLSS-G option overrides were refused with sl result {}; the game's own request was submitted instead. Toggle the option rows or frame generation to retry",
             static_cast<uint32_t>(result));
  }
  return real(viewport, options);
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

  if (!dynamic && !override_generated && !OptionOverridesSet()) return real(viewport, options);

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
  if (dynamic) {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: dynamic submission (eDynamic, target {}) -> sl result {}", target == 0.0f ? "active-display refresh" : std::to_string(static_cast<unsigned>(target)) + " FPS",
             static_cast<uint32_t>(result));
  }
  if (result == sl::Result::eOk) {
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
  if (!game_enabled) {
    // A game-side off/on cycle is a safe retry boundary after a rejection.
    g_dynamic_declined.store(false, std::memory_order_release);
    g_dynamic_applied.store(false, std::memory_order_relaxed);
  }
  ObserveDlssgProviderVersion();

  const bool dynamic_ready = DynamicReady(game_enabled);
  const uint32_t multiplier = g_force_multiplier.load(std::memory_order_relaxed);
  const uint32_t requested = options.numFramesToGenerate;
  if (game_enabled) CacheReplaySubmission(viewport, options);
  nr::Logf(nr::LogLevel::INFO,
           "sl-fg: slDLSSGSetOptions(mode {}, v{}, numFramesToGenerate {} = {}x, hudless fmt {}, ui fmt {}); panel: multiplier {}, dynamic {} -> {}",
           static_cast<uint32_t>(options.mode), options.structVersion, requested, requested + 1u,
           options.hudLessBufferFormat, options.uiBufferFormat, multiplier,
           g_dynamic_enabled.load(std::memory_order_relaxed) ? "on" : "off",
           dynamic_ready ? "eligible" : "not eligible");
  if (!dynamic_ready && g_dynamic_enabled.load(std::memory_order_relaxed)) {
    // Which gate held it back: exactly what a live test needs to know.
    nr::Logf(nr::LogLevel::INFO,
             "sl-fg: dynamic gates: game_enabled={} enabled={} d3d12={} versions_seen({}/{}) support_seen={} supported={} declined={}",
             game_enabled, g_dynamic_enabled.load(std::memory_order_relaxed),
             g_dynamic_d3d12.load(std::memory_order_relaxed),
             g_streamline_version_seen.load(std::memory_order_acquire), g_dlssg_version_seen.load(std::memory_order_acquire),
             g_dynamic_support_seen.load(std::memory_order_acquire), g_dynamic_supported.load(std::memory_order_relaxed),
             g_dynamic_declined.load(std::memory_order_relaxed));
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
    LogSlResult(OptionOverridesSet() ? "no panel multiplier selected; forwarded with the panel's DLSS-G options"
                                     : "no panel multiplier selected; forwarded unchanged",
                result);
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

void InstallTagProbe(HMODULE interposer);

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

  InstallTagProbe(interposer);

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
  nr::Logf(nr::LogLevel::INFO, "the FG multiplier hooks hooked sl.interposer.dll's slGetFeatureFunction at {}", reinterpret_cast<void*>(target));
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

// ---- Resource-tag probe (2026-10-26) ----
// UI recomposition needs the game to tag a HUD-less colour buffer and a UI colour-and-alpha buffer;
// fullscreen menu detection needs the latter alone. Watching the game's own slSetTag calls is the
// only conclusive answer: an options-side format field staying 0 means "not declared", not "not
// tagged". Log only, no rewriting -- tags pass through untouched.

std::atomic<void*> g_real_set_tag_for_frame{nullptr};
std::atomic<void*> g_real_set_tag{nullptr};

void RecordTags(const sl::ResourceTag* tags, uint32_t count, const char* entry_point) {
  if (tags == nullptr || count == 0u) return;
  uint32_t mask = 0u;
  for (uint32_t index = 0u; index < count; ++index) {
    if (tags[index].resource == nullptr) continue;
    if (tags[index].type == sl::kBufferTypeHUDLessColor) {
      mask |= 1u;
    } else if (tags[index].type == sl::kBufferTypeUIColorAndAlpha) {
      mask |= 2u;
    }
  }
  if (mask == 0u) return;
  const uint32_t added = mask & ~g_tag_mask.fetch_or(mask, std::memory_order_relaxed);
  if ((added & 1u) != 0u) {
    nr::Logf(nr::LogLevel::INFO,
             "sl-tags: the game tags kBufferTypeHUDLessColor via {}; UI recomposition has its HUD-less source here",
             entry_point);
  }
  if ((added & 2u) != 0u) {
    nr::Logf(nr::LogLevel::INFO,
             "sl-tags: the game tags kBufferTypeUIColorAndAlpha via {}; fullscreen menu detection has its mask here, and UI recomposition its UI layer",
             entry_point);
  }
}

sl::Result HookedSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                                uint32_t count, sl::CommandBuffer* cmd_buffer) {
  const auto real = reinterpret_cast<SetTagForFrameFn*>(g_real_set_tag_for_frame.load(std::memory_order_acquire));
  if (real == nullptr) return sl::Result::eErrorInvalidParameter;
  const sl::Result result = real(frame, viewport, tags, count, cmd_buffer);
  if (result == sl::Result::eOk) RecordTags(tags, count, "slSetTagForFrame");
  return result;
}

sl::Result HookedSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t count,
                        sl::CommandBuffer* cmd_buffer) {
  const auto real = reinterpret_cast<SetTagFn*>(g_real_set_tag.load(std::memory_order_acquire));
  if (real == nullptr) return sl::Result::eErrorInvalidParameter;
  const sl::Result result = real(viewport, tags, count, cmd_buffer);
  if (result == sl::Result::eOk) RecordTags(tags, count, "slSetTag");
  return result;
}

void InstallTagProbe(HMODULE interposer) {
  static std::atomic<bool> attempted{false};
  if (attempted.exchange(true, std::memory_order_acq_rel)) return;
  struct Entry {
    const char* name;
    void* detour;
    std::atomic<void*>* real;
  };
  const Entry entries[] = {
      {"slSetTagForFrame", reinterpret_cast<void*>(&HookedSetTagForFrame), &g_real_set_tag_for_frame},
      {"slSetTag", reinterpret_cast<void*>(&HookedSetTag), &g_real_set_tag},
  };
  for (const Entry& entry : entries) {
    const auto target = reinterpret_cast<void*>(GetProcAddress(interposer, entry.name));
    if (target == nullptr) continue;
    void* original = nullptr;
    if (MH_CreateHook(target, entry.detour, &original) != MH_OK || MH_EnableHook(target) != MH_OK) {
      nr::Logf(nr::LogLevel::WARN, "sl-tags: could not hook {}; the UI buffer tags stay unknown in this game", entry.name);
      continue;
    }
    entry.real->store(original, std::memory_order_release);
    nr::Logf(nr::LogLevel::INFO, "sl-tags: watching {} for the HUD-less and UI colour-and-alpha tags", entry.name);
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

void SetOptionOverrides(FgOptionOverride ui_recomposition, FgOptionOverride menu_detection) {
  const auto normalize = [](FgOptionOverride value) -> uint32_t {
    return static_cast<uint32_t>(value) <= 2u ? static_cast<uint32_t>(value) : 0u;
  };
  const uint32_t ui = normalize(ui_recomposition);
  const uint32_t menu = normalize(menu_detection);
  const uint32_t previous_ui = g_ui_recomposition.exchange(ui, std::memory_order_relaxed);
  const uint32_t previous_menu = g_menu_detection.exchange(menu, std::memory_order_relaxed);
  // Re-selecting the same pair is not a change: no replay, no log.
  if (previous_ui == ui && previous_menu == menu) return;
  const uint32_t tags = g_tag_mask.load(std::memory_order_relaxed);
  nr::Logf(nr::LogLevel::INFO,
           "sl-fg: DLSS-G options from the panel: UI recomposition {}, fullscreen menu detection {} (submitted at the next frame-generation call; the game's tags so far: HUD-less {}, UI colour {})",
           OptionOverrideName(ui), OptionOverrideName(menu),
           (tags & 1u) != 0u ? "yes" : "not seen", (tags & 2u) != 0u ? "yes" : "not seen");
  g_replay_pending.store(true, std::memory_order_release);
}

void SetAdvertisedMaxGenerated(uint32_t generated_frames) {
  g_advertised_max.store(generated_frames, std::memory_order_relaxed);
}

uint32_t GeneratedFramesCeiling() {
  return g_provider_max_generated.load(std::memory_order_relaxed);
}

void NotifyDynamicD3D12(bool d3d12) {
  g_dynamic_d3d12.store(d3d12, std::memory_order_relaxed);
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
