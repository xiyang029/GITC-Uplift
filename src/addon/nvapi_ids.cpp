#include "addon/nvapi_query.hpp"

// The one translation unit that includes NVIDIA's { "NvAPI_...", id } table: nvapi_interface.h defines
// it, so including it from a header puts one definition in every object file that uses the resolver.
#include <nvapi_interface.h>

#include <cstring>

namespace uplift::addon::nvapi {

uint32_t InterfaceId(const char* name) {
  for (const NVAPI_INTERFACE_TABLE& entry : nvapi_interface_table) {
    if (entry.func != nullptr && std::strcmp(entry.func, name) == 0) return entry.id;
  }
  return 0u;  // not in the table: Interface() then resolves no entry point
}

uint32_t GetSleepStatusId() {
  static const uint32_t id = InterfaceId("NvAPI_D3D_GetSleepStatus");
  return id;
}

uint32_t SetSleepModeId() {
  static const uint32_t id = InterfaceId("NvAPI_D3D_SetSleepMode");
  return id;
}

uint32_t DrsGetSettingId() {
  static const uint32_t id = InterfaceId("NvAPI_DRS_GetSetting");
  return id;
}

}  // namespace uplift::addon::nvapi
