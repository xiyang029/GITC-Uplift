#pragma once
// SPDX-License-Identifier: MIT
// The multi-frame model, published once per process before the first XeFG Init: the shader
// decoder port, the model catalog port (15 -> 16) and the XeLL/XeFG frame-count unlock. Same
// three ports the source project's PublishMultiFrameModel builds, without the swap chain route
// around them. Offsets are libxess_fg 1.3.1.78; any other build is refused before a byte is
// written.
#include <Windows.h>

#include <string>

namespace uplift::xmx::model {

// 5 generated frames per real frame: what the ported model serves (1.8's 3x-6x). The game's own
// DLSS-G setting picks the count it actually asks for (DLSSG.MultiFrameCount, 1..5); this is only
// the ceiling the unlock and XeFG's Init advertise.
inline constexpr uint32_t kGeneratedFrames = 5;

// Publishes the model for the pair the game loaded. Returns false with a reason the panel shows;
// the process then keeps NVIDIA's generation for this frame, never a half-published model.
bool Publish(HMODULE frame_generation, HMODULE latency, std::string& error);

}  // namespace uplift::xmx::model
