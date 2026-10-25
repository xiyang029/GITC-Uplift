#include "addon/sl_fg_hooks.hpp"

#include <MinHook.h>

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

// ---- The Streamline ABI this file speaks (sl.h / sl_dlss_g.h, SDK 2.x) ----
// The layouts are copied field-for-field from the SDK headers so the game's structures can be
// read and rebuilt byte-compatibly. Every unknown version is refused (forwarded unchanged),
// never guessed at.

enum class SlResult : uint32_t {
  eOk = 0,
  eErrorDeviceNotCreated = 5,
  eErrorNGXFailed = 15,
  eErrorNotInitialized = 21,
  eErrorInitNotCalled = 23,
  eErrorInvalidParameter = 25,
  eErrorUnsupportedInterface = 30,
  eErrorFeatureNotSupported = 32,
  eErrorFeatureManagerInvalidState = 37,
  eErrorInvalidState = 38,
};

constexpr uint32_t kFeatureDLSS_G = 1000u;

enum class SlDlssgMode : uint32_t {
  eOff = 0,
  eOn = 1,
  eAuto = 2,
  eDynamic = 3,
};

enum class SlBoolean : char {
  eFalse = 0,
  eTrue = 1,
  eInvalid = 2,
};

// sl::BaseStructure: a chained pointer, the structure's GUID, and its version.
struct SlGuid {
  uint32_t data1;
  uint16_t data2;
  uint16_t data3;
  uint8_t data4[8];
};
static_assert(sizeof(SlGuid) == 16);

struct SlBaseStructure {
  void* next;
  SlGuid struct_type;
  size_t struct_version;
};
static_assert(sizeof(SlBaseStructure) == 32);

// sl::DLSSGState's StructType GUID {CC8AC8E1-A179-44F5-97FA-E74112F9BC61}, for the addon-owned
// probe structures (sl_dlss_g.h).
constexpr SlGuid kDlssgStateGuid{0xcc8ac8e1, 0xa179, 0x44f5, {0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61}};

constexpr size_t kOptionsVersion1 = 1;
constexpr size_t kOptionsVersion2 = 2;
constexpr size_t kOptionsVersion3 = 3;
constexpr size_t kOptionsVersion4 = 4;
constexpr size_t kOptionsVersion5 = 5;

struct SlViewportHandle : SlBaseStructure {
  uint32_t value;
};
static_assert(sizeof(SlViewportHandle) == 40);

// sl::DLSSGOptions, kStructVersion5.
struct SlDlssgOptions : SlBaseStructure {
  SlDlssgMode mode;
  uint32_t num_frames_to_generate;  // generated frames: 2x -> 1, 3x -> 2, 4x -> 3
  uint32_t flags;
  uint32_t dynamic_res_width;
  uint32_t dynamic_res_height;
  uint32_t num_back_buffers;
  uint32_t mvec_depth_width;
  uint32_t mvec_depth_height;
  uint32_t color_width;
  uint32_t color_height;
  uint32_t color_buffer_format;
  uint32_t mvec_buffer_format;
  uint32_t depth_buffer_format;
  uint32_t hudless_buffer_format;
  uint32_t ui_buffer_format;
  void* on_error_callback;
  // kStructVersion2
  SlBoolean b_reserved15;
  // kStructVersion3
  uint32_t queue_parallelism_mode;
  // kStructVersion4
  SlBoolean enable_user_interface_recomposition;
  // kStructVersion5
  float dynamic_target_frame_rate;
};
static_assert(sizeof(SlDlssgOptions) == 120);

// sl::DLSSGState, kStructVersion4.
struct SlDlssgState : SlBaseStructure {
  uint64_t estimated_vram_usage_in_bytes;
  uint32_t status;
  uint32_t min_width_or_height;
  uint32_t num_frames_actually_presented;
  // kStructVersion2
  uint32_t num_frames_to_generate_max;
  SlBoolean b_reserved4;
  SlBoolean b_is_vsync_support_available;
  void* inputs_processing_completion_fence;
  uint64_t last_present_inputs_processing_completion_fence_value;
  // kStructVersion4
  SlBoolean b_is_dynamic_mfg_supported;
};
static_assert(sizeof(SlDlssgState) == 88);

using GetFeatureFunctionFn = SlResult(__cdecl*)(uint32_t feature, const char* function_name, void** function);
using SetOptionsFn = SlResult(__cdecl*)(const SlViewportHandle&, const SlDlssgOptions&);
using GetStateFn = SlResult(__cdecl*)(const SlViewportHandle&, SlDlssgState&, const SlDlssgOptions*);

// ---- The panel's choices ----

std::atomic<uint32_t> g_force_multiplier{0u};   // 0 = follow the game; 2..kMaxMultiplier = force
// The provider reports numFramesToGenerateMax 5 (6x) on this stack; the panel offers up to 6x.
constexpr uint32_t kMaxMultiplier = 6u;
std::atomic<bool> g_dynamic_enabled{false};
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

// ---- The release-stack allowlist (Dynamic MFG) ----
// NVIDIA's Dynamic MFG is only release-supported on Streamline 2.14.1/2.14.16 with DLSS-G
// 310.9.1/310.9.16 (MFGAdaUnlock's verified pairs); anything else keeps fixed MFG.

struct SlVersion {
  uint16_t major = 0, minor = 0, patch = 0, revision = 0;
  bool valid = false;
};

constexpr uint64_t Pack(const SlVersion& v) {
  return v.valid ? (uint64_t{v.major} << 48) | (uint64_t{v.minor} << 32) | (uint64_t{v.patch} << 16) | v.revision : 0;
}

constexpr bool IsAllowlisted(uint64_t packed, uint16_t major, uint16_t minor, uint16_t patch) {
  return packed != 0
         && packed == ((uint64_t{major} << 48) | (uint64_t{minor} << 32) | (uint64_t{patch} << 16));
}

constexpr bool IsDynamicDlssgVersion(uint64_t packed) {
  return IsAllowlisted(packed, 310, 9, 1) || IsAllowlisted(packed, 310, 9, 16);
}

constexpr bool IsDynamicStreamlineVersion(uint64_t packed) {
  return IsAllowlisted(packed, 2, 14, 1) || IsAllowlisted(packed, 2, 14, 16);
}

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

void ObserveStreamlineFunctionOwner(const void* function);

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
  const bool supported = IsDynamicStreamlineVersion(Pack(version));
  nr::Logf(nr::LogLevel::INFO, "the FG multiplier hooks observed Streamline {}.{}.{}.{} ({}; version readable, Dynamic support itself is gated on the provider's bIsDynamicMFGSupported)",
           version.major, version.minor, version.patch, version.revision,
           supported ? "Dynamic MFG release stack" : "outside the 2.14.1/2.14.16 release pairs");
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
  const bool supported = IsDynamicDlssgVersion(Pack(version));
  g_dlssg_version_seen.store(true, std::memory_order_release);
  nr::Logf(nr::LogLevel::INFO, "the FG multiplier hooks observed the DLSS-G provider {}.{}.{}.{} ({}; version readable, Dynamic support itself is gated on the provider's bIsDynamicMFGSupported)",
           version.major, version.minor, version.patch, version.revision,
           supported ? "release stack" : "outside the 310.9.1/310.9.16 release pairs");
}

bool DynamicVersionStackReady() {
  // The release allowlist (310.9.1/16 with 2.14.1/16) is logged for context, but the
  // authoritative gate is the provider's own bIsDynamicMFGSupported from GetState: this project
  // ships no kernel patches whose safety the exact versions would guard, and the OTA override
  // tree reports versions the fixed allowlist never anticipated. Both versions must merely be
  // readable, proving the modules were actually found.
  return g_streamline_version_seen.load(std::memory_order_acquire) && g_dlssg_version_seen.load(std::memory_order_acquire);
}

bool IsTransientDynamicFailure(SlResult result) {
  switch (result) {
    case SlResult::eErrorDeviceNotCreated:
    case SlResult::eErrorNGXFailed:
    case SlResult::eErrorNotInitialized:
    case SlResult::eErrorInitNotCalled:
    case SlResult::eErrorFeatureManagerInvalidState:
    case SlResult::eErrorInvalidState:
      return true;
    default:
      return false;
  }
}

// ---- The addon-owned options copy (hdr_compat.hpp's BuildAdvancedOptions) ----

bool BuildForwardedOptions(const SlDlssgOptions& source, SlDlssgOptions& destination,
                           uint32_t generated_frames, bool override_generated, bool enable_dynamic,
                           float dynamic_target_frame_rate) {
  const size_t version = source.struct_version;
  if (version < kOptionsVersion1 || version > kOptionsVersion5) return false;
  if (dynamic_target_frame_rate < 0.0f || dynamic_target_frame_rate > 1000.0f) return false;

  destination = SlDlssgOptions{};
  destination.next = source.next;
  // The interposer validates the structure GUID on entry: a zeroed struct_type is refused with
  // eErrorMissingInputParameter no matter what else the copy says. Always carry the game's own.
  destination.struct_type = source.struct_type;
  // Generating more than one frame needs the newer options fields (the OTA override provider
  // refuses a v2 request with eErrorMissingInputParameter for 3x/4x): promote to v5 whenever the
  // request is modified, with the added fields at their defaults. eDynamic needs v5 anyway.
  const bool modified = enable_dynamic || (override_generated && generated_frames > 0u);
  const size_t required_version = modified ? kOptionsVersion5 : kOptionsVersion1;
  destination.struct_version = version > required_version ? version : required_version;

  destination.mode = enable_dynamic && source.mode != SlDlssgMode::eOff ? SlDlssgMode::eDynamic : source.mode;
  destination.num_frames_to_generate = override_generated ? generated_frames : source.num_frames_to_generate;
  destination.flags = source.flags;
  destination.dynamic_res_width = source.dynamic_res_width;
  destination.dynamic_res_height = source.dynamic_res_height;
  destination.num_back_buffers = source.num_back_buffers;
  destination.mvec_depth_width = source.mvec_depth_width;
  destination.mvec_depth_height = source.mvec_depth_height;
  destination.color_width = source.color_width;
  destination.color_height = source.color_height;
  destination.color_buffer_format = source.color_buffer_format;
  destination.mvec_buffer_format = source.mvec_buffer_format;
  destination.depth_buffer_format = source.depth_buffer_format;
  destination.hudless_buffer_format = source.hudless_buffer_format;
  destination.ui_buffer_format = source.ui_buffer_format;
  destination.on_error_callback = source.on_error_callback;
  if (version >= kOptionsVersion2) destination.b_reserved15 = source.b_reserved15;
  if (version >= kOptionsVersion3) destination.queue_parallelism_mode = source.queue_parallelism_mode;
  destination.enable_user_interface_recomposition =
      version >= kOptionsVersion4 ? source.enable_user_interface_recomposition : SlBoolean::eFalse;
  if (version >= kOptionsVersion5) destination.dynamic_target_frame_rate = source.dynamic_target_frame_rate;
  if (enable_dynamic) destination.dynamic_target_frame_rate = dynamic_target_frame_rate;
  return true;
}

// ---- Diagnostics ----
// Every wrapper decision logs once per distinct shape: these lines are how a live test says WHY
// an override did or did not land (called at all? which ABI? which branch? what result?).

void LogSlResult(const char* what, SlResult result) {
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
SlViewportHandle g_replay_viewport{};
SlDlssgOptions g_replay_options{};
bool g_replay_valid = false;
std::atomic<bool> g_replay_pending{false};

void CacheReplaySubmission(const SlViewportHandle& viewport, const SlDlssgOptions& options) {
  if (options.mode == SlDlssgMode::eOff) return;
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

// Submits `options` (an addon-owned copy when generated_frames is overridden or dynamic is on),
// owning the Dynamic retry ladder. Fixed-multiplier retries and native fallback live in
// HookedSetOptions, so a rejected fixed override is never masked as a success here.
SlResult CallSetOptions(const SlViewportHandle& viewport, const SlDlssgOptions& options,
                        uint32_t generated_frames = 0u, bool override_generated = false) {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return SlResult::eErrorNotInitialized;

  const bool game_enabled = options.mode != SlDlssgMode::eOff;
  const bool dynamic = DynamicReady(game_enabled);

  if (!dynamic && !override_generated) return real(viewport, options);

  const float target = static_cast<float>(g_dynamic_target_fps.load(std::memory_order_relaxed));
  SlDlssgOptions forwarded{};
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

  const SlResult result = real(viewport, forwarded);
  if (dynamic) {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: dynamic submission (eDynamic, target {}) -> sl result {}", target == 0.0f ? "active-display refresh" : std::to_string(static_cast<unsigned>(target)) + " FPS",
             static_cast<uint32_t>(result));
  }
  if (result == SlResult::eOk) {
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
    SlDlssgOptions plain{};
    if (BuildForwardedOptions(options, plain, generated_frames, override_generated, false, 0.0f)) {
      const SlResult retry = real(viewport, plain);
      if (retry == SlResult::eOk) {
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
    return real(viewport, options);
  }

  return result;
}

SlResult __cdecl HookedSetOptions(const SlViewportHandle& viewport, const SlDlssgOptions& options) {
  const auto real = g_real_set_options.load(std::memory_order_acquire);
  if (real == nullptr) return SlResult::eErrorNotInitialized;

  const bool game_enabled = options.mode != SlDlssgMode::eOff;
  if (!game_enabled) {
    // A game-side off/on cycle is a safe retry boundary after a rejection.
    g_dynamic_declined.store(false, std::memory_order_release);
    g_dynamic_applied.store(false, std::memory_order_relaxed);
  }
  ObserveDlssgProviderVersion();

  const bool dynamic_ready = DynamicReady(game_enabled);
  const uint32_t multiplier = g_force_multiplier.load(std::memory_order_relaxed);
  const uint32_t requested = options.num_frames_to_generate;
  if (game_enabled) CacheReplaySubmission(viewport, options);
  nr::Logf(nr::LogLevel::INFO,
           "sl-fg: slDLSSGSetOptions(mode {}, v{}, numFramesToGenerate {} = {}x); panel: multiplier {}, dynamic {} -> {}",
           static_cast<uint32_t>(options.mode), options.struct_version, requested, requested + 1u, multiplier,
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
    const SlResult result = !game_enabled ? real(viewport, options) : CallSetOptions(viewport, options);
    LogSlResult(!game_enabled ? "forwarded the game's Off request unchanged" : "dynamic-priority passthrough", result);
    return result;
  }

  if (multiplier < 2u || multiplier > kMaxMultiplier) {
    const SlResult result = real(viewport, options);
    LogSlResult("no panel multiplier selected; forwarded unchanged", result);
    return result;
  }

  const uint32_t desired = multiplier - 1u;
  if (desired == requested) {
    const SlResult result = real(viewport, options);
    LogSlResult("the game's request already equals the panel's multiplier", result);
    return result;
  }

  if (options.struct_version < kOptionsVersion1 || options.struct_version > kOptionsVersion5) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
      nr::Logf(nr::LogLevel::WARN, "the FG multiplier override refuses an unknown DLSSGOptions ABI; forwarding the game's request unchanged");
    }
    const SlResult result = real(viewport, options);
    LogSlResult("unknown DLSSGOptions ABI; forwarded unchanged", result);
    return result;
  }

  SlResult result = CallSetOptions(viewport, options, desired, true);
  LogSlResult("override submission (attempt 1)", result);
  if (result != SlResult::eOk) {
    // eErrorFeatureManagerInvalidState can simply mean DLSS-G was not ready yet. Retry once
    // with the SAME overridden count before giving the count up as refused (MFGAdaUnlock's
    // lesson: dropping straight back would silently discard a working 4x).
    const SlResult retry = CallSetOptions(viewport, options, desired, true);
    LogSlResult("override submission (retry)", retry);
    if (retry == SlResult::eOk) {
      nr::Logf(nr::LogLevel::INFO, "slDLSSGSetOptions returned {} on the first attempt but accepted the override on retry; that first failure was feature-manager state",
               static_cast<uint32_t>(result));
      result = retry;
    } else {
      if (g_force_failed_for.exchange(desired, std::memory_order_relaxed) != desired) {
        nr::Logf(nr::LogLevel::WARN, "slDLSSGSetOptions refused numFramesToGenerate={} twice (results {} then {}); falling back to the game's own request of {}. The count itself is being refused",
                 desired, static_cast<uint32_t>(result), static_cast<uint32_t>(retry), requested);
      }
      // Never leave the player without FG: put the game's own request through.
      const SlResult fallback = real(viewport, options);
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

SlResult __cdecl HookedGetState(const SlViewportHandle& viewport, SlDlssgState& state, const SlDlssgOptions* options) {
  // Another addon or a reloaded wrapper can hand out a pointer that calls back into this
  // wrapper; re-entering would overflow the stack.
  thread_local bool call_active = false;
  if (call_active) return SlResult::eErrorInvalidState;

  const auto real = g_real_get_state.load(std::memory_order_acquire);
  if (real == nullptr) return SlResult::eErrorNotInitialized;

  const size_t caller_version = state.struct_version;
  SlResult result;
  SlDlssgState extended{};
  constexpr uint32_t kMaxDynamicProbeFailures = 8u;
  // Games older than the v4 state ABI cannot see bIsDynamicMFGSupported; query v4 into
  // addon-owned storage and copy back only what the caller's ABI guarantees.
  const bool probe_dynamic_state =
      caller_version >= kOptionsVersion1 && caller_version < kOptionsVersion4
      && g_dynamic_probe_failures.load(std::memory_order_relaxed) < kMaxDynamicProbeFailures;
  // Some games poll GetState every frame: log the first few calls only.
  const uint32_t logged = g_state_logs.fetch_add(1u, std::memory_order_relaxed);
  if (logged < 10u) {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: slDLSSGGetState(v{}), probing v4 for Dynamic support: {}", caller_version,
             probe_dynamic_state ? "yes" : "no");
  }
  const SlDlssgState* observed = &state;
  if (probe_dynamic_state) {
    // Query v4 into addon-owned storage: the header must name the v4 ABI (a zero-initialized
    // struct_version would query nothing and no dynamic flag would ever come back).
    extended.next = state.next;
    extended.struct_type = state.struct_type;
    extended.struct_version = kOptionsVersion4;
    call_active = true;
    result = real(viewport, extended, options);
    call_active = false;
    if (result == SlResult::eOk) {
      g_dynamic_probe_failures.store(0u, std::memory_order_relaxed);
      observed = &extended;
      state.estimated_vram_usage_in_bytes = extended.estimated_vram_usage_in_bytes;
      state.status = extended.status;
      state.min_width_or_height = extended.min_width_or_height;
      state.num_frames_actually_presented = extended.num_frames_actually_presented;
      if (caller_version >= kOptionsVersion2) {
        state.num_frames_to_generate_max = extended.num_frames_to_generate_max;
        state.b_reserved4 = extended.b_reserved4;
        state.b_is_vsync_support_available = extended.b_is_vsync_support_available;
        state.inputs_processing_completion_fence = extended.inputs_processing_completion_fence;
        state.last_present_inputs_processing_completion_fence_value = extended.last_present_inputs_processing_completion_fence_value;
      }
    } else {
      const bool structurally_unsupported = result == SlResult::eErrorInvalidParameter
                                            || result == SlResult::eErrorUnsupportedInterface
                                            || result == SlResult::eErrorFeatureNotSupported;
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
    const int dynamic_flag = observed->struct_version >= kOptionsVersion4 ? static_cast<int>(observed->b_is_dynamic_mfg_supported) : -1;
    nr::Logf(nr::LogLevel::INFO, "sl-fg: slDLSSGGetState -> result {}, max generated {}, dynamic flag {} (state v{})",
             static_cast<uint32_t>(result), observed->num_frames_to_generate_max, dynamic_flag, observed->struct_version);
  }
  if (result == SlResult::eOk) {
    if (observed->struct_version >= kOptionsVersion4
        && (observed->b_is_dynamic_mfg_supported == SlBoolean::eTrue || observed->b_is_dynamic_mfg_supported == SlBoolean::eFalse)) {
      const bool supported = observed->b_is_dynamic_mfg_supported == SlBoolean::eTrue;
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
  if (result == SlResult::eOk && (observed != &state || caller_version >= kOptionsVersion2)) {
    const uint32_t provider_max = observed->num_frames_to_generate_max;
    uint32_t known = g_provider_max_generated.load(std::memory_order_relaxed);
    while (known < provider_max
           && !g_provider_max_generated.compare_exchange_weak(known, provider_max, std::memory_order_relaxed)) {
    }
  }

  // Raise the ceiling for games that build their native 2x/3x/4x selector from
  // numFramesToGenerateMax (STALKER 2), never above what the runtime itself reports.
  if (result != SlResult::eOk || caller_version < kOptionsVersion2) return result;
  const uint32_t wanted = g_advertised_max.load(std::memory_order_relaxed);
  const uint32_t provider_max = g_provider_max_generated.load(std::memory_order_relaxed);
  const uint32_t advertised = (provider_max != 0u && wanted > provider_max) ? provider_max : wanted;
  if (advertised < 2u || state.num_frames_to_generate_max >= advertised) return result;
  state.num_frames_to_generate_max = advertised;
  return result;
}

SlResult __cdecl HookedGetFeatureFunction(uint32_t feature, const char* function_name, void** function) {
  const auto real = reinterpret_cast<GetFeatureFunctionFn>(g_real_get_feature_function.load(std::memory_order_acquire));
  if (real == nullptr || function == nullptr) return SlResult::eErrorInvalidParameter;
  const SlResult result = real(feature, function_name, function);
  // Log every DLSS-G / Reflex function the game asks for: if the wrapper never fires, these lines
  // show what names this integration actually queries (OptiScaler and other shims included).
  if (function_name != nullptr && (feature == kFeatureDLSS_G || strstr(function_name, "Reflex") != nullptr)) {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: slGetFeatureFunction(feature {}, '{}') -> {} (result {})", feature,
             function_name, *function != nullptr ? "a pointer" : "null", static_cast<uint32_t>(result));
  }
  if (result != SlResult::eOk || function_name == nullptr || *function == nullptr) return result;
  if (feature == kFeatureDLSS_G && strcmp(function_name, "slDLSSGSetOptions") == 0
      && *function != reinterpret_cast<void*>(&HookedSetOptions)) {
    ObserveStreamlineFunctionOwner(*function);
    g_real_set_options.store(reinterpret_cast<SetOptionsFn>(*function), std::memory_order_release);
    *function = reinterpret_cast<void*>(&HookedSetOptions);
    nr::Logf(nr::LogLevel::INFO, "wrapped slDLSSGSetOptions; the FG multiplier override is live");
  } else if (feature == kFeatureDLSS_G && strcmp(function_name, "slDLSSGGetState") == 0
             && *function != reinterpret_cast<void*>(&HookedGetState)) {
    ObserveStreamlineFunctionOwner(*function);
    g_real_get_state.store(reinterpret_cast<GetStateFn>(*function), std::memory_order_release);
    *function = reinterpret_cast<void*>(&HookedGetState);
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
  nr::Logf(nr::LogLevel::INFO, "the FG multiplier hooks hooked sl.interposer.dll's slGetFeatureFunction at {}", reinterpret_cast<void*>(target));
}

}  // namespace

void SetForceMultiplier(uint32_t multiplier) {
  const uint32_t clamped = multiplier >= 2u && multiplier <= kMaxMultiplier ? multiplier : 0u;
  const uint32_t previous = g_force_multiplier.exchange(clamped, std::memory_order_relaxed);
  g_replay_pending.store(true, std::memory_order_release);  // the Present poll re-submits it live
  if (previous != clamped) {
    // Both selecting a value and returning control to the game land on the game's next enabled
    // SetOptions call.
    g_force_failed_for.store(UINT32_MAX, std::memory_order_relaxed);
  }
}

void SetDynamicMfg(bool enabled, uint32_t target_fps) {
  const bool was = g_dynamic_enabled.exchange(enabled, std::memory_order_relaxed);
  g_dynamic_target_fps.store(target_fps > 1000u ? 0u : target_fps, std::memory_order_relaxed);
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
    for (;;) {
      const uint32_t current = g_install.load(std::memory_order_relaxed);
      if (current == 1u || current == 2u) return;
      if (GetModuleHandleW(L"sl.interposer.dll") != nullptr) break;
      Sleep(25);
    }
    InstallSlFgHooks();
    const uint32_t current = g_install.load(std::memory_order_relaxed);
    if (current == 3u) g_install.store(0u, std::memory_order_release);
  }).detach();
}

// Probes the provider's Dynamic-MFG support flag directly: many games call GetState once, before
// enabling FG, so the wrapper's in-transit probe alone never sees the flag. Bounded to a handful
// of attempts; called from the Present poll while Dynamic is wanted but unsupported-unseen.
void ProbeDynamicSupport(const SlViewportHandle& viewport) {
  static std::atomic<uint32_t> active_probes{0u};
  if (active_probes.load(std::memory_order_relaxed) >= 8u) return;
  const auto real = g_real_get_state.load(std::memory_order_acquire);
  if (real == nullptr) return;
  SlDlssgState probe{};
  probe.struct_type = kDlssgStateGuid;
  probe.struct_version = kOptionsVersion4;
  thread_local bool call_active = false;
  if (call_active) return;
  call_active = true;
  const SlResult result = real(viewport, probe, nullptr);
  call_active = false;
  active_probes.fetch_add(1u, std::memory_order_relaxed);
  const bool flag_valid = probe.struct_version >= kOptionsVersion4
                          && (probe.b_is_dynamic_mfg_supported == SlBoolean::eTrue
                              || probe.b_is_dynamic_mfg_supported == SlBoolean::eFalse);
  if (result == SlResult::eOk && flag_valid) {
    const bool supported = probe.b_is_dynamic_mfg_supported == SlBoolean::eTrue;
    g_dynamic_supported.store(supported, std::memory_order_release);
    g_dynamic_support_seen.store(true, std::memory_order_release);
    nr::Logf(nr::LogLevel::INFO, "sl-fg: active Dynamic support probe: {} (provider max generated {})",
             supported ? "supported" : "not supported", probe.num_frames_to_generate_max);
  } else {
    nr::Logf(nr::LogLevel::INFO, "sl-fg: active Dynamic support probe: result {}, state v{}, flag {}",
             static_cast<uint32_t>(result), probe.struct_version, static_cast<int>(probe.b_is_dynamic_mfg_supported));
  }
}

// Re-submits the cached enabled submission through the full wrapper logic so a panel change lands
// without the game's own next call. Called from the add-on's Present poll (the game's present
// thread). A refusal inside the wrapper fails safe exactly as a game-side call would.
void RefreshFromPanel() {
  if (!g_replay_pending.exchange(false, std::memory_order_acq_rel)) return;
  SlViewportHandle viewport{};
  SlDlssgOptions options{};
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
