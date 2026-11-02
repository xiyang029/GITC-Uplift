// SPDX-License-Identifier: MIT
#include "xmx/hook.hpp"

#include <atomic>
#include <mutex>
#include <string>

#include "nr/log.hpp"
#include "xmx/generator.hpp"
#include "xmx/vendor/cuda/cuda_route.h"

namespace uplift::xmx {
namespace {

// Counts evaluates, for the periodic report: a mode that waits must say why.
std::atomic<uint32_t> g_evaluates{0};
std::atomic<int> g_reported_state{-1};

}  // namespace

bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters) {
  if (!generator::Enabled()) return false;
  if (list == nullptr || parameters == nullptr) return false;
  const uint32_t seen = g_evaluates.fetch_add(1, std::memory_order_relaxed) + 1u;
  // One report every kReportEvaluates frame-generation evaluates: a refused mode always explains
  // itself, and a running one keeps saying what the tensor cores are doing. At 6x five evaluates
  // make one presented frame, so this is roughly one line a minute at 200 fps.
  constexpr uint32_t kReportEvaluates = 6000u;
  const bool report = (seen % kReportEvaluates) == 1u;
  if (!generator::Preflight()) {
    if (report) {
      const generator::Report state = generator::Snapshot();
      nr::Logf(nr::LogLevel::ERR, "xmx: preflight refused ({}); the game's DLSS-G keeps NVIDIA's generation",
               state.failure.empty() ? "no reason recorded" : state.failure);
    }
    return false;
  }
  // The generator writes into DLSSG.OutputInterpolated/OutputReal and returns false on any refusal,
  // in which case the core runs its own generation for this frame.
  const bool generated = generator::Evaluate(list, parameters);
  // At every report, what the tensor-core route has actually done: "active" only says it
  // installed. A route whose launches stay at 0 while fallbacks climb is not serving the layers at
  // all, and the frame rate would not show that on its own.
  if (report) {
    const auto route = xefg_cuda::Snapshot();
    const char* state = "not attempted";
    switch (route.state) {
      case xefg_cuda::State::Active: state = "active"; break;
      case xefg_cuda::State::Unavailable: state = "unavailable"; break;
      case xefg_cuda::State::Disabled: state = "disabled"; break;
      case xefg_cuda::State::NotAttempted: break;
    }
    nr::Logf(nr::LogLevel::INFO, "xmx: tensor cores {} ({} layer(s), {} launch(es), {} fallback(s)){}", state,
             route.layers, route.launches, route.fallbacks,
             route.reason.empty() ? std::string() : (" [" + route.reason + "]"));
  }
  return generated;
}

Status Snapshot() {
  static std::mutex lock;
  static std::string failure;
  const generator::Report report = generator::Snapshot();
  const int state = report.generating ? 1 : 0;
  {
    std::lock_guard guard(lock);
    failure = report.failure;
  }
  if (g_reported_state.exchange(state, std::memory_order_relaxed) != state) {
    if (report.generating) nr::Logf(nr::LogLevel::INFO, "xmx: XeFG is generating this game's DLSS-G frames");
  }
  // The panel calls this on the render thread every frame it draws, and the route holds gMutex while
  // it recognises pipeline states. TrySnapshot exists for exactly this reader ("never wait for
  // installation/retirement work"): on a contended frame the counters come from the last snapshot
  // instead of blocking the overlay behind a hook.
  static xefg_cuda::Status cached;
  xefg_cuda::Status route;
  if (xefg_cuda::TrySnapshot(route)) cached = route;
  else route = cached;
  return {report.generating, generator::Enabled() && report.hooked, failure.c_str(),
          route.layers,      route.launches,                        route.fallbacks};
}

}  // namespace uplift::xmx
