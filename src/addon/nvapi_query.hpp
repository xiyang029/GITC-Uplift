#pragma once

// The shared, read-mostly NvAPI resolver (addon/nvapi_query.hpp).
//
// Two callers resolve through it: the Reflex override (addon/reflex_override.cpp) reaches
// NvAPI_D3D_SetSleepMode's internal implementation and reads the driver's own state through
// NvAPI_D3D_GetSleepStatus; the FG preset override (addon/fg_preset.cpp) hooks NvAPI_DRS_GetSetting's
// internal implementation beside its public export. Every id resolves through the same
// nvapi_QueryInterface export, cached once per process (Module() below).
//
// The structures are NVIDIA's own (nvapi.h, shipped by the pinned Streamline commit): nothing
// here is a hand-copied ABI. NV_SET_SLEEP_MODE_PARAMS' version a caller sends is
// MAKE_NVAPI_VERSION(NV_SET_SLEEP_MODE_PARAMS_V1, 1) = sizeof | 1 << 16 = 0x1002c, which is what
// KnownSleepModeVersion in reflex_override.cpp checks against.

#include <windows.h>
#include <unknwn.h>

#include <nvapi.h>

#include <cstdint>
#include <cstring>

namespace uplift::addon::nvapi {

using Status = NvAPI_Status;
constexpr Status kOk = NVAPI_OK;
// Not an NvAPI_Status: this project's own "the library or the interface is not there".
constexpr Status kUnavailable = static_cast<Status>(INT32_MIN);

using QueryInterfaceFn = void*(__cdecl*)(uint32_t);

// The NvAPI_QueryInterface ids come from NVIDIA's own table instead of being transcribed here:
// nvapi_interface.h (MIT, shipped by the pinned Streamline commit next to nvapi.h) is a plain
// { "NvAPI_...", id } list, so the names are looked up in it (addon/nvapi_ids.cpp -- one definition of
// that table per process, so the header declares and never includes it). nvapi.h declares the
// functions and their structures but carries no ids, and nvapi64.dll exports only nvapi_QueryInterface.
[[nodiscard]] uint32_t InterfaceId(const char* name);

[[nodiscard]] uint32_t GetSleepStatusId();
[[nodiscard]] uint32_t SetSleepModeId();
[[nodiscard]] uint32_t DrsGetSettingId();

using SleepStatus = NV_GET_SLEEP_STATUS_PARAMS;
using SleepModeParams = NV_SET_SLEEP_MODE_PARAMS;

// Loads the system's nvapi64.dll (or returns the copy already mapped); null when the driver has
// none, which stays null for the process's life.
inline HMODULE Module() {
  static const HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return module;
}

inline QueryInterfaceFn QueryInterface() {
  static const auto query = []() -> QueryInterfaceFn {
    const HMODULE module = Module();
    return module == nullptr
               ? nullptr
               : reinterpret_cast<QueryInterfaceFn>(GetProcAddress(module, "nvapi_QueryInterface"));
  }();
  return query;
}

template <typename Fn>
Fn Interface(uint32_t id) {
  const auto query = QueryInterface();
  return query == nullptr ? nullptr : reinterpret_cast<Fn>(query(id));
}

// The Reflex/FG sleep state for `device`, with kUnavailable when NvAPI cannot answer. Read-only.
struct SleepSample {
  Status status = kUnavailable;
  SleepStatus sleep{};
};
inline SleepSample GetSleepStatus(IUnknown* device) {
  SleepSample sample{};
  if (device == nullptr) return sample;
  const auto get = Interface<Status(__cdecl*)(IUnknown*, SleepStatus*)>(GetSleepStatusId());
  if (get == nullptr) return sample;
  sample.sleep.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
  sample.status = get(device, &sample.sleep);
  return sample;
}

}  // namespace uplift::addon::nvapi
