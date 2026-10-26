#pragma once

// The shared, read-mostly NvAPI resolver (addon/nvapi_query.hpp).
//
// The Reflex override (addon/reflex_override.cpp) is the only caller: it needs nvapi_QueryInterface
// to reach NvAPI_D3D_SetSleepMode's internal implementation, reads the driver's own state through
// NvAPI_D3D_GetSleepStatus, and samples NvAPI_D3D_GetLatency for the panel's latency line. Every id
// resolves through the same nvapi_QueryInterface export, cached once per process like the FG preset
// hook's DRS resolver (addon/fg_preset.cpp).
//
// The structures are NVIDIA's own (nvapi.h, shipped by the pinned Streamline commit): nothing
// here is a hand-copied ABI. NV_SET_SLEEP_MODE_PARAMS' version a caller sends is
// MAKE_NVAPI_VERSION(NV_SET_SLEEP_MODE_PARAMS_V1, 1) = sizeof | 1 << 16 = 0x1002c, which is what
// KnownSleepModeVersion in reflex_override.cpp checks against.

#include <windows.h>
#include <unknwn.h>

#include <nvapi.h>
#include <nvapi_interface.h>  // NVIDIA's own { "NvAPI_...", id } table, the source of the ids below

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
// { "NvAPI_...", id } list, so the names this file needs are looked up in it. nvapi.h declares the
// functions and their structures but carries no ids, and nvapi64.dll exports only nvapi_QueryInterface.
inline uint32_t InterfaceId(const char* name) {
  for (const NVAPI_INTERFACE_TABLE& entry : nvapi_interface_table) {
    if (entry.func != nullptr && std::strcmp(entry.func, name) == 0) return entry.id;
  }
  return 0u;  // not in the table: Interface() then resolves no entry point
}

inline const uint32_t kGetSleepStatusId = InterfaceId("NvAPI_D3D_GetSleepStatus");
inline const uint32_t kSetSleepModeId = InterfaceId("NvAPI_D3D_SetSleepMode");
inline const uint32_t kGetLatencyId = InterfaceId("NvAPI_D3D_GetLatency");

using SleepStatus = NV_GET_SLEEP_STATUS_PARAMS;
using SleepModeParams = NV_SET_SLEEP_MODE_PARAMS;
using LatencyResult = NV_LATENCY_RESULT_PARAMS;

// The system's nvapi64.dll, or null when the driver has not loaded it into this process yet.
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
  const auto get = Interface<Status(__cdecl*)(IUnknown*, SleepStatus*)>(kGetSleepStatusId);
  if (get == nullptr) return sample;
  sample.sleep.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
  sample.status = get(device, &sample.sleep);
  return sample;
}

// The latency report: the newest completed frame sits at element 63 (nvapi.h's NvAPI_D3D_GetLatency
// comment), and every frame is zeroed until the game has marked at least that many frames.
inline Status GetLatency(IUnknown* device, LatencyResult* out) {
  if (device == nullptr || out == nullptr) return kUnavailable;
  const auto get = Interface<Status(__cdecl*)(IUnknown*, LatencyResult*)>(kGetLatencyId);
  if (get == nullptr) return kUnavailable;
  out->version = NV_LATENCY_RESULT_PARAMS_VER;
  return get(device, out);
}

}  // namespace uplift::addon::nvapi
