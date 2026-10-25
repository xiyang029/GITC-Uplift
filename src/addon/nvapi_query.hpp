#pragma once

// The shared, read-mostly NvAPI resolver (addon/nvapi_query.hpp).
//
// The Reflex override (addon/reflex_override.cpp) needs nvapi_QueryInterface to reach
// NvAPI_D3D_SetSleepMode's internal implementation, and the Streamline FG hooks
// (addon/sl_fg_hooks.cpp) read the live frame-generation multiplier through
// NvAPI_D3D_GetSleepStatus. Both interface ids resolve through the same
// nvapi_QueryInterface export, cached once per process like the FG preset hook's
// DRS resolver (addon/fg_preset.cpp).

#include <windows.h>
#include <unknwn.h>

#include <cstdint>

namespace uplift::addon::nvapi {

using Status = int32_t;
constexpr Status kOk = 0;
constexpr Status kUnavailable = INT32_MIN;  // the library or the interface is not there

using QueryInterfaceFn = void*(__cdecl*)(uint32_t);

// Public interface ids from NVIDIA's NvAPI SDK.
constexpr uint32_t kGetSleepStatusId = 0xAEF96CA1;
constexpr uint32_t kSetSleepModeId = 0xAC1CA9E0;

// NV_GET_SLEEP_STATUS_PARAMS_V1, 136 bytes. The fields this project reads are named; the
// live frame-generation multiplier is `frame_generation_multiplier` (MFGAdaUnlock's verified ABI).
#pragma pack(push, 8)
struct SleepStatus {
  uint32_t version;
  uint8_t low_latency_mode;
  uint8_t fullscreen_vrr;
  uint8_t control_panel_vsync;
  uint32_t sleep_interval_us;
  uint8_t game_sleep;
  uint8_t fullscreen_independent_flip;
  uint8_t frame_generation_multiplier;
  uint8_t dynamic_frame_generation_control;
  uint32_t dynamic_frame_time_target_us;
  uint8_t reserved[114];
};
#pragma pack(pop)
static_assert(sizeof(SleepStatus) == 136, "NV_GET_SLEEP_STATUS_PARAMS_V1 is 136 bytes");

constexpr uint32_t SleepStatusVersion() {
  return static_cast<uint32_t>(sizeof(SleepStatus)) | (1u << 16u);
}

// NV_SET_SLEEP_MODE_PARAMS_V1: NvU32 version, then five NvU32 booleans/interval, then 30 reserved
// bytes -- 54 bytes of fields, 56 once the structure's 4-byte alignment rounds it up. Field order
// matters; see NvAPI_D3D_SetSleepMode in nvapi.h.
struct SleepModeParams {
  uint32_t version;
  uint32_t b_low_latency_mode;
  uint32_t b_low_latency_boost;
  uint32_t minimum_interval_us;
  uint32_t b_use_markers_to_optimize;
  uint32_t b_use_min_queue_time;
  uint8_t reserved[30];
};
static_assert(sizeof(SleepModeParams) == 56, "NV_SET_SLEEP_MODE_PARAMS_V1 pads to 56 bytes");

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
  sample.sleep.version = SleepStatusVersion();
  sample.status = get(device, &sample.sleep);
  return sample;
}

}  // namespace uplift::addon::nvapi
