// SPDX-License-Identifier: MIT
#include "xmx/model.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>

#include "xmx/host.hpp"
#include "xmx/vendor/host_unlock.h"
#include "xmx/vendor/port/port_support.h"  // logEvent/log helpers the two ports below call
#include "xmx/vendor/port/model_port.h"
#include "xmx/vendor/port/shader_port.h"
#include "xmx/vendor/runtime_layout.h"

namespace uplift::xmx::model {
namespace {

std::mutex g_lock;
bool g_published = false;
bool g_failed = false;
std::string g_reason;

bool Refuse(const char* reason, std::string& error) {
  g_failed = true;
  g_reason = reason;
  error = reason;
  xess_mod::WriteLog("port_rejected", std::string("\"reason\":\"") + reason + "\"");
  return false;
}

}  // namespace

bool Publish(HMODULE frame_generation, HMODULE latency, std::string& error) {
  std::lock_guard guard(g_lock);
  if (g_published) return true;
  if (g_failed) {
    error = g_reason;
    return false;
  }
  try {
    if (!xess_layout::Pair(frame_generation, latency)) return Refuse("provider_layout_rejected", error);
    // Both ports stay alive for the process: contexts may still be in flight.
    static auto shaders = std::make_unique<ShaderPort>(frame_generation, xess_mod::Self());
    static auto models = std::make_unique<ModelPort>(frame_generation, true, true);
    static auto unlock = std::make_unique<xess_host_unlock::Unlock>(
        frame_generation, latency, static_cast<uint8_t>(kGeneratedFrames), false);
    g_published = true;
    xess_mod::WriteLog("model_port_published", std::string("\"generated\":") + std::to_string(kGeneratedFrames) +
                                                   ",\"keep_2x_model\":false");
    return true;
  } catch (const std::exception& e) {
    return Refuse(e.what(), error);
  } catch (...) {
    return Refuse("unknown", error);
  }
}

}  // namespace uplift::xmx::model
