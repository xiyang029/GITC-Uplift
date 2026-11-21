#include "addon/nvfp4_hook.hpp"

#include <windows.h>

#include <MinHook.h>

#include <atomic>

#include "ngx_hooks/minhook_once.hpp"
#include "nr/log.hpp"
#include "nvfp4/NrNvfp4.h"

namespace uplift::addon {
namespace {

using QueryInterfaceFn = void*(__cdecl*)(uint32_t);

std::atomic<uint32_t> g_install{0u};  // 0 idle, 3 installing, 1 installed, 2 failed
std::atomic<QueryInterfaceFn> g_original{nullptr};
std::filesystem::path g_assets_directory;  // set once at init, before any frame runs

void Nvfp4Log(const char* line) noexcept {
  try {
    nr::Log(nr::LogLevel::INFO, line != nullptr ? line : "");
  } catch (...) {
    // The callback runs on NVIDIA's threads, including from inside the NvAPI detour. Nothing may escape.
  }
}

void* __cdecl QueryInterfaceDetour(uint32_t id) {
  const QueryInterfaceFn original = g_original.load(std::memory_order_acquire);
  void* const entry = original != nullptr ? original(id) : nullptr;
  return DlssNr::Nvfp4::WrapNvapi(id, entry);
}

void InstallNvfp4Hook() {
  uint32_t state = 0u;  // Only the idle -> installing transition owns installation.
  if (!g_install.compare_exchange_strong(state, 3u, std::memory_order_acq_rel)) return;

  const HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
  if (nvapi == nullptr) {
    g_install.store(0u, std::memory_order_release);  // not loaded yet: the poll tries again
    return;
  }
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     reinterpret_cast<LPCWSTR>(nvapi), &pinned);

  if (const MH_STATUS status = hooks::EnsureMinHook(); status != MH_OK) {
    nr::Logf(nr::LogLevel::WARN, "NVFP4 could not start MinHook: {}", MH_StatusToString(status));
    g_install.store(2u, std::memory_order_release);
    return;
  }

  const auto query = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
  void* original = nullptr;
  if (query == nullptr || MH_CreateHook(reinterpret_cast<void*>(query),
                                        reinterpret_cast<void*>(&QueryInterfaceDetour), &original) != MH_OK
      || original == nullptr) {
    nr::Log(nr::LogLevel::WARN, "NVFP4 could not hook nvapi_QueryInterface; 4-bit models stay unavailable");
    g_install.store(2u, std::memory_order_release);
    return;
  }
  g_original.store(reinterpret_cast<QueryInterfaceFn>(original), std::memory_order_release);
  if (MH_EnableHook(reinterpret_cast<void*>(query)) != MH_OK) {
    nr::Log(nr::LogLevel::WARN, "NVFP4 could not enable the nvapi_QueryInterface hook; 4-bit models stay unavailable");
    g_install.store(2u, std::memory_order_release);
    return;
  }
  DlssNr::Nvfp4::SetLog(&Nvfp4Log);
  hooks::HoldMinHook();
  g_install.store(1u, std::memory_order_release);
  nr::Log(nr::LogLevel::INFO, "NVFP4 hooked nvapi_QueryInterface (4-bit kernels idle until a pass asks for them)");
}

}  // namespace

void EnsureNvfp4Hook() { InstallNvfp4Hook(); }

void SetNvfp4AssetsDirectory(const std::filesystem::path& addon_directory) {
  g_assets_directory = addon_directory;
}

void RequestNvfp4(bool wanted) {
  if (!wanted) return;  // Configure() keeps its layers loaded; asking for none just leaves them idle.
  // Configure() asks NvAPI for entry points through our detour, so the hook must be in place first.
  InstallNvfp4Hook();
  if (g_install.load(std::memory_order_acquire) != 1u) return;  // nvapi64.dll not loaded yet: retry next poll
  if (g_assets_directory.empty()) return;  // init has not supplied it yet
  DlssNr::Nvfp4::Configure(true, g_assets_directory / L"nvfp4");
}

bool Nvfp4Refused() { return !DlssNr::Nvfp4::Problem().empty(); }

std::string Nvfp4Problem() { return DlssNr::Nvfp4::Problem(); }

void RefuseNvfp4(const char* reason) { DlssNr::Nvfp4::Refuse(reason); }

}  // namespace uplift::addon
