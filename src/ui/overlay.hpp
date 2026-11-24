#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "text/before_fg_messages.hpp"
#include "ui/controls_coalescer.hpp"
#include "ui/settings.hpp"
#include "ui/status_text.hpp"
#include "sources/motion_source.hpp"

namespace uplift::ui {

// What the panel shows besides the settings.
struct OverlayView {
  std::string status_line;
  // The DLSS / Reflex tab's live block: what the runtime actually holds, as opposed to the choice that asks for
  // it (ui/status_text.hpp's FormatXLive). Empty lines are left out.
  std::string dlss_live_line;       // the mode, its preset, and a driver override that wins over it
  std::string rr_live_line;         // Ray Reconstruction's preset, when the game runs RR
  std::string fg_live_line;         // the frame-generation model preset, and whether it landed live
  std::string fg_multiplier_line;   // the accepted count, or Dynamic MFG's ownership of it
  std::string dynamic_mfg_line;     // in force, or which gate holds it back
  std::string reflex_live_line;     // the mode and cap the driver accepted
  std::string xmx_live_line;        // the XMX mode: XeFG generating, hooked, or why it cannot start
  std::string frame_rate_line;      // FormatFrameRateLine: real and generated frames per second; empty until measured
  // The game's DLSS sizes, for the live line's "1280x720 -> 2560x1440", and its quality mode.
  nr::Size dlss_render;
  nr::Size dlss_output;
  std::string dlss_mode_name;
  int dlss_preset_mode = -1;
  std::array<nr::RecordedModel, MAX_PASSES> pass_models{};
  std::string details_backend, details_note;
  void AddBeforeFgNote(const text::BeforeFgMessage& note) {
    if (details_note.empty() && note.Severity() == text::BeforeFgSeverity::UNAVAILABLE) details_note = note.Text();
  }
  color::ColorSpace output_space = color::ColorSpace::UNKNOWN;
  nr::Size output_size;
  nr::Size first_stage_input, first_stage_canvas;
  bool lumenite_motion = false;
  struct Activity { uint64_t real = 0, reshade = 0, passes = 0; };
  std::optional<Activity> activity;
  bool legacy_runtime = false;
  uint64_t intermediate_bytes = 0u;
  std::optional<uint64_t> runtime_bytes;  // the NR runtime's own allocation, when its stats report it
  float effective_diffuse_white_nits = 0.f;
  std::optional<color::Encoding> encoding;  // the Present path's, resolved (ContextStatus::encoding); nullopt before a frame, when unsupported, and in 32-bit games
  uint32_t last_key_pressed = 0u;         // effect_runtime::last_key_pressed, for hotkey capture
  // The lines the Details header shows after the status line, also promoted into the status card while it is
  // working (its fixed height has spare lines then). Each is the engine's or the bridge's own text.
  struct SafetyLock {
    std::string_view feature;
    std::string reason;
    bool Settings::* setting = nullptr;  // For persistent latches, the existing setting to restore at next startup.
    bool restore_value = true;
    bool retry_now = false;
  };
  std::vector<SafetyLock> safety_locks;
  void AddStartupLocks(const Settings& settings, bool vulkan, bool d3d9);
  struct StageRun {
    uint32_t first = 0, count = 0;
    nr::Size resolution;
    sources::MotionSource motion = sources::MotionSource::NONE;
    uint32_t planned = 0;
    std::string skip;
    std::optional<double> gpu_ms;  // NR evaluation alone: the GPU window between the encode's end and the passes' end
    // The whole NR chain for the same recording: copy/remap, encode and exposure meter, evaluation, compose/Look and
    // decode. The panel shows this one; gpu_ms stays the evaluation alone (the TRACE line's "evaluate").
    std::optional<double> gpu_total_ms;
  };
  std::array<StageRun, 4> stage_runs{};  // Pre-DLSS, After DLSS, Before FG, On Present. Only passes actually evaluated.
  [[nodiscard]] const char* RecordedStageName(uint32_t pass) const {
    for (size_t i = 0; i < stage_runs.size(); ++i) {
      const auto& group = stage_runs[i];
      if (pass >= group.first && pass - group.first < group.count) return NR_STAGE_NAMES[i];
    }
    return nullptr;
  }
  [[nodiscard]] const char* SharedPresentName(const Settings& settings) const {
    if (stage_runs[2].count) return NR_STAGE_NAMES[2];
    if (stage_runs[3].count) return NR_STAGE_NAMES[3];
    return StageLabel(SourcePick::PRESENT, settings.fg_present == Settings::FgPresent::BEFORE && before_fg_unavailable.empty());
  }
  std::string before_fg_unavailable;
  std::string frame_generation_value;    // FormatFrameGenerationValue; empty before a context exists and in 32-bit games
  bool frame_generation_active = false;  // frame generation is running: the FG tab reads it to offer Present's placements
  bool dlss_latched = false;              // DlssPlacementBlocked: the device-removal latch is set (or, in a Vulkan game, VulkanNativeNr = 0)
  std::string ui_correction_note;  // why UI correction does nothing on this placement
  std::string keep_faces_unavailable;  // Keep faces: non-empty greys its rows, and says why (ui::KeepFacesUnavailable)
  StatusCard card;  // replaces the message line at the top
  std::string mask_note;       // "NR mask: UPLIFT_MASK (1920x1080)", or why not
  std::string frame_generation_warning;  // shown under Pass count when it applies; empty otherwise
  // Why no DLSS placement can run this session (bridged, hooks, ReShade version, the
  // latch); empty when they can. Greys Game state after NR (the NR stage and Motion vectors toggles read it through Setup).
  std::string dlss_unavailable;
  // A Vulkan device, where Auto stays at Present and Before upscaling and After DLSS are explicit choices
  // (SourcePickOf's `explicit_dlss`); their tips name the one-time cost.
  bool dlss_explicit = false;
  bool cross_queue_passes_available = false;
  // The Direct3D 9Ex row. `d3d9ex_unavailable` non-empty greys it ("Only for 32-bit Direct3D 9
  // games" in gitc-uplift.addon32, "Only for Direct3D 9 games" in gitc-uplift.addon64) and is what shows under it; otherwise
  // `d3d9ex_readout` ("Now: ...") does.
  std::string d3d9ex_readout;
  std::string d3d9ex_unavailable;
  // The VulkanNr row. `vulkan_nr_unavailable` non-empty greys the whole row (not a 64-bit Vulkan game);
  // `vulkan_native_unavailable` greys Native alone, with why native NR cannot run on this device; `vulkan_nr_running` highlights the route that runs (the
  // stored choice stays as it is and returns by itself), and `vulkan_nr_note` says why it differs from the choice ("Now: ...").
  // `vulkan_d3d12_unavailable` greys Direct3D 12 alone the same way (Helper is never greyed: it is the chain's end).
  std::string vulkan_nr_unavailable;
  std::string vulkan_native_unavailable;
  std::string vulkan_d3d12_unavailable;
  std::optional<VulkanNrMode> vulkan_nr_running;
  std::string vulkan_nr_note;
  // On a 64-bit Vulkan device's helper view, why After DLSS and Before upscaling are greyed (the helper front's Setup
  // greys only them with it); empty elsewhere.
  std::string vulkan_stages_off;
  // Setup (every option's state, the three highlights, the Why) and the working card's rows, from FinishSetup. `setup`'s views point
  // into this view's own strings (and static text).
  SetupView setup;
  LiveCard live;
};

// Panel state kept across frames.
struct OverlayState {
  DragState drag;
  bool language_changed = false;
  OverlayView::Activity activity_start{}, activity_delta{};
  std::chrono::steady_clock::time_point activity_since{};
  double activity_seconds = 0.0;
  Settings::FgPresent activity_mode = Settings::FgPresent::OFF;
  bool capturing_key = false;
  // Set on the frame a new hotkey is captured. That key press is still "pressed" in the same
  // frame's input snapshot, so the hotkey handler must skip it once instead of toggling Enable.
  bool key_captured_this_frame = false;
  // 1.1.0: when the panel last showed the capture; a capture not shown for a moment (the panel closed, another tab) ends (ExpireKeyCapture).
  std::chrono::steady_clock::time_point capture_seen;
  std::string_view enable_warning_key;  // requested checkbox confirmation; ImGui owns the open modal
  bool confirm_present_stage = false;  // The diagram changes both Source and FG placement when the warning is accepted.
  bool confirm_restore = false;  // requested Restore all defaults modal, cleared on dismissal
  bool retry_now = false;        // the add-on calls DeviceContext::RetryNow after drawing the panel and clears it
  // An APPLY_ON_RELEASE slider being held: the value it shows. The setting keeps the applied one until release.
  std::optional<int> pending_release;
  std::string_view pending_key;  // the held row's key (schema keys are static)
  // The Setup highlights' settle, for the device they belong to (a change of device starts it afresh).
  SetupSettle setup_settle;
  const void* setup_device = nullptr;

  // The panel's own state (2026-11-23). Session-only: nothing here is persisted, and ReShade's own close resets it.
  int panel_tab = 1;         // 0 General, 1 Look, 2 Layers, 3 Fixes, 4 DLSS / Reflex, 5 Advanced
  bool panel_open = false;   // set by the panel hotkey, cleared by the cross, Escape or ReShade's own panel opening
  // True while ReShade's own settings window is up (reshade_open_overlay). The two windows exclude each other: this
  // panel is not drawn while it is true, and neither opening nor closing ReShade's window ever opens this one -- only
  // the panel hotkey does.
  bool reshade_overlay_open = false;
  bool panel_drew = false;   // set by DrawOverlayWindow when the window actually drew; input is blocked only then
  float panel_x = -1.f;      // where the window was last dragged; negative means the default corner
  float panel_y = -1.f;
  // The panel's transitions, all of them session-only: negative means "start it on the next draw".
  double panel_intro_started = -1.0;          // when the panel appeared, for the opening slide and fade
  double panel_tab_transition_started = -1.0;  // when the shown tab changed; negative means no transition in flight
  int panel_animation_tab = -1;                // the tab the last draw showed, to notice a change made by any input
  bool capturing_panel_key = false;
  // Last row inspected in this tab. Retained through row gaps and while using the right pane.
  std::string panel_focus = "Source";
  std::string panel_hovered_row;
  uint32_t panel_nav_choice = 0;
  uint32_t edited_pass = 2u;  // selected additional pass in Look
};

// Resolves Setup from `facts` (the shown device's), settles its highlights in `state` and, for a working card, builds the card's rows. Sets
// `view->setup` and `view->live`; `view->card` must be set already. `device`: the shown device's identity. `facts`' views must outlive the draw.
void FinishSetup(OverlayView* view, OverlayState* state, const void* device, const SetupFacts& facts, std::chrono::steady_clock::time_point now);

// Draws the standalone panel while OverlayState::panel_open is set. Returns true when `settings`
// changed, so the caller can sanitise, save and apply.
bool DrawOverlayWindow(const OverlayView& view, Settings* settings, OverlayState* state);

// Closes the standalone panel. Every path (the header's cross, Escape, the panel hotkey) goes through this, so a closed
// panel keeps nothing of its session: the cursor, the intro and the tab transition start over with the next open.
void ClosePanel(OverlayState* state);

// Opens the standalone panel from the state ClosePanel leaves, so the opening slide and fade replay and the tab it opens
// on arrives without a tab transition, whichever path closed it last. The add-on's hotkey uses this; ReShade's own
// reshade_open_overlay handler resets the panel too, so an open never depends on how the previous one ended.
void OpenPanel(OverlayState* state);

// 1.1.0: a hotkey capture the overlay has not shown for a moment (Esc closed ReShade's overlay, or another tab is open) ends, keeping the old hotkey, so
// the hotkey (which a capture holds off) works again. Called at each present.
inline void ExpireKeyCapture(OverlayState* state, std::chrono::steady_clock::time_point now) {
  if (now - state->capture_seen <= std::chrono::milliseconds(250)) return;
  state->capturing_key = false;
  state->capturing_panel_key = false;
}

}  // namespace uplift::ui
