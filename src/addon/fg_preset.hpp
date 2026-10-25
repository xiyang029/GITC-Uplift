#pragma once

#include <cstdint>

namespace uplift::addon {

// The DLSS-FG model preset switch (2026-10-25, after dashdogy/MFG-Unlocker's findings): DLSS Frame
// Generation's A/B/C model presets are not NGX hint keys -- the FG provider reads a driver setting
// (NvAPI DRS, id 0x10E41DF1) once, at its first feature create, and caches it. The panel's choice is
// applied by hooking NvAPI's DRS GetSetting (the public export and the internal dispatcher
// implementation, both) and rewriting the answer for that one id after the driver responded -- which
// also beats a NVIDIA App frame-generation override, for the same reason. The provider caches the
// first read, so a change lands at the process's next start (MFG-Unlocker's "live switching" needs
// pattern-scanning the provider's cached reader; deliberately not ported).
//
// ABI facts (public NVAPI: NVDRS_SETTING_V1, and the discovered setting id) after MFG-Unlocker, MIT.

// The panel's choice: 0 = the game's/driver's own, 1 = A, 2 = B, 3 = C (C is DLSS-G 310.9.16+'s
// "performance mode"; older providers ignore it). Pushed by the add-on at init and on SaveAndApply.
void SetFgPresetDesired(uint32_t preset);

// Installs the DRS read hooks when nvapi64.dll is in the process (cheap once-flagged polls; call
// from the add-on's PollHooks). Never throws; a failed install latches and is logged once.
void EnsureFgPresetHook();

// The last frame-generation feature just went away (the registry's FG count is zero): if the panel's
// choice differs from what the provider's configuration holds, clear the provider's cached
// configuration slot so its next create rebuilds it with the panel's choice -- the live switch.
void NotifyFgQuiescent();

}  // namespace uplift::addon
