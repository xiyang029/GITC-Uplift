// SPDX-License-Identifier: MIT
#include "xmx/host.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>

namespace xess_mod {

HMODULE Self() noexcept {
  static const HMODULE self = [] {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Self), &module);
    return module;
  }();
  return self;
}

void WriteLog(const char* stage, const std::string& fields) noexcept {
  // The ported code logs every shader it decodes (54 programs, ~70 lines a start) and every layer
  // it recognizes, which drowns the frame log. Those two are per-item inventory: keep the ones that
  // matter (a replacement, a refusal) and drop the rest. Everything else passes through, including
  // every failure -- a reason is never filtered out.
  static constexpr const char* kQuiet[] = {"shader_native_lookup", "shader_supplied", "cuda_layer", "cuda_list"};
  try {
    if (stage != nullptr) {
      for (const char* quiet : kQuiet) {
        if (std::strcmp(stage, quiet) == 0) return;
      }
    }
    uplift::nr::Logf(uplift::nr::LogLevel::INFO, "xmx {}{}", stage != nullptr ? stage : "",
                     fields.empty() ? std::string() : (" " + fields));
  } catch (...) {
    // A log line never costs the ported code its frame.
  }
}

}  // namespace xess_mod

namespace uplift::xmx {
namespace {
// The panel's switch. One relaxed load per evaluate: the path that reads it is on the frame's
// critical path and the value only ever changes from the add-on's own settings push.
std::atomic_bool g_enabled{false};
// The upstream 1.8.6 switch (e4d4aad). Same access pattern as the panel's switch: the init path
// reads it once per context creation and the add-on's settings push is the only writer.
std::atomic_bool g_jitter_strip{true};
}  // namespace

void SetEnabled(bool enabled) noexcept { g_enabled.store(enabled, std::memory_order_relaxed); }
bool Enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void SetJitterStrip(bool strip) noexcept { g_jitter_strip.store(strip, std::memory_order_relaxed); }
bool JitterStrip() noexcept { return g_jitter_strip.load(std::memory_order_relaxed); }

std::filesystem::path AddonDirectory() {
  static const std::filesystem::path cached = [] {
    std::wstring path(32768, L'\0');
    const HMODULE module = xess_mod::Self();
    const DWORD length = module != nullptr
                             ? GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()))
                             : GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::filesystem::path{};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
  }();
  return cached;
}

}  // namespace uplift::xmx
