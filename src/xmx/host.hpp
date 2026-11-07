#pragma once
// SPDX-License-Identifier: MIT
// The host adapter the ported XeFG model expects (xess_mod::WriteLog in the source project's
// headers). The ported code only ever asks for a log line; the mode itself is a Uplift setting,
// pushed here by the add-on (addon.cpp's ApplyProcessSettings), never read from a file.
#include <Windows.h>

#include <filesystem>
#include <string>

#include "nr/log.hpp"

namespace xess_mod {

// The module the ported RCDATA payload is read from: this add-on.
HMODULE Self() noexcept;
void WriteLog(const char* stage, const std::string& fields) noexcept;

}  // namespace xess_mod

namespace uplift::xmx {

// The mode the panel asked for, pushed by the add-on at every settings change. Live: the
// evaluate path reads it per call, so switching takes effect on the next frame and the core's own
// generation is never touched.
void SetEnabled(bool enabled) noexcept;
bool Enabled() noexcept;

// Upstream 1.8.6 (e4d4aad): the ported multi-frame model refuses the jittered-motion flag, so it
// is started without that flag (the game's jitter survives in the motion vectors, about a render
// pixel at most). Default on; XmxJitter=0 under [Uplift] in ReShade.ini restores the 2x fallback.
void SetJitterStrip(bool strip) noexcept;
bool JitterStrip() noexcept;

// The folder the Intel runtime is looked for in: the add-on's own directory (the game folder).
std::filesystem::path AddonDirectory();

}  // namespace uplift::xmx
