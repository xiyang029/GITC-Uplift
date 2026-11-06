#include "ui/overlay.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "addon/reshade_api.hpp"
#include "ui/settings_schema.hpp"
#include "sources/work_size.hpp"
#include "ui/overlay_widgets.hpp"

namespace uplift::ui {
namespace {

// Plan 14 (R90): the tooltip line of a highlighted option while the highlight is not the stored preference (what runs differs from what ReShade.ini holds).
std::string SavedNote(std::string_view preference) {
  return std::format("Saved: {}; it runs again by itself when it can.", preference);
}

bool DrawReset(const SettingDescriptor& descriptor, Settings& settings, OverlayState& state, const SetupView& setup) {
  if (descriptor.section == SettingSection::TOP || IsDefault(settings, descriptor)) return false;
  // The row's end slot as RowLabel measured it: an undo arrow in the secondary colour, the accent while the
  // pointer is over its cell, so a panel with many changed rows does not read as a column of arrows.
  const std::string default_text = descriptor.key == "MotionVectors" ? "DLSS" : FormatSettingValue(descriptor, DefaultSettings());
  const bool reset = widgets::ResetButton(descriptor.key.data(), default_text.c_str());
  if (reset) {
    ResetSetting(&settings, descriptor);
    if (descriptor.key == "ResolutionMode") state.setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(DefaultSettings().resolution_mode), setup);
    if (descriptor.key == "MotionVectors") state.setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPick::DLSS), setup, true);
  }
  return reset;
}

// Draw a settings-schema row with its normal availability, reset and commit rules.
bool DrawRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state, bool grouped = false) {
  if (descriptor.key == "BeforeFGMotion" && (settings->fg_present != Settings::FgPresent::BEFORE
      || (settings->motion_vectors != MotionVectorSource::LAUNCHPAD && settings->motion_vectors != MotionVectorSource::LUMENITE))) return false;
  std::string label(descriptor.label);
  if (descriptor.key == "ResolutionPresent") label = std::format("Resolution at {}", view.SharedPresentName(*settings));
  if (descriptor.key == "ResolutionScalePresent") label = std::format("Custom scale at {}", view.SharedPresentName(*settings));
  // Plan 6 (D6): with Compare = Built-in, a look-group row shows its built-in value, read-only: edits never reach `settings`.
  const bool read_only = (state->defaults_view && InDefaultsView(descriptor));
  Settings built_in;
  if (read_only) {
    built_in = DefaultsView(*settings);
    settings = &built_in;
  }
  // Not `follows`: the SkinStructure branch below declares one (C4456).
  const bool pass_follows = (descriptor.pass != 0u && (descriptor.flags & setting_flags::PASS_SWITCH) == 0u
                             && settings->passes[descriptor.pass - 2u].follow_pass1);
  const bool shaping_off = ((descriptor.section == SettingSection::RESULT || descriptor.section == SettingSection::LIMITS)
                            && descriptor.key != "ShapeResult" && !settings->look.enabled);
  const bool stabilize_off = ((descriptor.key == "StabilizeMs" || descriptor.key == "StabilizeDetail")
                              && settings->look.stabilize == look::StabilizeMode::OFF);
  // Plan 17: the Adaptation rows only where the meter can be used; 2026-10-09: Auto's own rows only under Auto.
  const bool exposure_hidden = ExposureRowHidden(descriptor, *settings);
  // Split chain (Task 3): a later stage group's Custom scale as Setup's, and Upsampling also while a later group works below Full.
  const bool resolution_scale_hidden =
      ((descriptor.key == "ResolutionScale" && settings->resolution_mode != ResolutionMode::CUSTOM)
       || (descriptor.key == "ResolutionScaleAfterDlss" && settings->resolution_mode_after_dlss != ResolutionMode::CUSTOM)
       || (descriptor.key == "ResolutionScalePresent" && settings->resolution_mode_present != ResolutionMode::CUSTOM));
  const auto reduced = [](ResolutionMode mode, float percent) {
    return mode != ResolutionMode::FULL && (mode != ResolutionMode::CUSTOM || percent < 100.f);
  };
  const auto later_group_scaled = [&](const PassStageRow& row) {
    return row.starts_group && ((row.runs == SourcePick::AFTER_DLSS && reduced(settings->resolution_mode_after_dlss, settings->resolution_scale_after_dlss))
                                || (row.runs == SourcePick::PRESENT && reduced(settings->resolution_mode_present, settings->resolution_scale_present)));
  };
  const bool reduced_any = reduced(settings->resolution_mode, settings->resolution_scale)
                           || std::ranges::any_of(view.setup.pass_stages, later_group_scaled);
  const bool upsampling_hidden = (descriptor.key == "Upsampling" && !reduced_any);
  // Edge-aware's own rows matter only while Edge-aware itself does (below Full); Classic has no bands.
  const bool edge_aware_off = (!reduced_any || settings->upsampling != color::Upsampling::EDGE_AWARE);
  const bool guided_gains_hidden = (descriptor.key == "GuidedBaseGain" && edge_aware_off);
  // Keep faces' own rows, and (fix round 1 addendum) its mask's tuning in Advanced, only while it is on.
  const bool keep_faces_tuning = descriptor.key.starts_with("FaceMask") || descriptor.key.starts_with("FaceFill") || descriptor.key == "FaceEdgeFalloff"
                                 || descriptor.key == "FaceLightingDensity" || descriptor.key == "FaceSpeckSize";
  // Split chain (Task 3, decision Q5a): a later stage's Keep faces here too.
  const bool keep_faces_here = (descriptor.key == "KeepFacesAfterDlss" || descriptor.key == "KeepFacesPresent");
  const bool keep_faces_off =
      ((descriptor.key == "FaceProtection" || descriptor.key == "LightingScale" || keep_faces_tuning || keep_faces_here) && !settings->keep_faces);
  // Round 5: the fill's radius and tolerance only while it is on.
  const bool fill_off = ((descriptor.key == "FaceFillRadius" || descriptor.key == "FaceFillTolerance") && !settings->face_tuning.fill_skin);
  // ui-review.md §3 rule 1: these rows matter only after a choice in the row just above them, so they
  // are hidden -- not merely greyed -- until that choice is made.
  if (pass_follows || shaping_off || stabilize_off || exposure_hidden || resolution_scale_hidden || upsampling_hidden || guided_gains_hidden
      || keep_faces_off || fill_off) {
    return false;
  }

  gui::PushID(label.c_str());
  // The row's note, if it has one, drawn once the reset arrow is placed: RowIconButton centres the arrow on the
  // row's last item, and a note is a line of its own, so drawing it earlier took the arrow down a line on the five
  // rows that have one -- Pass count's frame-generation warning, Mask, UICorrection, UseD3D9Ex and VulkanNr.
  std::string deferred_note;
  bool deferred_note_wraps = false;
  const ImVec2 row_top = gui::GetCursorScreenPos();  // the row's own top: its tooltip is asked for over the whole row
  // ui-review.md §3 rule 1 / §2 finding 5: Game state after NR only matters once NR can run inside the
  // game's frame; while no DLSS placement can run this session, it is greyed and says why.
  // Plan 9: the Direct3D 9Ex row is greyed the same way where it does nothing (every game but a Direct3D 9 one).
  // Plan 19: the VulkanNr row likewise, everywhere but a 64-bit Vulkan game.
  // Keep faces (2026-10-08): its rows where it cannot run, and the face mask's view (in Advanced, far from the switch) while it is off.
  const bool keep_faces_row = (descriptor.key == "KeepFaces" || descriptor.key == "FaceProtection" || descriptor.key == "LightingScale"
                               || descriptor.key == "ShowFaceMask" || keep_faces_tuning || keep_faces_here);
  const std::string_view disabled_reason =
      ((descriptor.flags & setting_flags::NETWORK_PRESET) != 0u && view.legacy_runtime)                   ? std::string_view("NR 310.8 only provides the Legacy network.")
      : (descriptor.key == "MaskBlendMode" && view.legacy_runtime)            ? std::string_view("NR 310.8 does not support native mask blend mode.")
      : (descriptor.key == "StateRestore" && !view.dlss_unavailable.empty())   ? std::string_view(view.dlss_unavailable)
      : (descriptor.key == "UseD3D9Ex" && !view.d3d9ex_unavailable.empty())    ? std::string_view(view.d3d9ex_unavailable)
      : (descriptor.key == "VulkanNr" && !view.vulkan_nr_unavailable.empty()) ? std::string_view(view.vulkan_nr_unavailable)
      : (descriptor.key == "AllowCrossQueuePasses" && !view.cross_queue_passes_available) ? std::string_view("Only for 64-bit Direct3D 12 games")
      : (keep_faces_row && !view.keep_faces_unavailable.empty())               ? std::string_view(view.keep_faces_unavailable)
      : (descriptor.key == "ShowFaceMask" && !settings->keep_faces)            ? std::string_view("Turn on Keep faces (Look) first")
                                                                               : std::string_view();
  gui::BeginDisabled(!disabled_reason.empty() || read_only);
  bool changed = false;
  if (descriptor.key == "SkinStructure") {
    bool follows = (settings->skin_structure < 0.f);
    widgets::RowLabel("Skin follows Structure");
    if (widgets::Switch("##SkinFollows", &follows, /*at_row_end=*/true)) {
      settings->skin_structure = (follows ? SKIN_SAME_AS_STRUCTURE : settings->local_structure);
      changed = true;
    }
  }
  if (descriptor.key == "DiffuseWhiteNits") {
    bool automatic = (settings->diffuse_white_nits <= 0.f);
    widgets::RowLabel("Automatic diffuse white");
    if (widgets::Switch("##AutoDiffuseWhite", &automatic, /*at_row_end=*/true)) {
      settings->diffuse_white_nits = (automatic ? 0.f : std::clamp(view.effective_diffuse_white_nits, 48.f, 500.f));
      changed = true;
    }
  }
  const bool captioned = grouped && descriptor.kind != SettingKind::BOOL;
  if (captioned) {
    const char* caption = descriptor.key == "ResolutionMode" ? "Working resolution" : descriptor.key == "ResolutionScale" ? "Custom scale" : label.c_str();
    // Two rows (Skin follows Structure, Automatic diffuse white) drew their own label and switch just above:
    // their control goes on the line under it, from the row's control column to its end slot, and still names
    // itself there -- the switch's words say "follows", the number below has to say what it is.
    // Nothing is disabled around that label: the only row that can be inherited is SkinStructure, whose own
    // switch already carried the disable when it was drawn above.
    widgets::RowLabel(caption);
    label = "##value";
  }
  std::string saved_note;  // Plan 14 (R90): the Resolution combo's tooltip, while the mode that runs is not the stored one
  if (grouped && descriptor.key == "MaskBlendMode") {
    static constexpr std::array<const char*, 4> modes{"0", "1", "2", "3"};
    const std::array<std::string_view, 4> why{disabled_reason, disabled_reason, disabled_reason, disabled_reason};
    const std::array<std::string_view, 4> tips{descriptor.tooltip, descriptor.tooltip, descriptor.tooltip, descriptor.tooltip};
    const int clicked = widgets::Segmented("##MaskBlendMode", modes.data(), 4, static_cast<int>(settings->mask_blend_mode), why.data(), tips.data());
    if (clicked >= 0) {
      descriptor.set(*settings, clicked);
      changed = true;
    }
  } else if (descriptor.key == "SkinStructure") {
    const bool follows = settings->skin_structure < 0.f;
    gui::BeginDisabled(follows);
    float skin = (follows ? settings->local_structure : settings->skin_structure);
    const bool edited = widgets::Slider(label.c_str(), &skin, static_cast<float>(descriptor.min),
                                        static_cast<float>(SliderMax(descriptor)), "%.2f", &state->drag.skin_structure);
    if (edited) {
      settings->skin_structure = skin;
      changed = true;
    }
    // The Slider already reported its held state through &state->drag.skin_structure; re-reading
    // gui::IsItemActive() here asked its trailing value label instead and always said "not held", which
    // dropped the drag coalescing (the row then re-applied, and reset NR's history, every frame).
    gui::EndDisabled();
  } else if (descriptor.key == "DiffuseWhiteNits") {
    // ui-review.md §3 rule 2 (the Skin pattern): an Automatic checkbox, the control greyed at the value
    // in use -- shown, not hidden, so the effective value always reads even while it is automatic.
    const bool automatic = settings->diffuse_white_nits <= 0.f;
    gui::BeginDisabled(automatic);
    float nits = (automatic ? view.effective_diffuse_white_nits : settings->diffuse_white_nits);
    if (widgets::Slider(label.c_str(), &nits, 48.f, 500.f, "%.0f", nullptr) && !automatic) {
      settings->diffuse_white_nits = nits;
      changed = true;
    }
    gui::EndDisabled();
  } else {
    switch (descriptor.kind) {
      case SettingKind::BOOL: {
        bool value = (descriptor.get(*settings) != 0.0);
        // The top row (Enable) carries the hotkey button and the language combo after its switch, so its switch
        // sits right after the label instead of at the row's end; every other BOOL row puts it at the end.
        const bool top_row = (descriptor.section == SettingSection::TOP);
        if (top_row) {
          widgets::InlineLabel(descriptor.label.data());
        } else {
          widgets::RowLabel(descriptor.label.data());
        }
        if (widgets::Switch(label.c_str(), &value, /*at_row_end=*/!top_row)) {
          if (value && !descriptor.enable_warning.empty()) {
            state->enable_warning_key = descriptor.key;
          } else {
            descriptor.set(*settings, (value ? 1.0 : 0.0));
            changed = true;
          }
        }
        break;
      }
      case SettingKind::UINT: {
        const int applied = static_cast<int>(descriptor.get(*settings));
        const bool on_release = ((descriptor.flags & setting_flags::APPLY_ON_RELEASE) != 0u);
        const bool held_here = (on_release && state->pending_release.has_value() && state->pending_key == descriptor.key);
        int value = (held_here ? *state->pending_release : applied);
        const int low = static_cast<int>(descriptor.min), high = static_cast<int>(descriptor.max);
        bool active = false;  // the control's own held state: APPLY_ON_RELEASE waits for the release, not for every step
        const bool typed = descriptor.max > 100.0;  // past a slider's range: a typed field, whose own held state is the release below
        // The render scales are shares of the output, so their number carries the sign: the label names the row
        // ("Quality render scale"), the value says what it is in.
        const char* const suffix = IntPercentSetting(descriptor.key) ? " %" : "";
        const bool edited = typed ? widgets::NumberField(label.c_str(), &value, 64, 512, &active)
                                  : widgets::SliderInt(label.c_str(), &value, low, high, &active, suffix);
        if (!on_release) {
          if (edited) {
            descriptor.set(*settings, std::clamp(static_cast<double>(value), descriptor.min, descriptor.max));
            changed = true;
          }
        } else {
          // Pass Count: while held the slider shows the value under the mouse; the setting, NR's features and the
          // Pass 2+ rows below change once, on release (also after a Ctrl+click typed value is confirmed).
          if (edited) {
            state->pending_release = value;
            state->pending_key = descriptor.key;
          }
          // The release is read from the control's own held state, never gui::IsItemDeactivated[AfterEdit]: the
          // row's widgets draw their rail and value after ImGui's own control, so the last item is a label with no
          // ID and those flags never fire. That silently dropped the setting of every APPLY_ON_RELEASE row (Pass
          // count: the slider showed 2..10, settings.pass_count stayed 1, so NR never ran the extra passes).
          if (!active && state->pending_key == descriptor.key && state->pending_release) {
            const int released = *std::exchange(state->pending_release, std::nullopt);
            if (released != applied) {
              descriptor.set(*settings, std::clamp(static_cast<double>(released), descriptor.min, descriptor.max));
              changed = true;
            }
          }
        }
        if (descriptor.key == "PassCount" && !view.frame_generation_warning.empty()) {
          deferred_note = view.frame_generation_warning;
          deferred_note_wraps = true;
        }
        break;
      }
      case SettingKind::FLOAT: {
        // 2026-10-09: AutoExposureBlend is stored 0..1 and shown as 0-100 %.
        const float shown_scale = (descriptor.key == "AutoExposureBlend" ? 100.f : 1.f);
        float value = static_cast<float>(descriptor.get(*settings)) * shown_scale;
        const float high = static_cast<float>(SliderMax(descriptor)) * shown_scale;  // Ctrl+click may type past it (DescriptorMax)
        const char* const format = (descriptor.key == "GraceSeconds" ? "%.1f"
                                    : FloatPercentSetting(descriptor.key)   ? "%.0f %%"
                                    : descriptor.key == "AutoExposureBlend" ? "%.0f %%"
                                    : descriptor.key == "StabilizeMs"       ? "%.0f ms"
                                                                            : "%.2f");
        bool active = false;  // the slider's own held state, for the rows that gate work on the release
        const bool edited =
            widgets::Slider(label.c_str(), &value, static_cast<float>(descriptor.min) * shown_scale, high, format, &active, shown_scale);
        if (edited) {
          descriptor.set(*settings, static_cast<double>(value / shown_scale));
          changed = true;
        }
        if (descriptor.key == "LocalStructure") {
          state->drag.local_structure = active;
        } else if (descriptor.key == "LocalTone") {
          state->drag.local_tone = active;
        } else if (descriptor.key == "ResolutionScale") {
          state->drag.resolution_scale = active;
        } else if (descriptor.key == "ResolutionScaleAfterDlss") {
          state->drag.resolution_scale_after_dlss = active;  // split chain (Task 3): the work size waits for the release, as Setup's
        } else if (descriptor.key == "ResolutionScalePresent") {
          state->drag.resolution_scale_present = active;
        } else if (descriptor.key == "FaceProtection") {
          state->drag.face_protection = active;
        }
        if (descriptor.pass != 0u && (descriptor.flags & setting_flags::RESTARTS_HISTORY) != 0u) {
          state->drag.pass_slider |= active;
        }
        break;
      }
      case SettingKind::CHOICE: {
        int value = static_cast<int>(descriptor.get(*settings));
        if ((descriptor.flags & setting_flags::NETWORK_PRESET) != 0u && view.legacy_runtime) value = 1;
        if (descriptor.choices.size() <= 3u) {
          // User decision: at most three short choices draws as the RadioRow segmented control above,
          // never a Combo (Model, Upsampling, Stabilise, NR mask, UI correction and the like).
          std::array<const char*, 3> labels = {};
          std::array<std::string_view, 3> why = {};
          if (!disabled_reason.empty()) {
            why.fill(disabled_reason);  // e.g. Game state after NR, greyed while no DLSS placement can run
          }
          if (descriptor.key == "PresentWithFrameGen") why[static_cast<size_t>(Settings::FgPresent::BEFORE)] = view.before_fg_unavailable;
          if (descriptor.key == "VulkanNr" && disabled_reason.empty()) {
            // Plan 19 (the owner's UI rules): Native greyed where it cannot run, with why; the highlight is the route that runs. T5: Direct3D 12 too.
            why[static_cast<size_t>(VulkanNrMode::NATIVE)] = view.vulkan_native_unavailable;
            why[static_cast<size_t>(VulkanNrMode::DIRECT3D_12)] = view.vulkan_d3d12_unavailable;
            if (view.vulkan_nr_running) {
              value = static_cast<int>(*view.vulkan_nr_running);
            }
          }
          std::array<std::string_view, 3> tips = {};
          tips.fill(descriptor.tooltip);
          for (size_t index = 0u; index < descriptor.choices.size(); ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          const std::span<const char* const> options(labels.data(), descriptor.choices.size());
          const std::span<const std::string_view> why_span(why.data(), descriptor.choices.size());
          const std::span<const std::string_view> tips_span(tips.data(), descriptor.choices.size());
          // Shown in the order the schema declares its choices (FG_PRESENT_MODES); the stored values are the ones presets use.
          if (const int clicked = widgets::Segmented(label.c_str(), options.data(), static_cast<int>(options.size()), value,
                                                     why_span.data(), tips_span.data()); clicked >= 0) {
            if (descriptor.key == "PresentWithFrameGen" && clicked == static_cast<int>(Settings::FgPresent::AFTER)) {
              state->confirm_present_stage = false;
              state->enable_warning_key = descriptor.key;
            } else {
              descriptor.set(*settings, static_cast<double>(clicked));
              changed = true;
            }
          }
        } else if (descriptor.key == "ResolutionMode" || descriptor.key == "ResolutionAfterDlss" || descriptor.key == "ResolutionPresent") {
          // Plan 14 (the user's decision 1): the entries that cannot run now are greyed, each with its reason, and the preview is the mode that runs.
          // Split chain (Task 3): a later stage group's Resolution greys the same entries (Match game reads the same DLSS render size); its preview is its
          // stored mode, since Setup's highlight is pass 1's.
          const bool setup_row = (descriptor.key == "ResolutionMode");
          std::array<const char*, 6> labels = {};
          for (size_t index = 0u; index < labels.size(); ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          const size_t stored_index = std::min(static_cast<size_t>(value), labels.size() - 1u);
          const size_t shown = (setup_row ? static_cast<size_t>(view.setup.resolution) : stored_index);
          const OptionState& stored = view.setup.resolution_options[stored_index];
          if (shown != stored_index && (stored.why.empty() || stored.temporary)) {  // a fixed cause never clears: no promise
            saved_note = SavedNote(labels[stored_index]);
          }
          // Fix round 1 (M5): Setup's row greys every entry that cannot run, its highlight included (as before the
          // split chain); a later stage's row keeps its stored mode selectable, since its preview is that mode and
          // not what runs.
          std::array<std::string_view, 6> why = {};
          for (size_t index = 0u; index < labels.size(); ++index) {
            const OptionState& option = view.setup.resolution_options[index];
            if (!option.why.empty() && (setup_row || index != shown)) why[index] = option.why;
          }
          if (const int picked = widgets::Combo("resolution", labels.data(), static_cast<int>(labels.size()),
                                                static_cast<int>(shown), why.data()); picked >= 0) {
            descriptor.set(*settings, static_cast<double>(picked));
            if (setup_row) {
              state->setup_settle.Clicked(SetupRow::RESOLUTION, static_cast<uint32_t>(picked));
            }
            changed = true;
          }
        } else {
          // Eight short choices fit a row: a longer set would lose its tail here (the longest today, QUALITY_MODES, has seven).
          std::array<const char*, 8> labels = {};
          const size_t count = std::min(descriptor.choices.size(), labels.size());
          for (size_t index = 0u; index < count; ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          if (const int clicked = widgets::Segmented(label.c_str(), labels.data(), static_cast<int>(count), value);
              clicked >= 0 && clicked != value) {
            descriptor.set(*settings, static_cast<double>(clicked));
            changed = true;
          }
        }
        break;
      }
      case SettingKind::TEXT: {
        std::string text(descriptor.get_text(*settings));
        if (widgets::TextField(label.c_str(), &text)) {
          descriptor.set_text(*settings, text);
          changed = true;
        }
        break;
      }
      case SettingKind::KEY: break;  // the Enable hotkey is drawn beside Enable
    }
  }
  // The row answers a hover over the whole of itself, never through gui::IsItemHovered: the row's own widgets
  // draw their rail and their value after ImGui's own control (overlay_widgets.hpp:292/355), and a segment group
  // its surfaces after its last InvisibleButton, so the last item by now is a label with no ID. Asking it left a
  // slider answering only over its number and a 2-3 choice row only over its last segment. The end slot is the
  // reset arrow's own cell, which carries its own tooltip and must not collect this one as well.
  const float row_bottom = gui::GetCursorScreenPos().y - gui::GetStyle().ItemSpacing.y;
  const float row_right = (std::min)(widgets::row_slot_x, gui::GetWindowPos().x + gui::GetWindowSize().x);
  const bool over_row = gui::IsWindowHovered()
                        && gui::IsMouseHoveringRect(ImVec2(gui::GetWindowPos().x, row_top.y), ImVec2(row_right, row_bottom));
  // Unless a control inside the row already drew one: a segment group explains its own segments as it draws
  // them (overlay_widgets.hpp's SegmentTip), and ImGui opens a window per BeginTooltip call, so asking here
  // too showed the row's text twice, one bubble behind the other.
  if (over_row && !widgets::TooltipDrawnThisFrame() && (!descriptor.tooltip.empty() || !disabled_reason.empty())) {
    std::string tooltip;
    if (!disabled_reason.empty()) {
      tooltip = std::format("Greyed: {}", disabled_reason);
      if (!descriptor.tooltip.empty()) {
        tooltip += "\n";
      }
    }
    tooltip += descriptor.tooltip;
    if (!saved_note.empty()) {
      tooltip += "\n" + saved_note;
    }
    widgets::MouseTooltip("%s", tooltip.c_str());
  }
  if (descriptor.key == "Mask") {
    deferred_note = view.mask_note;
  }
  if (descriptor.key == "UICorrection") {
    deferred_note = view.ui_correction_note;
  }
  if (descriptor.key == "UseD3D9Ex") {
    deferred_note = (view.d3d9ex_unavailable.empty() ? view.d3d9ex_readout : view.d3d9ex_unavailable);
  }
  if (descriptor.key == "VulkanNr" && view.vulkan_nr_unavailable.empty()) {
    deferred_note = view.vulkan_nr_note;
  }
  // The reset arrow here, on the control's own line: RowLabel reserved its slot before the control was drawn, so
  // drawing it earlier moved the cursor to the row's end and the control itself wrapped to a new line. It has to
  // come before the note above all, or the note becomes the row's last item and the arrow drops a line below it.
  changed |= DrawReset(descriptor, *settings, *state, view.setup);
  if (!deferred_note.empty()) {
    if (deferred_note_wraps) gui::PushTextWrapPos(0.f);
    widgets::SecondaryText(deferred_note.c_str());
    if (deferred_note_wraps) gui::PopTextWrapPos();
  }
  gui::EndDisabled();
  gui::PopID();
  return changed && !read_only;
}

bool DrawEnableWarning(const SettingDescriptor& descriptor, Settings* settings, OverlayState* state, bool explicit_dlss) {
  // The name ImGui shows, then the id it keeps: the dialog stays open by the key, so two rows cannot collide.
  const std::string title = std::format("{} - performance warning###enable_warning_{}", descriptor.label, descriptor.key);
  bool open = (state->enable_warning_key == descriptor.key);
  if (!open) return false;
  const bool enable = widgets::ConfirmDialog(
      title.c_str(), descriptor.enable_warning.data(),
      descriptor.key == "PresentWithFrameGen" ? "Keep current mode" : "Keep disabled", "Enable anyway", &open, 112.f);
  if (!open) state->enable_warning_key = {};  // answered either way: the row's request is spent
  if (enable) {
    descriptor.set(*settings, 1.0);
    if (descriptor.key == "PresentWithFrameGen" && state->confirm_present_stage) {
      SetSourcePick(settings, SourcePick::PRESENT, explicit_dlss);
      state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(SourcePick::PRESENT));
    }
  }
  return enable;
}

// Split chain (Task 3, design 6.2): pass n's Runs at row, as the NR stage row but a combo (four long choices): a choice that cannot run is greyed with why,
// the preview is where the pass runs, and the stored pick is named while it runs elsewhere. Where every pass runs at one stage the whole row is greyed.
// Not folded into DrawRow: its choices' states come from Setup's pass rows, not from the schema.
bool DrawPassStageRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state) {
  const size_t later = descriptor.pass - 2u;
  const PassStageRow& row = view.setup.pass_stages[later];
  const std::string label(descriptor.label);
  gui::PushID(label.c_str());
  gui::BeginDisabled(!view.setup.split_capable || view.setup.stopped);  // fix round 1 (M5): greyed with Setup's other rows once NR stopped (Plan 15)
  const auto stored = static_cast<size_t>(settings->passes[later].stage);
  const auto shown = static_cast<size_t>(row.shown);
  const std::array names{descriptor.choices[0].data(), NR_STAGE_NAMES[0], NR_STAGE_NAMES[1], view.SharedPresentName(*settings)};
  bool changed = false;
  widgets::RowLabel(label.c_str());
  std::array<std::string_view, 4> why = {};
  for (size_t index = 0u; index < row.options.size(); ++index) {
    why[index] = row.options[index].why;
  }
  if (const int picked = widgets::Combo("stage", names.data(), static_cast<int>(names.size()),
                                        static_cast<int>(shown), why.data()); picked >= 0 && static_cast<size_t>(picked) != stored) {
    descriptor.set(*settings, static_cast<double>(picked));
    changed = true;
  }
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    std::string tooltip(descriptor.tooltip);
    if (!view.setup.split_capable) {
      tooltip = std::format("Greyed: {}\n{}", ONE_STAGE_REASON, descriptor.tooltip);
    } else if (shown != stored && (row.options[stored].why.empty() || row.options[stored].temporary)) {  // a fixed cause never clears: no promise
      tooltip += "\n" + SavedNote(names[stored]);
    }
    widgets::ItemTooltip("%s", tooltip.c_str());
  }
  changed |= DrawReset(descriptor, *settings, *state, view.setup);
  gui::EndDisabled();
  gui::PopID();
  return changed;
}

// Plan 14: the Motion vectors row -- Off / DLSS / Launchpad / Lumenite (2026-10-08) -- from Setup, as the NR stage row.
bool DrawMotionRow(const OverlayView& view, Settings* settings, OverlayState* state) {
  gui::PushID("MotionVectors");
  static constexpr std::array<const char*, 4> OPTIONS = {"Off", "DLSS", "Launchpad", "Lumenite"};
  std::array<std::string_view, 4> tips = {
      "No motion vectors, anywhere.",
      "The game's own DLSS motion vectors: bound directly inside the frame, and copied for Present when "
      "NR stage is Present.",
      "iMMERSE Launchpad on, and Uplift.fx's Uplift technique below it. The presented image only.",
      "LumeniteFX's Kernel on, and Uplift.fx's Uplift technique below it. The presented image only. Auto uses Launchpad when both are on.",
  };
  std::array<std::string_view, 4> why = {};
  for (size_t index = 0u; index < why.size(); ++index) {
    why[index] = view.setup.motion_options[index].why;
  }
  const MotionPick preferred = MotionPickOf(*settings);
  const int shown = static_cast<int>(view.setup.motion);
  std::string saved_tip;
  const OptionState& stored_motion = view.setup.motion_options[static_cast<size_t>(preferred)];
  if (view.setup.motion != preferred && (stored_motion.why.empty() || stored_motion.temporary)) {  // a fixed cause never clears: no promise
    const std::string_view stored = (settings->motion_vectors == MotionVectorSource::AUTO ? std::string_view("Auto") : std::string_view(OPTIONS[static_cast<size_t>(preferred)]));
    saved_tip = std::format("{}\n{}", tips[static_cast<size_t>(shown)], SavedNote(stored));
    tips[static_cast<size_t>(shown)] = saved_tip;
  }
  // The row's label in its column, the segments across the control column, the reset arrow in the end slot:
  // the same RowLabel every other row goes through, which is also what gives Segmented its width. A bare
  // TextUnformatted left the row without a SetNextItemWidth, so the segments got CalcItemWidth's default.
  widgets::RowLabel("Motion vectors");
  bool changed = false;
  widgets::NextGap(style::LabelGap());
  if (const int clicked = widgets::Segmented("##MotionVectors", OPTIONS.data(), 4, shown, why.data(), tips.data()); clicked >= 0) {
    SetMotionPick(settings, static_cast<MotionPick>(clicked));
    state->setup_settle.Clicked(SetupRow::MOTION, static_cast<uint32_t>(clicked));
    changed = true;
  }
  changed |= DrawReset(*FindSetting("MotionVectors"), *settings, *state, view.setup);
  gui::PopID();
  return changed;
}

void DrawWorkingSize(const OverlayView& view, const Settings& settings) {
  nr::Size input = view.first_stage_input;
  if (input.Empty() && view.setup.stage == SourcePick::PRESENT) input = view.output_size;
  nr::Size size = view.first_stage_canvas;
  if (settings.resolution_mode == ResolutionMode::CUSTOM && !input.Empty()) {
    // WorkSize scales the target it is given, and `input` is already that target (SetupFacts::frame, what the mode
    // scales), so it belongs in the first two arguments unscaled; scaling it here too made a 50 % Custom read as the
    // NR floor instead of the size that runs.
    const double scale = settings.resolution_scale / 100.0;
    const double scale_y = (settings.resolution_asymmetric ? settings.resolution_scale_y / 100.0 : scale);
    size = sources::WorkSize(input.width, input.height, scale, scale_y, input);
    if (view.setup.stage == SourcePick::BEFORE_UPSCALING) size = sources::CanvasSize(size);
  } else if (size.Empty() && settings.resolution_mode == ResolutionMode::FULL && view.setup.stage == SourcePick::PRESENT) {
    size = input;
  }
  const bool pending = !size.Empty() && !view.first_stage_canvas.Empty() && size != view.first_stage_canvas;
  // 2026-11-06: the size itself is not shown here any more -- the top card's Display output and the stage rows' NR size say what runs.
  gui::TextUnformatted(pending ? "Target NR size" : "NR working size");
}

// The DLSS / Reflex card's sub-captions: the first row of each group names the group. The rows are drawn in
// schema order, so these keys are where a group begins.
constexpr const char* DlssReflexGroup(std::string_view key) {
  if (key == "DLSSPresetDLAA") return "DLSS presets";
  if (key == "DLSSRenderScaleDLAA") return "DLSS render scale";
  if (key == "DLSSQualityMode") return "DLSS create overrides";
  if (key == "DLSSFGPreset") return "Frame generation";
  if (key == "ReflexMode") return "Reflex";
  if (key == "RayReconstructionPreset") return "Ray Reconstruction";
  if (key == "XmxGenerator") return "XMX";
  return nullptr;
}

// The live DLSS, frame-generation and Reflex overrides: the switches players come to the panel for, not the
// diagnostics and tuning that stay in Advanced.
bool DrawDlssReflexSection(const OverlayView& view, Settings* settings, OverlayState* state, bool& changed) {
  const bool section_changed = SectionChanged(*settings, SettingSection::DLSS_REFLEX);
  bool reset = false;
  const bool open = widgets::SectionHeader("DLSS / Reflex", "Presets · Frame generation · Reflex", "Reset",
                                             section_changed && !state->defaults_view, &reset);
  if (reset) {
    ResetSection(settings, SettingSection::DLSS_REFLEX);
    changed = true;
  }
  if (open) widgets::Panel("##dlss_body", [&] {
    const char* group = nullptr;
    for (const SettingDescriptor& descriptor : SettingsSchema()) {
      if (descriptor.section != SettingSection::DLSS_REFLEX || descriptor.kind == SettingKind::KEY) continue;
      if (const char* const next = DlssReflexGroup(descriptor.key); next != nullptr && next != group) {
        if (group != nullptr) widgets::NextGap(style::BlockGap());
        widgets::GroupHeading(next);
        group = next;
      }
      changed |= DrawRow(descriptor, view, settings, state, true);
    }
    // What actually runs, gathered in one place at the bottom: a row says what was asked for, these say what the
    // runtime is doing with it.
    const std::array<const std::string*, 8> live = {&view.dlss_live_line,     &view.rr_live_line,     &view.fg_live_line,
                                                    &view.fg_multiplier_line, &view.dynamic_mfg_line, &view.frame_rate_line,
                                                    &view.reflex_live_line,   &view.xmx_live_line};
    if (std::ranges::any_of(live, [](const std::string* line) { return !line->empty(); })) {
      widgets::NextGap(style::BlockGap());
      widgets::GroupHeading("Currently");
      for (const std::string* line : live) {
        if (!line->empty()) widgets::SecondaryText(line->c_str());
      }
    }
  }, 0);
  return open;
}

bool DrawSetupGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  // Setup's rows sit in a card of their own, as Look's do (2026-11-06): straight on the panel background they
  // read as part of whatever was above them, while Look's card edge said where its own rows began.
  widgets::Panel("##setup_body", [&] {
    // Frame inputs and Passes in the left half, NR processing in the right: upstream's own split, so the two
    // halves of Setup can be read side by side instead of as one long column.
    widgets::PairedGroups("##setup_groups", [&] {
      widgets::GroupHeading("Frame inputs");
      changed |= DrawMotionRow(view, settings, state);
      changed |= DrawRow(*FindSetting("MaskBlendMode"), view, settings, state, true);
      widgets::NextGap(style::BlockGap());
      widgets::GroupHeading("Passes");
      changed |= DrawRow(*FindSetting("PassCount"), view, settings, state, true);
    }, [&] {
      widgets::GroupHeading("NR processing", "Pass 1 defaults");
      changed |= DrawRow(*FindSetting("Preset"), view, settings, state, true);
      changed |= DrawRow(*FindSetting("ResolutionMode"), view, settings, state, true);
      changed |= DrawRow(*FindSetting("ResolutionScale"), view, settings, state, true);
      // Anamorphic (2026-10-29): Custom's two axes apart, under the mode they belong to.
      if (settings->resolution_mode == ResolutionMode::CUSTOM) {
        changed |= DrawRow(*FindSetting("ResolutionAsymmetric"), view, settings, state, true);
        if (settings->resolution_asymmetric) changed |= DrawRow(*FindSetting("ResolutionScaleY"), view, settings, state, true);
      }
      DrawWorkingSize(view, *settings);
      changed |= DrawRow(*FindSetting("Upsampling"), view, settings, state, true);
      // Edge-aware's base band gain, right below the choice it depends on.
      changed |= DrawRow(*FindSetting("GuidedBaseGain"), view, settings, state, true);
      changed |= DrawRow(*FindSetting("Foveation"), view, settings, state, true);
      if (settings->foveation) {
        changed |= DrawRow(*FindSetting("FoveationWidth"), view, settings, state, true);
        changed |= DrawRow(*FindSetting("FoveationHeight"), view, settings, state, true);
        changed |= DrawRow(*FindSetting("FoveationFalloffX"), view, settings, state, true);
        changed |= DrawRow(*FindSetting("FoveationFalloffY"), view, settings, state, true);
        changed |= DrawRow(*FindSetting("FoveationEdge"), view, settings, state, true);
      }
    });
  });
  return changed;
}

bool DrawLookGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  static constexpr std::array<const char*, 2> options{"Mine", "Built-in"};
  static constexpr std::array<std::string_view, 2> why{}, tips{"", "The built-in look, read-only; your own settings are kept."};
  bool changed = false;
  const auto row = [&](const char* key, bool grouped = true) { changed |= DrawRow(*FindSetting(key), view, settings, state, grouped); };
  // The Look rows live in a card of their own, title included (2026-11-06): a bare caption on the panel background
  // read as naming the card above, and a section that ends on a bare row leaves the next collapsible header at a
  // different offset than one that ends on a card. Compare joined them inside it: the section's one row left outside.
  widgets::Panel("##look_body", [&] {
    if (widgets::SectionCaption("Look", "Reset", SectionChanged(*settings, SettingSection::LOOK) && !state->defaults_view)) {
      ResetSection(settings, SettingSection::LOOK);
      changed = true;
    }
    // Compare draws through the same row layout as everything else: its two segments take the control column and
    // the reset slot stays empty (there is no reset for a view switch).
    widgets::RowLabel("Compare");
    {
      gui::PushID("Compare");
      if (const int pick = widgets::Segmented("##Compare", options.data(), 2, state->defaults_view ? 1 : 0, why.data(), tips.data());
          pick >= 0) {
        state->defaults_view = pick == 1;
      }
      gui::PopID();
    }
    widgets::NextGap(style::BlockGap());
    // Image in the left half, Characters in the right, as upstream draws them: the two are read against each
    // other, and one long column of nine rows hides that they are two groups.
    widgets::PairedGroups("##look_groups", [&] {
      widgets::GroupHeading("Image", "Pass 1");
      row("Style");
      row("Intensity");
      row("LocalStructure");
      row("LocalTone");
    }, [&] {
      widgets::GroupHeading("Characters");
      row("AutoMask");
      row("SkinStructure");
      row("KeepFaces");
      row("FaceProtection");
      row("LightingScale");
    });
  });
  return changed;
}

// The VRAM line of the Details header and of the working status card.
std::string VramLine(const OverlayView& view) {
  constexpr double MIB = 1024.0 * 1024.0;
  if (view.runtime_bytes) {
    return std::format("VRAM: NR runtime {:.0f} MiB, Uplift intermediates {:.0f} MiB", static_cast<double>(*view.runtime_bytes) / MIB,
                       static_cast<double>(view.intermediate_bytes) / MIB);
  }
  return std::format("VRAM: Uplift intermediates {:.0f} MiB", static_cast<double>(view.intermediate_bytes) / MIB);
}

}  // namespace

void FinishSetup(OverlayView* view, OverlayState* state, const void* device, const SetupFacts& facts, std::chrono::steady_clock::time_point now) {
  if (state->setup_device != device) {
    state->setup_device = device;
    state->setup_settle.Reset();
    state->activity_since = {};
    state->activity_seconds = 0.0;
  }
  view->first_stage_input = facts.frame;
  view->first_stage_canvas = facts.canvas.Empty() ? facts.work : facts.canvas;
  view->setup = state->setup_settle.Update(ResolveSetup(facts), now);
  if (view->card.working) {
    view->live = BuildLiveCard(facts, view->setup);
  }
}

namespace {
void DrawDetails(const OverlayView& view, const Settings& settings, OverlayState& state) {
  const auto fact = [](const char* key, const std::string& value) {
    gui::TableNextRow();
    gui::TableSetColumnIndex(0); widgets::SecondaryText(gui::Display(key).c_str());
    gui::TableSetColumnIndex(1); widgets::Wrapped("%s", value.c_str());
  };
  widgets::Table("##runtime_facts", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH,
                 {0.f, style::LabelGap() * 0.5f}, [&] {
    gui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, std::max({gui::CalcTextSize("Backend").x, gui::CalcTextSize("Display output").x, gui::CalcTextSize("Frame generation").x}) + style::BlockGap());
    gui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    if (!view.details_backend.empty()) fact("Backend", gui::Display(view.details_backend));
    if (!view.output_size.Empty()) {
      const char* space = "";
      switch (view.output_space) {
        case color::ColorSpace::SRGB_NONLINEAR: space = "sRGB"; break;
        case color::ColorSpace::EXTENDED_SRGB_LINEAR: space = "scRGB"; break;
        case color::ColorSpace::HDR10_ST2084: space = "HDR10"; break;
        case color::ColorSpace::HDR10_HLG: space = "HLG"; break;
        default: break;
      }
      fact("Display output", std::format("{} × {}  {}", view.output_size.width, view.output_size.height, space));
    }
    if (view.frame_generation_active || !view.frame_generation_line.empty())
      fact("Frame generation", gui::Display(view.frame_generation_active ? "On" : "Off"));
  });

  widgets::NextGap(style::BlockGap());
  const bool compact = gui::GetContentRegionAvail().x < style::Units(112);
  bool any = false;
  widgets::Table("##executed_passes", compact ? 2 : 5,
                 ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings,
                 {style::RowGap(), style::LabelGap()}, [&] {
    const auto column = [](const char* label, float weight) { gui::TableSetupColumn(gui::Display(label).c_str(), ImGuiTableColumnFlags_WidthStretch, weight); };
    column("Pass", 0.5f);
    if (compact) column("NR evaluation", 3.f);
    else {
      column("Ran at", 1.15f); column("Network / style", 1.3f); column("NR size", 1.f); column("Motion used", 1.25f);
    }
    if (!compact) gui::TableHeadersRow();
    for (size_t i = 0; i < view.stage_runs.size(); ++i) {
      const auto& group = view.stage_runs[i];
      for (uint32_t n = 0; n < group.count && group.first + n < view.pass_models.size(); ++n) {
        const uint32_t pass = group.first + n;
        const auto& model = view.pass_models[pass];
        const char* network = "Unconfirmed";
        switch (model.network) {
          case nr::RecordedNetwork::LEGACY: network = "Legacy"; break;
          case nr::RecordedNetwork::SMALL_FP8: network = "Small FP8"; break;
          case nr::RecordedNetwork::OTHER: network = "Other network"; break;
          default: break;
        }
        const std::string name = model.style < 3u ? std::format("{} / {}", gui::Display(network), static_cast<char>('A' + model.style)) : gui::Display(network);
        const char* motion = "None";
        switch (group.motion) {
          case sources::MotionSource::DIRECT: motion = "DLSS"; break;
          case sources::MotionSource::PRESENT_COPY: motion = "DLSS copy"; break;
          case sources::MotionSource::LAUNCHPAD: motion = view.lumenite_motion ? "Lumenite" : "Launchpad"; break;
          default: break;
        }
        gui::TableNextRow();
        gui::TableNextColumn(); gui::Text("P%u", pass + 1u);
        const auto record = [compact](const char* label, const std::string& value) {
          if (!compact) gui::TableNextColumn();
          if (compact) widgets::Wrapped("%s  %s", gui::Display(label).c_str(), value.c_str());
          else widgets::Wrapped("%s", value.c_str());
        };
        if (compact) gui::TableNextColumn();
        record("Ran at", gui::Display(NR_STAGE_NAMES[i]));
        record("Network / style", name);
        record("NR size", group.resolution.Empty() ? gui::Display("Unknown") : std::format("{} × {}", group.resolution.width, group.resolution.height));
        record("Motion used", gui::Display(motion));
        any = true;
      }
    }
  });
  if (!any) widgets::SecondaryText("No pass recorded.");
  if (!view.details_note.empty()) widgets::Wrapped("%s", view.details_note.c_str());
  for (const auto& group : view.stage_runs) {
    if (group.skip.empty() || group.planned <= group.count) continue;
    const uint32_t first = group.first + group.count + 1u, last = group.first + group.planned;
    const std::string passes = first == last ? std::format("P{}", first) : std::format("P{}-{}", first, last);
    widgets::Wrapped("%s: %s", passes.c_str(), gui::Display(group.skip).c_str());
  }
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - state.activity_since).count();
  if (view.activity) {
    const auto& v = *view.activity;
    const auto& base = state.activity_start;
    if (state.activity_mode != settings.fg_present || elapsed > 2.5 || v.real < base.real || v.reshade < base.reshade || v.passes < base.passes) {
      state.activity_since = now;
      state.activity_start = v;
      state.activity_seconds = 0.0;
      state.activity_mode = settings.fg_present;
    } else if (elapsed >= 1.0) {
      state.activity_delta = {v.real-base.real, v.reshade-base.reshade, v.passes-base.passes};
      state.activity_seconds = elapsed;
      state.activity_start = v;
      state.activity_since = now;
    }
    if (state.activity_seconds > 0 && state.activity_delta.real > 0) {
      const auto& delta = state.activity_delta;
      const auto line = std::format("{} {:.1f} s: {} {} / {} {} / {} {}", gui::Display("FG activity over"), state.activity_seconds,
          delta.real, gui::Display("real frames"), delta.reshade, gui::Display("ReShade Presents"), delta.passes, gui::Display("Present-pass runs"));
      widgets::Wrapped("%s", line.c_str());
    }
  } else {
    state.activity_since = {};
    state.activity_seconds = 0.0;
  }
  widgets::Wrapped("%s", VramLine(view).c_str());
}
}  // namespace

void OverlayView::AddStartupLocks(const Settings& settings, bool vulkan, bool d3d9) {
  if (d3d9 && (settings.startup_latches & Settings::LATCH_D3D9EX)) {
    safety_locks.push_back({"Direct3D 9Ex", "The previous 9Ex startup ended before the first frame.", &Settings::use_d3d9ex});
  }
  if (vulkan && (settings.startup_latches & Settings::LATCH_VK_DEVICE)) {
    safety_locks.push_back({"Vulkan device adjustments", "The previous startup ended before the adjusted device presented a frame.", &Settings::adjust_vulkan_devices});
  }
  if (vulkan && (settings.startup_latches & Settings::LATCH_VK_NGX)) {
    safety_locks.push_back({"Vulkan NGX device extensions", "The previous startup with these extensions ended before the first frame.", &Settings::adjust_vulkan_devices_for_ngx});
  }
}

static bool DrawOverlayContent(const OverlayView& view, Settings* settings, OverlayState* state) {
  state->drag.pass_slider = false;  // the rows OR into it
  state->language_changed = false;
  const gui::LanguageScope language_scope(settings->language);
  bool changed = false;

  changed |= DrawRow(*FindSetting(ENABLED_KEY), view, settings, state);
  gui::SameLine();

  // 1.1.0 (a player's report): Esc also closes ReShade's overlay, so it cannot be the way to clear the hotkey. A right-click on the button clears it
  // (capturing or not); Esc only cancels a capture. A capture whose overlay closed ends by itself (ExpireKeyCapture), so the hotkey never stays off.
  if (state->capturing_key) {
    state->capture_seen = std::chrono::steady_clock::now();
    const widgets::KeyCaptureHit hit = widgets::KeyCaptureButton("", /*capturing=*/true);
    if (hit.right_clicked) {
      settings->enable_key = 0u;
      state->capturing_key = false;
      changed = true;
    } else if (view.last_key_pressed != 0u) {
      if (view.last_key_pressed != VK_ESCAPE) {
        settings->enable_key = view.last_key_pressed;
        changed = true;
      }
      state->capturing_key = false;
      state->key_captured_this_frame = true;
    }
  } else {
    std::string key_label = "Hotkey: none";
    if (settings->enable_key >= VK_F1 && settings->enable_key <= VK_F24) {
      key_label = std::format("Hotkey: F{}", settings->enable_key - VK_F1 + 1u);
    } else if (settings->enable_key != 0u) {
      std::array<wchar_t, 64> key_name = {};
      const auto scan_code = static_cast<LONG>(MapVirtualKeyW(settings->enable_key, MAPVK_VK_TO_VSC) << 16u);
      const int length = GetKeyNameTextW(scan_code, key_name.data(), static_cast<int>(key_name.size()));
      if (length > 0) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, key_name.data(), length, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, key_name.data(), length, utf8.data(), bytes, nullptr, nullptr);
        key_label = std::format("Hotkey: {}", utf8);
      } else {
        key_label = std::format("Hotkey: key {:#04x}", settings->enable_key);
      }
    }
    const widgets::KeyCaptureHit hit = widgets::KeyCaptureButton(key_label.c_str(), /*capturing=*/false);
    if (hit.clicked) {
      state->capturing_key = true;
      state->capture_seen = std::chrono::steady_clock::now();
    } else if (hit.right_clicked && settings->enable_key != 0u) {
      settings->enable_key = 0u;
      changed = true;
    }
    if (hit.hovered) {
      widgets::ItemTooltip("Click, then press a key. Right-click clears it.");
    }
  }

  gui::SameLine();
  const float min_language_width = gui::CalcTextSize(settings->language == Language::ENGLISH ? "English" : "简体中文 / Chinese").x + gui::GetFrameHeight() + gui::GetStyle().ItemSpacing.x;
  if (gui::GetContentRegionAvail().x < min_language_width) gui::NewLine();
  const float language_width = std::min(gui::GetContentRegionAvail().x, std::max(min_language_width, 12.f * gui::GetFontSize()));
  gui::SetCursorPosX(gui::GetCursorPosX() + gui::GetContentRegionAvail().x - language_width);
  gui::SetNextItemWidth(language_width);
  int selected_language = static_cast<int>(settings->language);
  const char* languages[] = {"English", "简体中文 / Chinese"};
  if (const int picked = widgets::Combo("UpliftLanguage", languages, 2, selected_language); picked >= 0) {
    FindSetting("Language")->set(*settings, picked);
    state->language_changed = true;
  }
  if (gui::IsItemHovered()) {
    const gui::LanguageScope tooltip_language(settings->language);
    widgets::ItemTooltip("English / Simplified Chinese. If Chinese appears as boxes, choose a Chinese-capable Global font in ReShade Settings (for example C:\\Windows\\Fonts\\msyh.ttc). No extra Uplift files are needed.");
  }

  widgets::NextGap(style::BlockGap());
  if (!view.safety_locks.empty()) {
    const ImVec4& bad = style::tokens::kBad;
    const ImVec2 warning_start = gui::GetCursorScreenPos();
    widgets::Panel("##safety_locks", [&] {
      {
        style::StyleScope accent;
        accent.Color(ImGuiCol_Text, bad);
        gui::SeparatorText("SAFETY LATCH ACTIVE");
      }
      for (const auto& lock : view.safety_locks) {
        gui::PushID(lock.feature.data());
        widgets::ColoredText(bad, lock.feature.data());
        widgets::Wrapped("%s", lock.reason.c_str());
        if (lock.setting != nullptr) {
          if (settings->*(lock.setting) == lock.restore_value) {
            widgets::Wrapped("Latch cleared for next launch. Restart the game to retry.");
          } else if (widgets::CardActionButton("Clear latch for next launch")) {
            settings->*(lock.setting) = lock.restore_value;
            changed = true;
          }
        } else if (lock.retry_now) {
          if (widgets::CardActionButton("Retry now")) state->retry_now = true;
        } else {
          widgets::Wrapped("Restart the game to retry this feature.");
        }
        gui::PopID();
      }
    });
    widgets::SideAccent(warning_start, gui::GetCursorScreenPos().y - gui::GetStyle().ItemSpacing.y, bad);
    widgets::NextGap(style::BlockGap());
  }

  const StatusCard& card = view.card;
  gui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {style::RowGap(), style::LabelGap()});
  widgets::Panel("##status_card", [&] {
    if (card.working) {
      // Plan 14 (design §1): green while NR works, three rows that say what Setup highlights (keys muted on the left, values in normal text from one column),
      // one muted Why line for a temporary fallback (else the note), and the footer. The height above is unchanged; whatever does not fit is left out.
      widgets::ColoredText(style::tokens::kAccent, card.title.c_str());
      const float value_x = gui::GetCursorPosX() + gui::CalcTextSize("Motion vectors").x + 2.f * gui::GetStyle().ItemSpacing.x;
      const auto row = [value_x](const char* key, const std::string& value) {
        widgets::SecondaryText(key);
        gui::SameLine(value_x);
        widgets::Wrapped("%s", value.c_str());
      };
      std::string recorded;
      for (size_t i = 0; i < view.stage_runs.size(); ++i) {
        if (view.stage_runs[i].count == 0) continue;
        if (!recorded.empty()) recorded += " · ";
        recorded += gui::Display(NR_STAGE_NAMES[i]);
      }
      row("Running at", recorded.empty() ? view.live.stage : recorded);
      row("Motion vectors", view.live.motion);
      if (!view.details_backend.empty()) row("Backend", view.details_backend);
      row("Resolution", view.live.resolution);
      if (!view.setup.why.empty()) {
        const std::string why = "Why: " + view.setup.why;
        widgets::SecondaryWrapped(why.c_str());
        if (gui::IsItemHovered()) {
          widgets::ItemTooltip("%s", why.c_str());
        }
      } else if (!view.live.note.empty()) {
        widgets::SecondaryText(view.live.note.c_str());
      }
      if (!view.live.footer.empty()) widgets::SecondaryText(view.live.footer.c_str());
    } else {
      widgets::ColoredText(style::tokens::kWarn, card.title.c_str());
      if (!card.reason.empty()) {
        widgets::Wrapped("%s", card.reason.c_str());
      }
      for (const std::string& fix : card.fixes) {
        gui::BulletText("%s", fix.c_str());
      }
    }
    switch (card.button) {
      case CardButton::NONE: break;
      case CardButton::RETRY_NOW:
        if (widgets::CardActionButton("Retry now")) state->retry_now = true;
        break;
      case CardButton::CLEAR_LATCH:  // Each blocked feature has its own explicit recovery action above the card.
        break;
    }
  }, style::Units(27));
  gui::PopStyleVar();

  const auto rows = [&](std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) changed |= DrawRow(*FindSetting(key), view, settings, state, true);
  };
  // ui-review.md §2 finding 1 / §3: the NR stage and Motion vectors toggles, in pipeline order, at the top
  // of the panel -- the decisions that shape everything else, instead of about the 45th line. Plan 14: Setup.
  widgets::NextGap(style::SectionGap());
  const bool placement_changed = SectionChanged(*settings, SettingSection::PLACEMENT);
  if (widgets::SectionCaption("Setup", "Reset", placement_changed && !state->defaults_view)) {
    ResetSection(settings, SettingSection::PLACEMENT);
    const Settings defaults = DefaultSettings();  // the three highlights follow at once, each to the best option that can run (the stage first)
    state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(SourcePickOf(defaults, view.dlss_explicit)), view.setup);
    state->setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPickOf(defaults)), view.setup, true);
    state->setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(defaults.resolution_mode), view.setup);
    changed = true;
  }
  widgets::NextGap(style::BlockGap());
  changed |= DrawStagePicker(view, settings, state);
  widgets::NextGap(style::BlockGap());
  changed |= DrawSetupGroups(view, settings, state);

  // ui-review.md §2 finding 7: Compare (Mine/Built-in) moves here, the first row of Look. The section's own
  // title lives in its card now (2026-11-06), drawn inside DrawLookGroups, so nothing sits bare on the panel.
  widgets::NextGap(style::SectionGap());
  changed |= DrawLookGroups(view, settings, state);
  DrawDlssReflexSection(view, settings, state, changed);

  if (settings->pass_count > 1u) {
    widgets::NextGap(style::SectionGap());
    gui::SeparatorText("Passes");
    widgets::NextGap(style::BlockGap());
  }
  // Plan 5: pass 2..pass_count, each its own collapsible tree (P4-P13), so the per-pass rows do not
  // flood the panel for the common one-pass case. ui-review.md §5: the tree's own label shows its state.
  // Split chain (Task 3, design 6.2): Runs at first; on the first pass of a later stage group, that stage's Resolution (and Custom scale) and, while Keep
  // faces is on, its Keep faces here; while the chain is split the tree's label names where the pass runs.
  for (uint32_t pass = 2u; pass <= settings->pass_count; ++pass) {
    const bool follows = settings->passes[pass - 2u].follow_pass1;
    const PassStageRow& stage_row = view.setup.pass_stages[pass - 2u];
    const char* stage = view.RecordedStageName(pass - 1u);
    if (!stage) stage = stage_row.runs == SourcePick::PRESENT ? view.SharedPresentName(*settings) : StageLabel(stage_row.runs);
    const std::string summary = gui::Display(stage) + " · " + gui::Display(follows ? "Same look as pass 1" : "Own look");
    if (widgets::SectionHeader(gui::Display(std::format("Pass {}", pass)).c_str(), summary.c_str(), nullptr, false, nullptr)) {
      gui::PushID(static_cast<int>(pass));
      widgets::Panel("##pass_body", [&] {
        widgets::PairedGroups("##pass_groups", [&] {
          widgets::GroupHeading("Placement and network");
          changed |= DrawPassStageRow(*FindSetting(std::format("Pass{}Stage", pass)), view, settings, state);
          changed |= DrawRow(*FindSetting(std::format("Pass{}Preset", pass)), view, settings, state, true);
          if (stage_row.starts_group && stage_row.runs != SourcePick::BEFORE_UPSCALING) {
            if (stage_row.runs == SourcePick::AFTER_DLSS) rows({"ResolutionAfterDlss", "ResolutionScaleAfterDlss", "KeepFacesAfterDlss"});
            else rows({"ResolutionPresent", "ResolutionScalePresent", "KeepFacesPresent"});
          }
        }, [&] {
          widgets::GroupHeading("Pass look");
          for (const SettingDescriptor& descriptor : SettingsSchema()) {
            if (descriptor.pass == pass && (descriptor.flags & (setting_flags::PASS_STAGE | setting_flags::NETWORK_PRESET)) == 0u)
              changed |= DrawRow(descriptor, view, settings, state, true);
          }
        });
      }, 0);
      gui::PopID();
    }
  }

  widgets::NextGap(style::SectionGap());
  bool reset = false;
  const bool shaping_changed = SectionChanged(*settings, SettingSection::RESULT) || SectionChanged(*settings, SettingSection::LIMITS);
  const bool shaping_enabled = state->defaults_view ? DefaultSettings().look.enabled : settings->look.enabled;
  gui::SetNextItemOpen(shaping_enabled, ImGuiCond_Once);
  const bool shaping = widgets::SectionHeader("Result shaping", shaping_enabled ? "On" : "Off", "Reset",
                                                shaping_changed && !state->defaults_view, &reset);
  if (reset) {
    ResetSection(settings, SettingSection::RESULT);
    ResetSection(settings, SettingSection::LIMITS);
    changed = true;
  }
  if (shaping) widgets::Panel("##shaping_body", [&] {
    rows({"ShapeResult"});
    if (shaping_enabled) {
      widgets::NextGap(style::BlockGap());
      widgets::PairedGroups("##result_groups", [&] {
        widgets::GroupHeading("Strength and colour");
        rows({"EditStrength", "Brighten", "Darken", "ResultColor", "HueShift"});
      }, [&] {
        widgets::GroupHeading("Tonal range");
        rows({"Shadows", "Midtones", "Highlights"});
      });
      widgets::NextGap(style::BlockGap());
      widgets::PairedGroups("##limit_groups", [&] {
        widgets::GroupHeading("Limits");
        rows({"MaxBrighten", "MaxDarken", "MaxColorChange"});
      }, [&] {
        widgets::GroupHeading("Detail and stability");
        rows({"LargeScaleTone", "FineDetail", "DetailRadius", "HaloSuppression", "Stabilize", "StabilizeMs", "StabilizeDetail"});
      });
    }
  });

  reset = false;
  const bool mask_changed = SectionChanged(*settings, SettingSection::MASK);
  const bool mask = widgets::SectionHeader("Mask and HUD", nullptr, "Reset", mask_changed && !state->defaults_view, &reset);
  if (reset) { ResetSection(settings, SettingSection::MASK); changed = true; }
  if (mask) widgets::Panel("##mask_body", [&] {
    widgets::PairedGroups("##mask_groups", [&] { widgets::GroupHeading("Mask"); rows({"Mask"}); },
                                         [&] { widgets::GroupHeading("HUD"); rows({"UICorrection"}); });
  });

  reset = false;
  const bool fixes_changed = SectionChanged(*settings, SettingSection::FIXES_COLOR) || SectionChanged(*settings, SettingSection::FIXES_GUIDES);
  const bool fixes = widgets::SectionHeader("Fixes", "Colour · Exposure · Motion", "Reset",
                                              fixes_changed && !state->defaults_view, &reset);
  if (reset) {
    ResetSection(settings, SettingSection::FIXES_COLOR);
    ResetSection(settings, SettingSection::FIXES_GUIDES);
    changed = true;
  }
  if (fixes) widgets::Panel("##fixes_body", [&] {
    widgets::SecondaryText("Only change these if you know what you are doing.");
    widgets::PairedGroups("##colour_groups", [&] {
      widgets::GroupHeading("Colour");
      rows({"Encoding", "DiffuseWhiteNits", "TransferStrength", "ColorStrength"});
    }, [&] {
      widgets::GroupHeading("Exposure");
      rows({"InputExposure", "AutoExposureMode", "AutoExposureBlend", "ExposureAdapt", "AdaptBrighterStops", "AdaptDarkerStops"});
    });
    widgets::NextGap(style::BlockGap());
    widgets::PairedGroups("##guide_groups", [&] {
      widgets::GroupHeading("Colour conversion");
      rows({"LinearUnitNits", "SourcePrimaries", "NeuralTransfer", "ChromaClamp", "NearBlackGuard"});
    }, [&] {
      widgets::GroupHeading("Motion and game state");
      rows({"MotionScaleX", "MotionScaleY", "ChainedHistory", "StateRestore"});
    });
  });

  if (widgets::SectionHeader("Advanced", "Routing · Runtime · Diagnostics", nullptr, false, nullptr)) widgets::Panel("##advanced_body", [&] {
    widgets::PairedGroups("##advanced_groups", [&] {
      widgets::GroupHeading("Routing");
      rows({"PresentWithFrameGen", "BeforeFGMotion", "VulkanNr", "AllowCrossQueuePasses", "UseD3D9Ex"});
    }, [&] {
      widgets::GroupHeading("Memory and recovery");
      rows({"VramCheck", "BudgetMarginMB", "GraceSeconds", "AutoResume", "AutoRetry"});
    });
    widgets::NextGap(style::BlockGap());
    widgets::Panel("##runtime_group", [&] {
      widgets::GroupHeading("Runtime and logging");
      rows({"SnippetPath", "NgxHooks", "ForeignNr", "LogLevel", "ShowNvIndicator"});
    });
    if (settings->keep_faces) {
      widgets::NextGap(style::BlockGap());
      if (widgets::SectionHeader("Face mask tuning", "Keep faces", nullptr, false, nullptr)) widgets::Panel("##face_controls", [&] {
        rows({"ShowFaceMask", "FaceMaskThreshold", "FaceMaskFull", "FaceMaskStrength", "FaceMaskSoftness", "FaceEdgeFalloff",
              "FaceLightingDensity", "FaceSpeckSize", "FaceFillSkin", "FaceFillRadius", "FaceFillTolerance"});
      });
    }
  });
  if (widgets::SectionHeader("Details", "Recorded passes", nullptr, false, nullptr)) widgets::Panel("##details_body", [&] { DrawDetails(view, *settings, *state); });
  widgets::NextGap(style::SectionGap());

  // G2: Restore all defaults, confirmed once before it takes effect.
  gui::Separator();
  if (widgets::CardActionButton("Restore all defaults")) {
    state->confirm_restore = true;
  }
  if (state->confirm_restore) {
    widgets::Wrapped("Every setting returns to its default, the hotkey included; Enable stays as it is.");
    if (widgets::CardActionButton("Restore all")) {
      RestoreAllDefaults(settings);
      changed = true;
      state->confirm_restore = false;
    }
    gui::SameLine();
    if (widgets::CardActionButton("Keep my settings")) {
      state->confirm_restore = false;
    }
  }

  changed |= DrawEnableWarning(*FindSetting("PresentWithFrameGen"), settings, state, view.dlss_explicit);
  changed |= DrawEnableWarning(*FindSetting("AllowCrossQueuePasses"), settings, state, view.dlss_explicit);
  return changed;
}

bool DrawOverlay(const OverlayView& view, Settings* settings, OverlayState* state) {
  // The panel's own look, applied every frame: ReShade writes its own style back whenever its settings are
  // touched (see style::RefreshForDpi), which would otherwise put ReShade's colours and corners over the panel's.
  style::RefreshForDpi();
  return DrawOverlayContent(view, settings, state);
}

}  // namespace uplift::ui
