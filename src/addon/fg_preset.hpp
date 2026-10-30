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
// The driver setting is NVDRS_SETTING_V1, NVIDIA's own structure from nvapi.h (nothing hand-copied);
// the setting id is the discovered one, after MFG-Unlocker, MIT.

// The panel's choice: 0 = the game's/driver's own, 1 = A, 2 = B, 3 = C (C is DLSS-G 310.9.16+'s
// "performance mode"; older providers ignore it). Pushed by the add-on at init and on SaveAndApply.
void SetFgPresetDesired(uint32_t preset);

// The DLSS-SR and Ray Reconstruction preset settings, read by their provider once at initialization
// exactly like the FG one, with the same consequence: a driver or NVIDIA App value there outranks the
// per-create hint keys the live SR/RR swap writes. While the panel forces a preset for a feature, its
// read answers "not set" so the hints decide; with the panel on Game the driver's answer passes.
// Pushed by the add-on at init and on every SaveAndApply, next to SetFgPresetDesired.
void SetSrRrPresetForced(bool sr, bool rr);

// Installs the DRS read hooks when nvapi64.dll is in the process (cheap once-flagged polls; call
// from the add-on's PollHooks). Never throws; a failed install latches and is logged once.
void EnsureFgPresetHook();

// The last frame-generation feature just went away (the registry's FG count is zero): if the panel's
// choice differs from what the provider's configuration holds, clear the provider's cached
// configuration slot so its next create rebuilds it with the panel's choice -- the live switch.
void NotifyFgQuiescent();

// Whether a live A/B/C switch is both possible (the provider's configuration slot is known) and still
// to be done (the panel's choice differs from what the provider last served). True once per distinct
// panel choice: the caller turns frame generation off for a drain, then NotifyFgQuiescent.
bool FgPresetSwitchPending();

// The provider's reset count, for a caller waiting on the drain to land.
uint64_t FgPresetResets();

// What the DLSS-SR (or, with `ray_reconstruction`, the Ray Reconstruction) provider's own driver-setting
// read answered when it initialised, for the panel's live row. `driver_value` is the value the driver
// held -- an NVIDIA App override lands there; `suppressed` says whether the add-on withheld it so the
// creation hints decide. A too-late hook shows as `read_seen` false with a chosen preset never applied.
struct PresetEvidence {
  bool read_seen = false;
  uint32_t driver_value = 0u;  // 0 = the driver had no override
  bool suppressed = false;
};
[[nodiscard]] PresetEvidence SrRrPresetEvidence(bool ray_reconstruction);

}  // namespace uplift::addon
