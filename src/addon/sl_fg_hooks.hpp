#pragma once

// The Streamline frame-generation hooks (addon/sl_fg_hooks.{hpp,cpp}).
//
// The live FG multiplier and Dynamic MFG overrides, ported from MFGAdaUnlock-RenoDx's
// framecount.hpp. Streamline games obtain slDLSSGSetOptions/slDLSSGGetState through
// sl.interposer.dll's slGetFeatureFunction, once, during DLSS-G setup -- so that export is
// hooked and the game is handed wrappers. The wrappers rewrite numFramesToGenerate
// (generated frames: 2x -> 1, 3x -> 2, 4x -> 3) in an addon-owned copy and submit NVIDIA's
// eDynamic mode for Dynamic MFG; the game's own structures are never written, an Off request
// is never rewritten, and a rejected call falls back to the game's request so the player
// never loses frame generation.

#include <cstdint>

namespace uplift::addon::slfg {

// A DLSS-G option row: Game keeps the game's own value, On/Off force it into the add-on's own
// options copy on every enabled submission.
enum class FgOptionOverride : uint32_t {
  GAME = 0u,
  ON = 1u,
  OFF = 2u,
};

// 0 = leave the game's request alone; 2..6 = force that exact multiplier. Applied on the
// game's next enabled slDLSSGSetOptions call (no restart, no config reload).
void SetForceMultiplier(uint32_t multiplier);

// The two DLSS-G option fields (2026-10-26), applied on every enabled submission -- with or
// without a multiplier override. UI recomposition asks the provider to interpolate the HUD and
// game UI separately (only effective when the game tags HUDLessColor/UIColorAndAlpha);
// fullscreen menu detection is DLSSGFlags::eEnableFullscreenMenuDetection.
void SetOptionOverrides(FgOptionOverride ui_recomposition, FgOptionOverride menu_detection);

// Dynamic MFG: submit DLSSGMode::eDynamic and let NVIDIA's provider own multiplier selection
// and pacing. `target_fps` 0 = follow the active display refresh rate.
void SetDynamicMfg(bool enabled, uint32_t target_fps);

// Raise DLSSGState::numFramesToGenerateMax in the game's GetState answers, so native
// multiplier selectors built from it (STALKER 2) offer more than 2x. 0 = do not advertise.
// Never raises above the runtime's own ceiling (GeneratedFramesCeiling).
void SetAdvertisedMaxGenerated(uint32_t generated_frames);

// The DLSS-G runtime's own numFramesToGenerateMax, seen before this add-on advertises more
// (generated frames: 4 = 5x, 5 = 6x). 0 = not observed yet: offer every multiplier.
uint32_t GeneratedFramesCeiling();

// The game's graphics API, observed by the add-on at Present; Dynamic MFG is D3D12-only.
void NotifyDynamicD3D12(bool d3d12);

// Installs the slGetFeatureFunction hook once sl.interposer.dll is mapped. Must land before
// the game asks for the function pointer (it asks once); call from every poll.
void EnsureSlFgHooks();

// Re-submits a pending panel change through the cached last enabled submission, so the multiplier
// and Dynamic MFG land without the game's own next call. Called from the add-on's Present poll.
void RefreshFromPanel();

}  // namespace uplift::addon::slfg
