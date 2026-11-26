#pragma once

// The Streamline frame-generation hooks (addon/sl_fg_hooks.{hpp,cpp}).
//
// The live FG multiplier and Dynamic MFG overrides, ported from MFGAdaUnlock-RenoDx's
// framecount.hpp. Streamline games obtain slDLSSGSetOptions/slDLSSGGetState through
// sl.interposer.dll's slGetFeatureFunction, once, during DLSS-G setup -- so that export is
// hooked and the game is handed wrappers. The wrappers rewrite numFramesToGenerate
// (generated frames: 2x -> 1, 3x -> 2, 4x -> 3) in an addon-owned copy and submit NVIDIA's
// eDynamic mode for Dynamic MFG; the game's own structures are never written. Normal multiplier
// overrides leave Off alone and fall back to the game's request on rejection. The experimental
// HUD-inputs-only mode instead submits real Off, reports rejection, and never falls back to On.

#include <cstdint>
#include <string_view>

namespace uplift::addon::slfg {

// 0 = leave the game's request alone; 2..6 = force that exact multiplier. Applied on the
// game's next enabled slDLSSGSetOptions call (no restart, no config reload).
void SetForceMultiplier(uint32_t multiplier);

// Experimental: keep the game's request for restoration, but submit real Off to Streamline.
// This does not manufacture HUD inputs or skip/fake a successful NGX evaluate.
void SetHudInputsOnly(bool enabled);
[[nodiscard]] bool HudInputsOnlyWanted();
[[nodiscard]] bool HudInputsOnlyActive();  // true only after the backend accepted this mode's Off
void NoteFgEvaluate();
// The selected Present's previous UI-correction outcome, in addition to tag availability.
void TraceHudInputs(uint64_t hudless, uint64_t opacity, bool recent_hudless, bool recent_opacity, std::string_view correction);

// Dynamic MFG: submit DLSSGMode::eDynamic and let NVIDIA's provider own multiplier selection
// and pacing. `target_fps` 0 = follow the active display refresh rate.
void SetDynamicMfg(bool enabled, uint32_t target_fps);

// Raise DLSSGState::numFramesToGenerateMax in the game's GetState answers, so native
// multiplier selectors built from it (STALKER 2) offer more than 2x. 0 = do not advertise.
// Never raises above the runtime's own ceiling.
void SetAdvertisedMaxGenerated(uint32_t generated_frames);

// XMX owns the multiplier through its own ported model (src/xmx/), so the provider is never asked
// for NVIDIA's Dynamic MFG: that row stands down while this is on.
void SetXmxMode(bool xmx);

// The game's graphics API, observed by the add-on at Present; Dynamic MFG is D3D12-only.
void NotifyDynamicD3D12(bool d3d12);

// Installs the slGetFeatureFunction hook once sl.interposer.dll is mapped. Must land before
// the game asks for the function pointer (it asks once); call from every poll.
void EnsureSlFgHooks();

// Re-submits a pending panel change through the cached last enabled submission, so the multiplier
// and Dynamic MFG land without the game's own next call. Called from the add-on's Present poll.
void RefreshFromPanel();

// The DLSS Frame Generation model preset's live switch (addon/fg_preset.hpp): the provider caches its
// model configuration at the first FG create, so a new preset only lands while no FG feature lives.
// While a panel choice is pending this submits Off for one drain (without resource retention), lets the
// release empty the provider's cache, then resubmits the game's own request. Called from the add-on's
// Present poll; a no-op in a game without Streamline.
void FgPresetTick();

// What the DLSS-G runtime last accepted, for the panel's live row: the presented frames per real frame
// (1 when frame generation is off), whether that count is the panel's own rather than the game's, and
// whether Dynamic MFG owns the selection (its multiplier belongs to the provider, so multiplier is then
// 1 and dynamic says why). Before any submission: 1, false, false.
struct LiveState {
  uint32_t multiplier = 1u;
  bool panel_multiplier = false;
  bool dynamic = false;
  bool streamline = false;  // the game's frame generation goes through Streamline (a live preset switch is possible)
  bool switching = false;   // the FG preset's drain is in progress right now
  bool hud_inputs_only = false;
  bool hud_inputs_active = false;
  bool hud_inputs_refused = false;
  bool hud_inputs_unsupported = false;
  uint64_t fg_evaluates = 0u;  // calls observed since enabling the experiment, not GPU dispatches
  uint32_t presented_batch = 0u;  // last real GetState answer, 0 when none was observed
  // Dynamic MFG's own gates, for its row: what the panel asked for, whether every gate allows it right
  // now, and which one refused when it does not (support not yet reported, or reported unsupported;
  // declined after the runtime refused a submission).
  bool dynamic_wanted = false;
  bool dynamic_ready = false;
  bool dynamic_supported = false;
  bool dynamic_support_known = false;
  bool dynamic_declined = false;
  bool d3d12 = false;
};
[[nodiscard]] LiveState ReadLiveState();

}  // namespace uplift::addon::slfg
