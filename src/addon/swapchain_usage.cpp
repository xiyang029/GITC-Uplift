#include "addon/swapchain_usage.hpp"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "addon/reshade_api.hpp"
#include "addon/uplift_catch.hpp"
#include "nr/log.hpp"
#include "vk/frame.hpp"

namespace uplift::addon {
namespace {

namespace api = reshade::api;

std::atomic<bool> g_vulkan_swapchain_event{false};
std::atomic<bool> g_desc_has_fullscreen_state{false};
std::atomic<bool> g_fullscreen_skip_logged{false};
std::atomic<bool> g_d3d9ex_requested{false};  // 1.1.5: NoteD3D9ExRequested
std::atomic<bool> g_d3d9ex_windowed_logged{false};

// Vulkan only, and no Uplift lock, ever (Plan 10 C-1): ReShade raises this inside vkCreateSwapchainKHR, and other APIs' swap chains pass through
// it too. Returning true makes ReShade rebuild the create info from `desc`; the round trip keeps every other field and every unmapped usage bit
// (vulkan_hooks_swapchain.cpp:220-255, vulkan_impl_type_convert.cpp:860-910), except for a swap chain that asks for exclusive fullscreen (below).
bool OnCreateSwapchain(api::device_api device_api, api::swapchain_desc& desc, void* /*hwnd*/) {
  try {
    if (device_api == api::device_api::d3d9) {
      // 1.1.5: a Direct3D 9Ex device in exclusive fullscreen cannot be made through ReShade's upgrade (see NoteD3D9ExRequested): run it windowed at the
      // same size instead. Returning true makes ReShade write Windowed = TRUE and a zero refresh rate into the present parameters (d3d9.cpp:151-155).
      if (!g_d3d9ex_requested.load(std::memory_order_relaxed) || !g_desc_has_fullscreen_state.load(std::memory_order_relaxed)) return false;
      if (!desc.fullscreen_state) return false;
      desc.fullscreen_state = false;
      desc.fullscreen_refresh_rate = 0.f;
      if (!g_d3d9ex_windowed_logged.exchange(true, std::memory_order_relaxed)) {
        nr::Log(nr::LogLevel::INFO,
                "Direct3D 9Ex: the game asks for exclusive fullscreen, which ReShade cannot create as a Direct3D 9Ex device; Uplift runs it as a "
                "window of the same size instead (borderless)");
      }
      return true;
    }
    if (device_api != api::device_api::vulkan) return false;
    if (g_desc_has_fullscreen_state.load(std::memory_order_relaxed) && desc.fullscreen_state) {
      // Final review I-2 (see the header): no rebuild. The swap chain keeps ReShade's own usage bits, without TRANSFER_DST, and the card says why.
      vk::NoteExclusiveFullscreenSwapchain();
      if (!g_fullscreen_skip_logged.exchange(true, std::memory_order_relaxed)) {
        nr::Log(nr::LogLevel::INFO,
                "Vulkan: swap-chain copy access not added (create_swapchain): the game asks for exclusive fullscreen, so NR cannot write back into "
                "this swap chain; use borderless or windowed mode for NR");
      }
      return false;
    }
    desc.back_buffer.usage |= api::resource_usage::copy_dest | api::resource_usage::copy_source;
    if (!g_vulkan_swapchain_event.exchange(true, std::memory_order_relaxed)) {
      nr::Log(nr::LogLevel::INFO, "Vulkan: swap-chain copy access added (create_swapchain)");
    }
    return true;
  }
  UPLIFT_CATCH("create_swapchain", false)
}

}  // namespace

void NoteD3D9ExRequested() {
  g_d3d9ex_requested.store(true, std::memory_order_relaxed);
}

void RegisterCreateSwapchainEvent(bool desc_has_fullscreen_state) {
  g_desc_has_fullscreen_state.store(desc_has_fullscreen_state, std::memory_order_relaxed);
  reshade::register_event<reshade::addon_event::create_swapchain>(&OnCreateSwapchain);
}

bool CreateSwapchainEventSeen() {
  return g_vulkan_swapchain_event.load(std::memory_order_relaxed);
}

}  // namespace uplift::addon
