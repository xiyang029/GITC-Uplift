#include "addon/fg_preset.hpp"

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <intrin.h>
#include <thread>
#include <vector>

#include <psapi.h>

#include "ngx_hooks/minhook_once.hpp"
#include "nr/log.hpp"

namespace uplift::addon {
namespace {

// ---- The NVAPI ABI of one driver setting (public NVAPI: NVDRS_SETTING_V1, pack(4)) ----

#pragma pack(push, 4)
union SettingValue {
  uint32_t dword;
  struct {
    uint32_t length;
    uint8_t data[4096];
  } binary;
  uint16_t text[2048];
  uint64_t qword;
};
struct DrsSetting {
  uint32_t version;
  uint16_t name[2048];
  uint32_t id;
  uint32_t type;
  uint32_t location;
  uint32_t current_predefined;
  uint32_t predefined_valid;
  SettingValue predefined;
  SettingValue current;
};
#pragma pack(pop)
static_assert(sizeof(DrsSetting) == 12320);
constexpr uint32_t SETTING_VERSION = sizeof(DrsSetting) | (1u << 16u);

// The DLSS-FG preset setting, and the values the provider understands (MFG-Unlocker's findings).
constexpr uint32_t FG_PRESET_ID = 0x10E41DF1u;
constexpr int32_t NVAPI_OK = 0;
constexpr int32_t NVAPI_SETTING_NOT_FOUND = -160;

// ---- The filter: rewrite the FG preset id's answer after the driver's ----

std::atomic<uint32_t> g_desired{0};  // the panel's choice, 0 = follow
// The provider's cached configuration slot and the choice the cached reader last served (see the
// configuration-slot block below).
std::atomic<void**> g_config_slot{nullptr};
std::atomic<uint32_t> g_last_served{0};
void** FindConfigSlot(HMODULE module, uintptr_t helper0, uintptr_t helper1);

// True when this read is one the rewrite can own: the FG preset id, a setting block of the version
// this process's NVAPI produces, and either the driver's answer or its setting-not-found. Everything
// else (other ids, other versions, driver errors) passes through untouched.
// The driver's own answer, logged once: the evidence that the FG provider's read actually crosses one
// of the hooked paths, and what the driver said before any override.
bool CompatibleRead(uint32_t id, int32_t result, const DrsSetting& setting) {
  return id == FG_PRESET_ID && setting.version == SETTING_VERSION
         && (result == NVAPI_SETTING_NOT_FOUND
             || (result == NVAPI_OK && setting.id == FG_PRESET_ID && setting.type == 0u));
}

// Writes `preset` into `setting` as a current-profile DWORD value; a not-found answer is fabricated
// into a well-formed setting first (the provider treats a missing setting as "no override").
void OverrideRead(DrsSetting& setting, uint32_t preset) {
  if (setting.version != SETTING_VERSION) {
    setting = {};
    setting.version = SETTING_VERSION;
    setting.id = FG_PRESET_ID;
    constexpr char name[] = "Override DLSS-FG preset";
    for (size_t i = 0; i < sizeof(name); ++i) {
      setting.name[i] = static_cast<uint16_t>(name[i]);
    }
  }
  setting.location = 0;  // NVDRS_CURRENT_PROFILE, for this returned value only
  setting.current_predefined = 0;
  std::memset(&setting.current, 0, sizeof(setting.current));
  setting.current.dword = preset;
}

// ---- The provider's cached preset reader (the live switch) ----
// The provider reads the driver setting once and caches it; the panel's DRS rewrite therefore only
// reaches a fresh process. The cache is served by a small function -- bool(uint32_t id, uint32_t*
// value) -- found by scanning the provider's code for its call sites: `mov ecx,<FG preset id>; call
// <reader>; test al,al; jz`, with the reader's own prologue and RUNTIME_FUNCTION entry validated
// (the discovery is MFG-Unlocker's, MIT). Hooking the reader serves the panel's choice to the
// provider directly, so the next FG create -- a frame-generation toggle in game -- builds the new
// model without a restart. Its two known call sites (the configuration read at create and the
// indicator's report) never run mid-evaluation, so no quiescence gating is needed on 310.9.x.

std::atomic<void*> g_reader_original{nullptr};
std::atomic<void*> g_reader_target{nullptr};   // the hooked reader's real address (trampoline kept apart)
std::atomic<uint64_t> g_last_scan{0};     // steady ms of the last scan, while hunting
std::atomic<HMODULE> g_hooked_modules[4] = {};  // providers whose reader (and maybe slot) is handled

bool __cdecl CachedReaderDetour(uint32_t id, uint32_t* value) {
  const auto original = reinterpret_cast<bool (__cdecl*)(uint32_t, uint32_t*)>(g_reader_original.load(std::memory_order_acquire));
  if (original == nullptr) return false;
  const bool result = original(id, value);
  const uint32_t desired = g_desired.load(std::memory_order_relaxed);
  if (result && id == FG_PRESET_ID && desired >= 1u && desired <= 3u && value != nullptr) {
    *value = desired;
    g_last_served.store(desired, std::memory_order_relaxed);
  }
  return result;
}

// Scans the module's executable sections for the reader's call sites and hooks the one unique
// reader. False on any ambiguity (a reader that is not provably single must not be hooked).
bool FindAndHookCachedReader(HMODULE module) {
  // The hunt's goal is the configuration slot; a provider whose reader is hooked but whose slot was
  // not found must not stop it (return false keeps the enumeration going).
  const bool slot_known = (g_config_slot.load(std::memory_order_acquire) != nullptr);
  for (const auto& hooked : g_hooked_modules) {
    if (hooked.load(std::memory_order_acquire) == module) return slot_known;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
      || !nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96) return false;
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  if (image_size < 4096u || image_size > 1024u * 1024u * 1024u) return false;
  // mov ecx,0x10E41DF1; call rel32 -- test al,al; jz rel32
  constexpr uint8_t PREFIX[]{0xB9, 0xF1, 0x1D, 0xE4, 0x10, 0xE8};
  constexpr uint8_t SUFFIX[]{0x84, 0xC0, 0x0F, 0x84};
  constexpr uint8_t PROLOGUE[]{0x48, 0x89, 0x5C, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x57,
                               0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x8B, 0xD9};
  void* selected = nullptr;
  uintptr_t helpers[2] = {};
  unsigned helper_count = 0u;
  const auto enclosing_begin = [&](const uint8_t* address) -> uintptr_t {
    // The call site's enclosing function, through any unwind-chain links (MFG-Unlocker's Enclosing,
    // without the depth guard's full validation).
    DWORD64 owner = 0;
    auto* entry = RtlLookupFunctionEntry(reinterpret_cast<uintptr_t>(address), &owner, nullptr);
    for (unsigned depth = 0u; depth < 16u && entry != nullptr && owner == base; ++depth) {
      if (entry->BeginAddress == 0u || entry->EndAddress <= entry->BeginAddress || entry->EndAddress > image_size) return 0u;
      uint8_t unwind[4] = {};
      memcpy(unwind, reinterpret_cast<const void*>(base + entry->UnwindData), sizeof(unwind));
      if (((unwind[0] >> 3) & UNW_FLAG_CHAININFO) == 0u) return base + entry->BeginAddress;
      const size_t codes = (size_t(unwind[2]) + 1u) & ~size_t(1u);
      entry = reinterpret_cast<PRUNTIME_FUNCTION>(base + entry->UnwindData + 4u + codes * 2u);
    }
    return 0u;
  };
  for (unsigned i = 0u; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& section = sections[i];
    if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.VirtualAddress >= image_size) continue;
    const size_t length = std::min<size_t>(section.Misc.VirtualSize, image_size - section.VirtualAddress);
    const auto* start = reinterpret_cast<const uint8_t*>(base + section.VirtualAddress);
    for (size_t offset = 0u; offset + 18u <= length; ++offset) {
      const auto* call = start + offset;
      if (memcmp(call, PREFIX, sizeof(PREFIX)) != 0 || memcmp(call + 10, SUFFIX, sizeof(SUFFIX)) != 0) continue;
      {
        const uintptr_t helper = enclosing_begin(call);
        if (helper == 0u) return false;
        if (helper_count == 0u) {
          helpers[0] = helper;
          helper_count = 1u;
        } else if (helpers[0] != helper) {
          if (helper_count == 2u && helpers[1] != helper) return false;
          helpers[1] = helper;
          helper_count = 2u;
        }
      }
      int32_t call_offset = 0, branch_offset = 0;
      memcpy(&call_offset, call + 6, 4);
      memcpy(&branch_offset, call + 14, 4);
      const uintptr_t target = reinterpret_cast<uintptr_t>(call + 10) + call_offset;
      const uintptr_t branch = reinterpret_cast<uintptr_t>(call + 18) + branch_offset;
      const auto in_image = [&](uintptr_t value, size_t size) {
        return value >= base && value - base < image_size && size <= image_size - (value - base);
      };
      if (!in_image(target, sizeof(PROLOGUE)) || !in_image(branch, 1u)
          || memcmp(reinterpret_cast<const void*>(target), PROLOGUE, sizeof(PROLOGUE)) != 0) return false;
      DWORD64 owner = 0;
      const auto* function = RtlLookupFunctionEntry(target, &owner, nullptr);
      if (function == nullptr || owner != base
          || reinterpret_cast<uintptr_t>(function) < base
          || reinterpret_cast<uintptr_t>(function) - base >= image_size
          || base + function->BeginAddress != target
          || function->EndAddress <= function->BeginAddress || function->EndAddress > image_size) return false;
      if (selected != nullptr && selected != reinterpret_cast<void*>(target)) return false;
      selected = reinterpret_cast<void*>(target);
    }
  }
  if (selected == nullptr || helper_count == 0u) return false;
  // The reader hook is per provider; a module whose reader is already handled only needs its slot
  // discovery. The trampoline lives apart from the real address: pattern targets use the real one.
  void* original = nullptr;
  if (g_reader_target.load(std::memory_order_acquire) != selected) {
    if (MH_CreateHook(selected, reinterpret_cast<void*>(&CachedReaderDetour), &original) != MH_OK) return false;
    g_reader_original.store(original, std::memory_order_release);
    g_reader_target.store(selected, std::memory_order_release);
    if (MH_EnableHook(selected) != MH_OK) {
      g_reader_original.store(nullptr, std::memory_order_release);
      g_reader_target.store(nullptr, std::memory_order_release);
      return false;
    }
  }
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(module), &pinned);
  for (auto& hooked : g_hooked_modules) {
    HMODULE expected = nullptr;
    if (hooked.compare_exchange_strong(expected, module, std::memory_order_acq_rel)) break;
  }
  void** const slot = (helper_count == 2u ? FindConfigSlot(module, helpers[0], helpers[1]) : nullptr);
  if (slot != nullptr) {
    g_config_slot.store(slot, std::memory_order_release);
    nr::Logf(nr::LogLevel::INFO, "the FG preset override found the provider's configuration slot at {} (live switching on)", reinterpret_cast<void*>(slot));
  } else {
    nr::Logf(nr::LogLevel::INFO, "the FG preset override found no configuration slot in this provider (helper count {}, helpers {:#x} and {:#x}); "
                                 "other NGX modules may still have one",
             helper_count, helpers[0], helpers[1]);
    nr::Logf(nr::LogLevel::INFO, "the FG preset override hooked the provider's cached preset reader at {} (live switching on)", selected);
    return false;  // the reader is hooked but this provider holds no slot: keep hunting others
  }
  nr::Logf(nr::LogLevel::INFO, "the FG preset override hooked the provider's cached preset reader at {} (live switching on)", selected);
  return true;
}

// The provider module, captured from a DRS read's return address. Hunted until the configuration
// slot is found: the reader hook per provider, then every other NGX-snippet module as fallback (the
// DRS read may come from a host rather than the provider, whose file name matches nothing).
void AdoptProviderFrom(const void* caller) {
  // A scan walks whole images: at most one per second while hunting.
  const uint64_t now = GetTickCount64();
  const uint64_t last = g_last_scan.load(std::memory_order_relaxed);
  if (now - last < 500u) return;
  g_last_scan.store(now, std::memory_order_relaxed);

  HMODULE module = nullptr;
  if (caller != nullptr
      && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(caller), &module)
             != FALSE
      && module != nullptr && FindAndHookCachedReader(module)) {
    return;
  }
  std::vector<HMODULE> loaded(256u);
  while (true) {
    DWORD needed = 0u;
    if (K32EnumProcessModules(GetCurrentProcess(), loaded.data(), static_cast<DWORD>(loaded.size() * sizeof(HMODULE)), &needed) == FALSE) {
      return;
    }
    const size_t count = needed / sizeof(HMODULE);
    if (count <= loaded.size()) {
      loaded.resize(count);
      break;
    }
    loaded.resize(count);
  }
  for (const HMODULE candidate : loaded) {
    if (candidate == nullptr || candidate == module) continue;
    if (GetProcAddress(candidate, "NVSDK_NGX_D3D12_CreateFeature") == nullptr
        && GetProcAddress(candidate, "NVSDK_NGX_VULKAN_CreateFeature") == nullptr) continue;
    if (FindAndHookCachedReader(candidate)) return;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    nr::Logf(nr::LogLevel::WARN, "the FG preset override found no configuration slot in any loaded provider; "
                                 "the panel's FG preset needs a restart there");
  }
}

// ---- The provider's configuration slot (the reset that makes a change stick without a restart) ----
// The model is selected from a cached EndpointConfiguration object -- a make_shared block behind a
// two-word slot (object pointer, control-block pointer) -- built once, at the provider's first
// create, from the driver setting. Hooking the cached reader alone serves the panel's choice to the
// indicator while the built model stays. The slot is discovered structurally: exactly one call site
// loads it and calls the reader (`mov rcx,[slot]; call <reader>`). Resetting (CAS the object word to
// null while no FG feature lives) makes the next create publish a fresh configuration, which reads
// the preset through the hooked reader -- the panel's choice (the discovery and reset are
// MFG-Unlocker's; the validation here is the lighter runtime kind).

// The configuration call site's shape: `mov rcx,[rip+disp]; call <reader>` -- exactly one in the
// image, and its memory operand is the slot. (MFG-Unlocker additionally validates the create
// function's guard/publish/apply instructions and the control block's RTTI; here the slot is
// validated at reset time instead, from its live contents.)
void** FindConfigSlot(HMODULE module, uintptr_t helper0, uintptr_t helper1) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  const size_t image_size = nt->OptionalHeader.SizeOfImage;
  // The configuration call site loads the singleton slot and calls one of the reader's two helpers:
  // `mov rcx,[rip+disp]; call <helper>`. Exactly one such site; its memory operand is the slot.
  void** slot = nullptr;
  for (unsigned i = 0u; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& section = sections[i];
    if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.VirtualAddress >= image_size) continue;
    const size_t length = std::min<size_t>(section.Misc.VirtualSize, image_size - section.VirtualAddress);
    const auto* start = reinterpret_cast<const uint8_t*>(base + section.VirtualAddress);
    for (size_t offset = 0u; offset + 12u <= length; ++offset) {
      const auto* p = start + offset;
      if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x0D || p[7] != 0xE8) continue;
      int32_t slot_disp = 0, call_disp = 0;
      memcpy(&slot_disp, p + 3, 4);
      memcpy(&call_disp, p + 8, 4);
      const uintptr_t target = reinterpret_cast<uintptr_t>(p + 12) + call_disp;  // e8 at p+7: +5 for the call itself
      if (target != helper0 && target != helper1) continue;
      auto* candidate = reinterpret_cast<void**>(const_cast<uint8_t*>(p) + 7 + slot_disp);
      if ((reinterpret_cast<uintptr_t>(candidate) & 7u) != 0u) return nullptr;
      if (slot != nullptr && slot != candidate) return nullptr;  // ambiguous: refuse
      slot = candidate;
    }
  }
  return slot;
}

// The shared detour body for both entry points: the original ran already; `id` and `result` are its
// inputs and result. Returns the result the caller sees.
int32_t FilterRead(const void* caller, uint32_t id, int32_t result, DrsSetting* setting) {
  if (id == FG_PRESET_ID) {
    // The provider reads this id once and caches it: this call is the one chance to find its module
    // (from the read's return address) and hook the cached reader behind it.
    AdoptProviderFrom(caller);
  }
  const uint32_t desired = g_desired.load(std::memory_order_relaxed);
  if (desired == 0u || desired > 3u || setting == nullptr) return result;
  if (!CompatibleRead(id, result, *setting)) return result;
  OverrideRead(*setting, desired);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    nr::Logf(nr::LogLevel::INFO, "the FG preset override rewrote the driver's answer ({}); it lands at the game's next DLSS-FG create",
             (desired == 1u ? "A" : desired == 2u ? "B" : "C"));
  }
  return NVAPI_OK;
}

// ---- The two hooked entry points ----

using GetSettingFn = int32_t(__cdecl*)(void* session, void* profile, uint32_t unknown, DrsSetting* setting);
using PublicGetSettingFn = int32_t(__cdecl*)(void* session, void* profile, DrsSetting* setting);

std::atomic<GetSettingFn> g_internal_original{nullptr};
std::atomic<PublicGetSettingFn> g_public_original{nullptr};

int32_t __cdecl InternalGetSettingDetour(void* session, void* profile, uint32_t id, DrsSetting* setting) {
  const GetSettingFn original = g_internal_original.load(std::memory_order_acquire);
  if (original == nullptr) return NVAPI_SETTING_NOT_FOUND;
  const int32_t result = original(session, profile, id, setting);
  return FilterRead(_ReturnAddress(), id, result, setting);
}

int32_t __cdecl PublicGetSettingDetour(void* session, void* profile, DrsSetting* setting) {
  const PublicGetSettingFn original = g_public_original.load(std::memory_order_acquire);
  if (original == nullptr) return NVAPI_SETTING_NOT_FOUND;
  // The caller names the setting it wants in `setting->id` before the call, and the driver leaves it
  // there on a not-found, so the id is read before the original can be trusted to keep it.
  const uint32_t id = (setting != nullptr ? setting->id : 0u);
  const int32_t result = original(session, profile, setting);
  return FilterRead(_ReturnAddress(), id, result, setting);
}

// ---- Installation state ----

std::atomic<uint32_t> g_install{0};  // 0 = not tried, 1 = hooked, 2 = failed (nvapi64 absent counts as retryable)

}  // namespace

void SetFgPresetDesired(uint32_t preset) {
  g_desired.store(preset, std::memory_order_relaxed);
}

void InstallFgPresetHook();

void EnsureFgPresetHook() {
  uint32_t state = g_install.load(std::memory_order_relaxed);
  if (state != 0u) return;  // hooked, failed, or the watchdog owns the install
  if (!g_install.compare_exchange_strong(state, 4u, std::memory_order_acq_rel)) return;  // 4 = watchdog started
  // The FG provider reads its preset once, during engine init -- possibly before the first present,
  // and always before the add-on's entry-point polls. A short watchdog installs the hook the moment
  // nvapi64.dll appears, so no NvAPI call can precede it. 25 ms steps, exits on install or failure.
  std::thread([] {
    for (;;) {
      uint32_t current = g_install.load(std::memory_order_relaxed);
      if (current == 1u || current == 2u) return;  // hooked or failed for good
      if (GetModuleHandleW(L"nvapi64.dll") != nullptr) break;
      Sleep(25);
    }
    InstallFgPresetHook();
  }).detach();
}

void InstallFgPresetHook() {
  uint32_t state = g_install.load(std::memory_order_relaxed);
  if (state == 1u || state == 2u) return;
  if (!g_install.compare_exchange_strong(state, 3u, std::memory_order_acq_rel)) return;  // 3 = install in progress

  const HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
  if (nvapi == nullptr) {
    g_install.store(0u, std::memory_order_release);  // not loaded yet: keep polling
    return;
  }
  // The driver's NVAPI lives for the process, but pin it like the NGX cores all the same.
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(nvapi), &pinned);

  if (const MH_STATUS status = hooks::EnsureMinHook(); status != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "the FG preset override could not start MinHook: {}", MH_StatusToString(status));
    g_install.store(2u, std::memory_order_release);
    return;
  }

  // The public export: every caller that goes through nvapi64.dll's own NvAPI_DRS_GetSetting. The
  // trampoline is stored before the hook is enabled: a call racing the enable must find the original.
  const auto public_target = reinterpret_cast<PublicGetSettingFn>(GetProcAddress(nvapi, "NvAPI_DRS_GetSetting"));
  void* public_original = nullptr;
  bool public_hooked = false;
  if (public_target != nullptr
      && MH_CreateHook(reinterpret_cast<void*>(public_target), reinterpret_cast<void*>(&PublicGetSettingDetour), &public_original) == MH_OK) {
    g_public_original.store(reinterpret_cast<PublicGetSettingFn>(public_original), std::memory_order_release);
    public_hooked = (MH_EnableHook(reinterpret_cast<void*>(public_target)) == MH_OK);
  }

  // The internal dispatcher implementation (nvapi_QueryInterface 0x73BF8338): callers that resolve
  // NvAPI through the interface table bypass the public export. The same filter covers both.
  const auto query = reinterpret_cast<void* (__cdecl*)(uint32_t)>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
  void* internal_original = nullptr;
  bool internal_hooked = false;
  if (query != nullptr) {
    const auto target = reinterpret_cast<GetSettingFn>(query(0x73BF8338u));
    if (target != nullptr
        && MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&InternalGetSettingDetour), &internal_original) == MH_OK) {
      g_internal_original.store(reinterpret_cast<GetSettingFn>(internal_original), std::memory_order_release);
      internal_hooked = (MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK);
    }
  }

  if (!public_hooked && !internal_hooked) {
    nr::Logf(nr::LogLevel::WARN, "the FG preset override could not hook NvAPI's DRS reads; the panel's FG preset stays off");
    g_install.store(2u, std::memory_order_release);
    return;
  }
  hooks::HoldMinHook();  // the hooks live for the process, like the NGX hooks'
  g_install.store(1u, std::memory_order_release);
  nr::Logf(nr::LogLevel::INFO, "the FG preset override hooked NvAPI's DRS reads ({}, {})", (public_hooked ? "public export" : "public export not hooked"),
           (internal_hooked ? "internal implementation" : "internal implementation not hooked"));
}

void NotifyFgQuiescent() {
  void** const slot = g_config_slot.load(std::memory_order_acquire);
  const uint32_t desired = g_desired.load(std::memory_order_relaxed);
  if (slot == nullptr || desired == 0u || desired > 3u || desired == g_last_served.load(std::memory_order_relaxed)) return;
  // Validate the slot's live contents before touching it: object and control pointers, the control
  // block's vtable inside the provider image, and the make_shared layout with both counts at 1.
  void* object = slot[0];
  void* control = slot[1];
  if (object == nullptr && control == nullptr) return;  // already reset, or never built
  const uintptr_t control_address = reinterpret_cast<uintptr_t>(control);
  if ((control_address & 7u) != 0u || control == nullptr) return;
  // The provider is named by the control block's vtable (inside its image); the object itself is a
  // heap allocation and never in a module.
  MEMORY_BASIC_INFORMATION heap = {};
  if (VirtualQuery(control, &heap, sizeof(heap)) == 0 || heap.Type != MEM_PRIVATE
      || heap.State != MEM_COMMIT || (heap.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0
      || (heap.Protect & 0xff) != PAGE_READWRITE) {
    return;
  }
  const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(control_address);
  HMODULE provider = nullptr;
  if (vtable == 0u
      || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(vtable), &provider)
             == FALSE
      || provider == nullptr) {
    return;
  }
  bool ours = false;
  for (const auto& hooked : g_hooked_modules) {
    ours |= (hooked.load(std::memory_order_acquire) == provider);
  }
  if (!ours || reinterpret_cast<void*>(object) != reinterpret_cast<void*>(control_address + 16)) return;
  if (*reinterpret_cast<const uint32_t*>(control_address + 8) != 1u
      || *reinterpret_cast<const uint32_t*>(control_address + 12) != 1u) {
    return;
  }
  // Quiescent (no FG feature lives) and the layout checks out: clear the object word. The provider's
  // next create publishes a fresh configuration, reading the preset through our hooked reader.
  if (InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot), nullptr, object) == object) {
    g_last_served.store(0u, std::memory_order_relaxed);
    nr::Logf(nr::LogLevel::INFO, "the FG preset override reset the provider's cached configuration; the next FG create rebuilds it with the panel's choice");
  }
}

}  // namespace uplift::addon
