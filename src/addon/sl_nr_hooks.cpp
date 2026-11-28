#include "addon/sl_nr_hooks.hpp"

#include <Windows.h>
#include <sl_core_api.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include "addon/sl_nr_viewports.hpp"

#include "nr/log.hpp"

namespace uplift::addon::slnr {
namespace {
// Verified against Naraka's sl.dlss_nr.dll, not a declaration of a public NR SDK.
// See docs/native-nr-streamline-abi.md for the option->NGX data flow and version sizes.
constexpr sl::StructType OPTIONS_TYPE{0x29dfdfe0, 0x273a, 0x4e72, {0xb4, 0x92, 0x2d, 0xc8, 0x23, 0xd5, 0xb1, 0xad}};
constexpr uint32_t PROVIDER_TIMESTAMP = 0x6ac513ccu;
constexpr uint32_t PROVIDER_IMAGE_SIZE = 0x63000u;
constexpr size_t SET_OPTIONS_RVA = 0x40180u;
constexpr size_t PRESET_OFFSET = 0x38u;
static_assert(sizeof(void*) == 8u && sizeof(sl::BaseStructure) == 0x20u && sizeof(sl::ViewportHandle) == 0x28u);
using SetOptions = sl::Result (*)(const sl::ViewportHandle&, const sl::BaseStructure&);

struct CachedOptions {
  alignas(8) std::array<unsigned char, 0x50u> bytes{};
  PresetSubmission submission;
  std::optional<uint32_t> waiting_for_game;
  bool replayable = false;
};
std::atomic<uint32_t> desired{0u};
std::recursive_mutex mutex;
SetOptions original = nullptr;
HMODULE provider = nullptr;
ViewportOptions<CachedOptions> viewports;
bool waiting_logged = false;

uint32_t Preset(const CachedOptions& options) {
  uint32_t preset = 0u;
  std::memcpy(&preset, options.bytes.data() + PRESET_OFFSET, sizeof(preset));
  return preset;
}

bool CopyOptions(const sl::BaseStructure& options, CachedOptions& copy) {
  if (options.structType != OPTIONS_TYPE || options.structVersion < 2u || options.structVersion > 4u) return false;
  const size_t size = options.structVersion == 4u ? 0x50u : 0x48u;
  std::memcpy(copy.bytes.data(), &options, size);
  return true;
}

sl::Result Submit(const sl::ViewportHandle& viewport, const sl::BaseStructure& game, CachedOptions& cache, bool replay) {
  const uint32_t wanted = desired.load(std::memory_order_acquire);
  const uint32_t game_preset = Preset(cache);
  // nvngx_dlssnr.dll clears bit 31 before selecting the network (RVA 0x17d1a).
  // Preserve that game flag while replacing only the actual network ID.
  const uint32_t effective = wanted == 0u || (cache.submission.accepted == wanted && cache.submission.refused == wanted) ? game_preset : (game_preset & 0x80000000u) | wanted;
  const bool changed = cache.submission.accepted != wanted || cache.submission.effective != effective;
  alignas(8) auto copy = cache.bytes;
  std::memcpy(copy.data() + PRESET_OFFSET, &effective, sizeof(effective));
  auto& options = *reinterpret_cast<sl::BaseStructure*>(copy.data());
  // Only a live call may retain the caller's extension chain. A replay never keeps borrowed pointers.
  options.next = replay ? nullptr : game.next;
  const auto submit = original;
  sl::Result result = submit(viewport, options);
  // Reentrant free/shutdown can detach this snapshot while the provider runs. Do not restore an evicted viewport.
  const auto still_current = [&] { return original == submit && viewports.Find(viewport).get() == &cache; };
  if (!still_current()) return result;
  const bool restore = result != sl::Result::eOk && wanted != 0u && effective != game_preset;
  if (restore) {
    nr::Logf(nr::LogLevel::WARN, "Native NR preset: Streamline rejected viewport {} preset {:#010x}, result {}; restoring game {:#010x}",
             static_cast<uint32_t>(viewport), effective, static_cast<uint32_t>(result), game_preset);
    result = submit(viewport, game);
    if (!still_current()) return result;
  } else if (changed || replay) {
    nr::Logf(nr::LogLevel::INFO,
             "Native NR preset: Streamline options {} viewport {} version {} game {:#010x} -> submitted {:#010x}, panel {}, result {}; provider recreates on its next evaluate if preset changed",
             replay ? "replay" : "game call", static_cast<uint32_t>(viewport), game.structVersion,
             game_preset, effective, wanted, static_cast<uint32_t>(result));
  }
  cache.submission.Complete(wanted, restore ? game_preset : effective, restore, result, PresetSubmission::Clock::now());
  return result;
}

sl::Result HookedSetOptions(const sl::ViewportHandle& viewport, const sl::BaseStructure& options) {
  const std::lock_guard lock(mutex);
  if (!original) return sl::Result::eErrorInvalidParameter;
  CachedOptions copy;
  if (!CopyOptions(options, copy) || !IsViewport(viewport)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      nr::Logf(nr::LogLevel::WARN, "Native NR preset: unsupported options ABI version {}; game call unchanged", options.structVersion);
    }
    return original(viewport, options);
  }
  const auto saved = viewports.GetOrInsert(viewport);
  if (!saved) return original(viewport, options);
  auto& cache = *saved;
  copy.submission = cache.submission;
  copy.replayable = options.next == nullptr && viewport.next == nullptr;
  cache = copy;
  return Submit(viewport, options, cache, false);
}

bool VerifiedProvider(void* function, HMODULE& owner) {
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(function), &owner)) return false;
  const auto* base = reinterpret_cast<const unsigned char*>(owner);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == PROVIDER_TIMESTAMP
      && nt->OptionalHeader.SizeOfImage == PROVIDER_IMAGE_SIZE
      && static_cast<const void*>(base + SET_OPTIONS_RVA) == function;
}
}  // namespace

void SetDesiredPreset(uint32_t preset) {
  const uint32_t value = preset == 1u || preset == 2u ? preset : 0u;
  if (desired.exchange(value, std::memory_order_acq_rel) != value) {
    nr::Logf(nr::LogLevel::INFO, "Native NR preset: panel selection {} (0 game, 1 Legacy, 2 Small FP8); pending Streamline submission", value);
  }
}

void ObserveFunction(const char* name, void*& function) {
  if (!name || std::strcmp(name, "slDLSSNRSetOptions") != 0 || !function
      || function == reinterpret_cast<void*>(&HookedSetOptions)) return;
  const std::lock_guard lock(mutex);
  HMODULE owner = nullptr;
  if (!VerifiedProvider(function, owner)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      nr::Log(nr::LogLevel::WARN, "Native NR preset: unverified Streamline provider; options forwarded unchanged");
    }
    return;
  }
  if (provider != owner || original != reinterpret_cast<SetOptions>(function)) {
    viewports.Clear();
    waiting_logged = false;
    provider = owner;
    original = reinterpret_cast<SetOptions>(function);
    nr::Log(nr::LogLevel::INFO, "Native NR preset: wrapped verified slDLSSNRSetOptions (options v2-v4, preset offset 0x38)");
  }
  function = reinterpret_cast<void*>(&HookedSetOptions);
}

void BeforeEvaluate(const sl::BaseStructure* const* inputs, uint32_t count) {
  const auto* const viewport = FindEvaluateViewport(inputs, count);
  if (!viewport) return;
  const uint32_t id = *viewport;
  const std::lock_guard lock(mutex);
  const uint32_t wanted = desired.load(std::memory_order_acquire);
  const auto cache = viewports.Find(id);
  if (!original || !provider || !cache) {
    if (wanted != 0u && !waiting_logged) {
      waiting_logged = true;
      nr::Log(nr::LogLevel::INFO, "Native NR preset: waiting for verified game slDLSSNRSetOptions; no cached options to replay");
    }
    return;
  }
  if (!cache->submission.ShouldReplay(wanted, PresetSubmission::Clock::now())) return;
  if (!cache->replayable) {
    if (cache->waiting_for_game != wanted) {
      nr::Logf(nr::LogLevel::WARN, "Native NR preset: viewport {} has a borrowed extension chain; waiting for its next game options call", id);
      cache->waiting_for_game = wanted;
    }
    return;
  }
  if (GetModuleHandleW(L"sl.dlss_nr.dll") != provider) return;
  alignas(8) auto game_bytes = cache->bytes;
  auto& game = *reinterpret_cast<sl::BaseStructure*>(game_bytes.data());
  game.next = nullptr;
  Submit(sl::ViewportHandle(id), game, *cache, true);
}

void AfterFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport, sl::Result result) {
  if (!FreedNrViewport(feature, viewport, result)) return;
  const std::lock_guard lock(mutex);
  viewports.Erase(viewport);
}

void BeforeShutdown() {
  const std::lock_guard lock(mutex);
  viewports.Clear();
  original = nullptr;
  provider = nullptr;
  waiting_logged = false;
}

}  // namespace uplift::addon::slnr
