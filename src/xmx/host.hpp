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

// The folder the Intel runtime is looked for in: the add-on's own directory (the game folder).
std::filesystem::path AddonDirectory();

}  // namespace uplift::xmx
