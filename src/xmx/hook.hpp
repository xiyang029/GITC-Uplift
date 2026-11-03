#pragma once
// SPDX-License-Identifier: MIT
// The XMX mode's entry point into the NGX evaluate path.
//
// The source project hooked nvngx_dlssg.dll's own EvaluateFeature, which works for a game that
// loads DLSS-G as a separate provider module. A Streamline game never maps that module: its
// DLSS-G arrives as Feature_FrameGeneration(11) through the NGX core, which Uplift already hooks
// (ngx_hooks/hook_installer.cpp). Field evidence (Phantom Blade Zero, 2026-11-02): the process
// mapped nvngx_dlssnr.dll and nvngx_deepdvc.dll only, while Uplift's own observer logged the
// game's DLSS-FG feature being created. So XMX rides that observer instead of a second MinHook:
// no provider module to find, no patch to take back out, and no way for the core's own generation
// to run behind XeFG's back.
#include <d3d12.h>
#include <cstdint>

#include "nr/ngx.hpp"

namespace uplift::xmx {

// Generates this evaluate's frame with XeFG. False when XMX is off, when the mode could not start,
// or when this frame's generation failed: the caller then lets the core run its own generation.
// On true the DLSS-G output buffers already hold the frame and the evaluate must be skipped.
[[nodiscard]] bool Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters);

// What the panel reports. failure points at storage that lives until the next call.
struct Status {
  // No "enabled" here: the caller already holds the setting it pushed, so a copy in the snapshot
  // would only be a second answer to the same question.
  bool generating{};
  bool hooked{};        // Preflight passed: the mode can serve this game's DLSS-G (no hook involved)
  bool evaluating{};    // DLSS-G called the generator within the last two seconds
  const char* failure{};
  // Why the last evaluate produced no generated frame (empty when it did): "waiting for two real
  // frames in a row" and "the game sent no input" are both "not generating", and only this tells
  // them apart. A literal, so it lives as long as the process.
  const char* idle{};
  // The tensor-core route's own counters (src/xmx/vendor/cuda/). "active" only says it installed;
  // a route whose launches stop climbing, or whose fallbacks climb at all, has stopped serving
  // layers -- and the frame rate alone would not show which. 0 layers/launches before the game's
  // first frame generation, and on the DXIL-only path.
  uint32_t layers{};
  uint64_t launches{};
  uint64_t fallbacks{};
};
Status Snapshot();

}  // namespace uplift::xmx
