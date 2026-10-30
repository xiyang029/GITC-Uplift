#include "ui/overlay.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "addon/reshade_api.hpp"
#include "ui/settings_schema.hpp"
#include "ui/localized_imgui.hpp"
#include "ui/style.hpp"
#include "ui/widgets.hpp"

namespace uplift::ui {
namespace {

// ui-review.md §3 (user decision) and §4.3: every row with three or fewer short choices -- Source,
// Motion vectors, Compare, Model, Upsampling, Stabilise, NR mask, UI correction and any other CHOICE row
// this short -- draws as a segmented control instead of RadioButton (which sizes to its text) or a
// Combo, so every option is equally sized and the row's total width matches the control column, lining up
// with the sliders and combos above and below it. `why[i]` non-empty greys option `i`; its tooltip
// starts "Unavailable here: <why[i]>", then `tips[i]`. The label goes into the row's label column, as
// every other row's does (widgets::RowLabel). Returns the clicked option, or -1.
int RadioRow(const char* label, std::span<const char* const> options, std::span<const std::string_view> why,
             std::span<const std::string_view> tips, int selected) {
  widgets::RowLabel(label);
  return widgets::Segmented(label, options.data(), static_cast<int>(options.size()), selected,
                            why.empty() ? nullptr : why.data(), tips.empty() ? nullptr : tips.data());
}

// Plan 14 (R90): the tooltip line of a highlighted option while the highlight is not the stored preference (what runs differs from what ReShade.ini holds).
std::string SavedNote(std::string_view preference) {
  return std::format("Saved: {}; it runs again by itself when it can.", preference);
}

// One schema row. Returns true when the value changed. Called both from DrawOverlay (the Enable
// row) and from its own draw_rows lambda (every other row), so it stays a free function. `inline_label`
// asks for the row's label to sit right against its control instead of in the panel's label column: only
// the Enable row does, since its switch and hotkey button are the whole row and a column would leave a gap
// between the label and the switch.
bool DrawRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state,
             bool inline_label = false) {
  const std::string label(descriptor.label);
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
  const auto later_group_scaled = [settings](const PassStageRow& row) {
    return row.starts_group && ((row.runs == SourcePick::AFTER_DLSS && settings->resolution_mode_after_dlss != ResolutionMode::FULL)
                                || (row.runs == SourcePick::PRESENT && settings->resolution_mode_present != ResolutionMode::FULL));
  };
  const bool below_full = (settings->resolution_mode != ResolutionMode::FULL)
                           || std::ranges::any_of(view.setup.pass_stages, later_group_scaled);
  const bool upsampling_hidden = (descriptor.key == "Upsampling" && !below_full);
  // The two band gains matter only while Edge-aware itself does (below Full); Classic has no bands.
  const bool guided_gains_hidden = ((descriptor.key == "GuidedBaseGain" || descriptor.key == "GuidedDetailGain"
                                     || descriptor.key == "DensityOnBase" || descriptor.key == "EdgeDensityAttenuation")
                                    && (!below_full || settings->upsampling != color::Upsampling::EDGE_AWARE));
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
  // Every row's label goes into the label column and its control into the control column with no text of
  // its own (`##key` keeps the id), so labels, controls and the reset arrows line up down the whole panel.
  const std::string hidden = std::format("##{}", descriptor.key);
  const std::string tip(descriptor.tooltip);
  const char* const tip_text = (tip.empty() ? nullptr : tip.c_str());
  gui::BeginDisabled(!disabled_reason.empty() || read_only);
  bool changed = false;
  std::string saved_note;  // Plan 14 (R90): the Resolution combo's tooltip, while the mode that runs is not the stored one
  if (descriptor.key == "SkinStructure") {
    bool follows = (settings->skin_structure < 0.f);
    // The row's own label is its control's, so the two-step row (follow, or set a value) reads as two rows.
    widgets::RowLabel("Skin follows Structure");
    if (widgets::Switch("##SkinFollows", &follows)) {
      settings->skin_structure = (follows ? SKIN_SAME_AS_STRUCTURE : settings->local_structure);
      changed = true;
    }
    gui::BeginDisabled(follows);
    float skin = (follows ? settings->local_structure : settings->skin_structure);
    widgets::RowLabel(label.c_str(), tip_text);
    if (widgets::Slider(hidden.c_str(), &skin, 0.f, 1.f, "%.2f", nullptr, &state->drag.skin_structure)) {
      settings->skin_structure = skin;
      changed = true;
    }
    gui::EndDisabled();
  } else if (descriptor.key == "DiffuseWhiteNits") {
    // ui-review.md §3 rule 2 (the Skin pattern): an Automatic switch, the control greyed at the value
    // in use -- shown, not hidden, so the effective value always reads even while it is automatic.
    bool automatic = (settings->diffuse_white_nits <= 0.f);
    widgets::RowLabel("Automatic diffuse white");
    if (widgets::Switch("##DiffuseWhiteAutomatic", &automatic)) {
      settings->diffuse_white_nits = (automatic ? 0.f : std::clamp(view.effective_diffuse_white_nits, 48.f, 500.f));
      changed = true;
    }
    gui::BeginDisabled(automatic);
    float nits = (automatic ? view.effective_diffuse_white_nits : settings->diffuse_white_nits);
    widgets::RowLabel(label.c_str(), tip_text);
    if (widgets::Slider(hidden.c_str(), &nits, 48.f, 500.f, "%.0f", nullptr, nullptr) && !automatic) {
      settings->diffuse_white_nits = nits;
      changed = true;
    }
    gui::EndDisabled();
  } else {
    switch (descriptor.kind) {
      case SettingKind::BOOL: {
        bool value = (descriptor.get(*settings) != 0.0);
        if (inline_label) {
          widgets::InlineLabel(label.c_str(), tip_text);
        } else {
          widgets::RowLabel(label.c_str(), tip_text);
        }
        // In place on the Enable row: its line carries the hotkey button and the language combo after the
        // switch, so the row's far end is not the switch's to take.
        if (widgets::Switch(hidden.c_str(), &value, /*at_row_end=*/!inline_label)) {
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
        widgets::RowLabel(label.c_str(), tip_text);
        // A row of whole numbers draws as the panel's own slider; only a value that may be typed past 100
        // (the VRAM margin) keeps ImGui's input, where a slider would have no useful range.
        bool active = false;
        bool edited = false;
        if (descriptor.max > 100.0) {
          edited = gui::InputInt(hidden.c_str(), &value, descriptor.step, descriptor.fast_step);
          active = ImGui::IsItemActive();  // a typed row reports its focus, so APPLY_ON_RELEASE waits for the field to let go
        } else {
          edited = widgets::SliderInt(hidden.c_str(), &value, static_cast<int>(descriptor.min),
                                      static_cast<int>(descriptor.max), nullptr, &active);
        }
        if (!on_release) {
          if (edited) {
            descriptor.set(*settings, std::clamp(static_cast<double>(value), descriptor.min, descriptor.max));
            changed = true;
          }
        } else {
          // Pass Count and the foveation rows, as a float row: while held the slider shows the value under the
          // mouse; the setting, NR's features and the rows below change once, on release (a Ctrl+click typed
          // value counts as one edit, when the field lets go).
          if (edited) {
            state->pending_release = value;
            state->pending_key = descriptor.key;
          }
          if (!active && state->pending_key == descriptor.key && state->pending_release) {
            const int released = *std::exchange(state->pending_release, std::nullopt);
            if (released != applied) {
              descriptor.set(*settings, std::clamp(static_cast<double>(released), descriptor.min, descriptor.max));
              changed = true;
            }
          }
        }
        break;
      }
      case SettingKind::FLOAT: {
        // 2026-10-09: AutoExposureBlend is stored 0..1 and shown as 0-100 %.
        const float shown_scale = (descriptor.key == "AutoExposureBlend" ? 100.f : 1.f);
        float value = static_cast<float>(descriptor.get(*settings));
        const float high = static_cast<float>(SliderMax(descriptor)) * shown_scale;  // Ctrl+click may type past it (DescriptorMax)
        const bool resolution_scale = (descriptor.key == "ResolutionScale" || descriptor.key == "ResolutionScaleAfterDlss"
                                       || descriptor.key == "ResolutionScalePresent" || descriptor.key == "ResolutionScaleY");
        const char* const format = (descriptor.key == "GraceSeconds" ? "%.1f"
                                    : resolution_scale                      ? "%.0f %%"
                                    : descriptor.key == "AutoExposureBlend" ? "%.0f %%"
                                    : descriptor.key == "StabilizeMs"       ? "%.0f ms"
                                                                            : "%.2f");
        widgets::RowLabel(label.c_str(), tip_text);
        bool active = false;
        const bool dragged = widgets::Slider(hidden.c_str(), &value, static_cast<float>(descriptor.min) * shown_scale, high, format, nullptr, &active,
                            shown_scale);
        if (dragged) {
          descriptor.set(*settings, static_cast<double>(value));
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
        // Every choice row draws through the panel's own control (widgets::Segmented): equal-width buttons,
        // or a dropdown of the same choices where the row is too narrow for its longest label (ReShade docks
        // the panel at ~450 px). An option that cannot run now is greyed with its reason on hover, and the
        // highlight is the option that runs -- the stored one, except on Setup's rows, whose highlights are
        // Plan 14's settle.
        const bool resolution_row = (descriptor.key == "ResolutionMode" || descriptor.key == "ResolutionAfterDlss"
                                     || descriptor.key == "ResolutionPresent");
        size_t count = (std::min)(descriptor.choices.size(), static_cast<size_t>(8));
        std::array<const char*, 8> labels = {};
        for (size_t index = 0u; index < count; ++index) {
          labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
        }
        std::array<std::string_view, 8> why = {};
        std::array<std::string_view, 8> tips = {};
        tips.fill(descriptor.tooltip);
        int shown = value;
        if (!disabled_reason.empty()) {
          why.fill(disabled_reason);  // e.g. Game state after NR, greyed while no DLSS placement can run
        } else if (resolution_row) {
          // Plan 14 (the user's decision 1): the preview is the mode that runs. Split chain (Task 3): a later
          // stage group's preview is its stored mode, since Setup's highlight is pass 1's.
          const bool setup_row = (descriptor.key == "ResolutionMode");
          const size_t stored = (std::min)(static_cast<size_t>(value), count - 1u);
          shown = static_cast<int>(setup_row ? static_cast<size_t>(view.setup.resolution) : stored);
          for (size_t index = 0u; index < count; ++index) {
            const OptionState& option = view.setup.resolution_options[index];
            // Fix round 1 (M5): Setup's row greys every entry that cannot run, its highlight included (as
            // before the split chain); a later stage's row keeps its stored mode selectable, since its
            // preview is that mode and not what runs.
            if (!option.why.empty() && (setup_row || index != static_cast<size_t>(shown))) {
              why[index] = option.why;
            }
          }
          const OptionState& option = view.setup.resolution_options[stored];
          if (static_cast<size_t>(shown) != stored && (option.why.empty() || option.temporary)) {  // a fixed cause never clears: no promise
            saved_note = SavedNote(labels[stored]);
          }
        } else if (descriptor.key == "VulkanNr") {
          // Plan 19 (the owner's UI rules): Native greyed where it cannot run, with why; the highlight is the route that runs. T5: Direct3D 12 too.
          why[static_cast<size_t>(VulkanNrMode::NATIVE)] = view.vulkan_native_unavailable;
          why[static_cast<size_t>(VulkanNrMode::DIRECT3D_12)] = view.vulkan_d3d12_unavailable;
          if (view.vulkan_nr_running) {
            shown = static_cast<int>(*view.vulkan_nr_running);
          }
        } else if (descriptor.key == "FGMultiplier") {
          // Only what the DLSS-G runtime's ceiling allows is offered (generated frames + 1: 3 = up to
          // 4x). A stored choice above it stays saved: the row shows Game and the tooltip says so.
          count = (std::min)(count, view.fg_multiplier_options);
          if (static_cast<size_t>(shown) >= count) {
            saved_note = SavedNote(labels[static_cast<size_t>(shown)]);
            shown = 0;
          }
        }
        widgets::RowLabel(label.c_str(), tip_text);
        const int clicked = widgets::Segmented(hidden.c_str(), labels.data(), static_cast<int>(count), shown,
                                               why.data(), tips.data());
        if (clicked >= 0) {
          descriptor.set(*settings, static_cast<double>(clicked));
          if (descriptor.key == "ResolutionMode") {
            state->setup_settle.Clicked(SetupRow::RESOLUTION, static_cast<uint32_t>(clicked));
          }
          changed = true;
        }
        break;
      }
      case SettingKind::TEXT: {
        std::array<char, 1024> text = {};
        descriptor.get_text(*settings).copy(text.data(), text.size() - 1u);
        widgets::RowLabel(label.c_str(), tip_text);
        if (gui::InputText(hidden.c_str(), text.data(), text.size())) {
          descriptor.set_text(*settings, text.data());
          changed = true;
        }
        break;
      }
      case SettingKind::KEY: break;  // the Enable hotkey is drawn beside Enable
    }
  }
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && (!descriptor.tooltip.empty() || !disabled_reason.empty())) {
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
    gui::SetTooltip("%s", tooltip.c_str());
  }
  // The reset arrow after a row that differs from its default (design §3.16, G1). The Enable
  // switch itself is the one row without an arrow: "reset" would only mean "switch NR off". Drawn before
  // the row's notes, so the arrow stays on the control's own line.
  if (descriptor.section != SettingSection::TOP && !IsDefault(*settings, descriptor)) {
    const bool reset_clicked = widgets::ResetButton(descriptor.key.data(), FormatSettingValue(descriptor, DefaultSettings()).c_str());
    if (reset_clicked) {
      ResetSetting(settings, descriptor);
      if (descriptor.key == "ResolutionMode") {
        // The highlight follows at once, to the default when it can run and else to the best that can.
        state->setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(DefaultSettings().resolution_mode), view.setup);
      }
      changed = true;
    }
  }
  // A row's own footnote, under its control column: what the control is doing, or why it cannot run.
  if (descriptor.key == "PassCount" && !view.frame_generation_warning.empty()) {
    widgets::RowNote(view.frame_generation_warning.c_str());
  }
  if (descriptor.key == "Mask" && !view.mask_note.empty()) {
    widgets::RowNote(view.mask_note.c_str());
  }
  if (descriptor.key == "UICorrection" && !view.ui_correction_note.empty()) {
    widgets::RowNote(view.ui_correction_note.c_str());
  }
  if (descriptor.key == "UseD3D9Ex") {
    const std::string& note = (view.d3d9ex_unavailable.empty() ? view.d3d9ex_readout : view.d3d9ex_unavailable);
    if (!note.empty()) {
      widgets::RowNote(note.c_str());
    }
  }
  if (descriptor.key == "VulkanNr" && view.vulkan_nr_unavailable.empty() && !view.vulkan_nr_note.empty()) {
    widgets::RowNote(view.vulkan_nr_note.c_str());
  }
  gui::EndDisabled();
  gui::PopID();
  return changed && !read_only;
}

bool DrawEnableWarning(const SettingDescriptor& descriptor, Settings* settings, OverlayState* state) {
  const std::string title = std::format("{} - performance warning", descriptor.label);
  if (state->enable_warning_key == descriptor.key) {
    state->enable_warning_key = {};
    gui::OpenPopup(title.c_str());
  }
  if (!gui::IsPopupOpen(title.c_str())) return false;
  gui::SetNextWindowSize(ImVec2(gui::GetFontSize() * 34.f, 0.f), ImGuiCond_Appearing);
  bool open = true;
  if (!gui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) return false;
  gui::TextWrapped("%s", descriptor.enable_warning.data());
  gui::Separator();
  const bool cancel = gui::Button("Keep disabled");
  gui::SetItemDefaultFocus();
  gui::SameLine();
  bool enable = false;
  {
    style::StyleScope buttons;
    buttons.AccentButton();
    enable = gui::Button("Enable anyway");
  }
  if (cancel || enable || gui::IsKeyPressed(ImGuiKey_Escape)) {
    gui::CloseCurrentPopup();
  }
  if (enable) {
    descriptor.set(*settings, 1.0);
  }
  gui::EndPopup();
  return enable;
}

// Plan 14: the NR stage row -- Before upscaling / After DLSS / Present -- from Setup: an option that cannot run now is greyed (its reason on hover), the
// highlight is what runs, and a click writes the stored preference as before. Not folded into DrawRow: it reads and writes two schema keys (Source,
// PreUpscale) as one composite choice, and it carries the DLSS-placement latch's Clear latch button.
bool DrawSourceRow(const OverlayView& view, Settings* settings, OverlayState* state) {
  gui::PushID("Source");
  static constexpr std::array<const char*, 3> OPTIONS = {"Before upscaling", "After DLSS", "Present"};
  const std::array<std::string_view, 3> direct3d_tips = {
      "NR works on the game's render-size image, and the game's DLSS upscales the result. Cheaper. Experimental.",
      "NR works on DLSS's output inside the game's frame, before the HUD, post-processing and frame "
      "generation. Runs on Present until the game runs DLSS.",
      "NR works on the finished frame, HUD included. Works in every game. Uplift.fx's Uplift technique "
      "sets where among your effects.",
  };
  // Plan 13 (the user's decision 1): on Vulkan the DLSS placements are explicit choices, and their tips name what stays after the first use.
  const std::array<std::string_view, 3> vulkan_tips = {
      "NR works on the game's render-size image, natively on the game's Vulkan device, and the game's DLSS upscales the result. Experimental. About "
      "0.6 GB of VRAM stays with the game's device after the first use, until the game exits.",
      "NR works on DLSS's output inside the game's frame, natively on the game's Vulkan device, before the HUD and post-processing. Runs on "
      "Present until the game runs DLSS. About 0.6 GB of VRAM stays with the game's device after the first use, until the game exits.",
      "NR works on the finished frame, HUD included. The default on Vulkan. Uplift.fx's Uplift technique sets where among your effects.",
  };
  std::array<std::string_view, 3> tips = (view.dlss_explicit ? vulkan_tips : direct3d_tips);
  // Split-chain design 11 (the owner, 2026-10-15): with Ray Reconstruction, Before upscaling works on RR's input.
  std::string before_tip;
  if (view.setup.before_upscaling_on_rr) {
    before_tip = std::format("{}\nThe game's DLSS is Ray Reconstruction: NR works on its input, before it denoises and upscales it.", tips[0]);
    tips[0] = before_tip;
  }
  std::array<std::string_view, 3> why = {};
  for (size_t index = 0u; index < why.size(); ++index) {
    why[index] = view.setup.stage_options[index].why;
  }
  const SourcePick preferred = SourcePickOf(*settings, view.dlss_explicit);
  const int shown = static_cast<int>(view.setup.stage);
  std::string saved_tip;  // the highlighted option's tip, with the stored preference when it is not the one that runs
  const OptionState& stored_stage = view.setup.stage_options[static_cast<size_t>(preferred)];
  if (view.setup.stage != preferred && (stored_stage.why.empty() || stored_stage.temporary)) {  // a fixed cause never clears: no promise
    saved_tip = std::format("{}\n{}", tips[static_cast<size_t>(shown)], SavedNote(OPTIONS[static_cast<size_t>(preferred)]));
    tips[static_cast<size_t>(shown)] = saved_tip;
  }
  bool changed = false;
  if (const int clicked = RadioRow("NR stage", OPTIONS, why, tips, shown); clicked >= 0) {
    SetSourcePick(settings, static_cast<SourcePick>(clicked), view.dlss_explicit);
    state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(clicked));
    changed = true;
  }
  if (!IsDefault(*settings, *FindSetting("Source")) || !IsDefault(*settings, *FindSetting("PreUpscale"))) {
    const bool reset_clicked = widgets::ResetButton("Source", view.dlss_explicit ? "Present on Vulkan" : "After DLSS");
    if (reset_clicked) {
      // The defaults themselves (Auto, PreUpscale off): After DLSS on Direct3D 12, Present on Vulkan.
      const SourcePick default_pick = (view.dlss_explicit ? SourcePick::PRESENT : SourcePick::AFTER_DLSS);
      SetSourcePick(settings, default_pick, view.dlss_explicit);
      state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(default_pick), view.setup);  // at once, to the best that can run
      changed = true;
    }
  }
  // ui-review.md §4.1: the latch's one surviving Clear latch button, unless the status card already shows it.
  if (view.dlss_latched && view.card.button != CardButton::CLEAR_LATCH) {
    gui::SameLine();
    if (gui::SmallButton("Clear latch")) {
      settings->dlss_placement_blocked = false;
      settings->vulkan_native_nr = true;  // Plan 19 fix round (I-C): native Vulkan NR's latch too, from the next start
      changed = true;
    }
  }
  gui::PopID();
  return changed;
}

// Split chain (Task 3, design 6.2): pass n's Runs at row, as the NR stage row (widgets::Segmented, which itself falls back to a combo where the four long
// choices do not fit): a choice that cannot run is greyed with why, the highlight is where the pass runs, and the stored pick is named while it runs
// elsewhere. Where every pass runs at one stage the whole row is greyed.
// Not folded into DrawRow: its choices' states come from Setup's pass rows, not from the schema.
bool DrawPassStageRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings) {
  const size_t later = descriptor.pass - 2u;
  const PassStageRow& row = view.setup.pass_stages[later];
  const std::string label(descriptor.label);
  gui::PushID(label.c_str());
  gui::BeginDisabled(!view.setup.split_capable || view.setup.stopped);  // fix round 1 (M5): greyed with Setup's other rows once NR stopped (Plan 15)
  const auto stored = static_cast<size_t>(settings->passes[later].stage);
  const auto shown = static_cast<size_t>(row.shown);
  bool changed = false;
  const std::string tip(descriptor.tooltip);
  std::array<const char*, 4> labels = {};
  std::array<std::string_view, 4> why = {};
  // Both halves of the row (its labels and its option states) are bounded by the schema, not by each other:
  // the two 4s are written separately, and a mismatch would read a null label into CalcTextSize.
  const size_t count = (std::min)(row.options.size(), descriptor.choices.size());
  for (size_t index = 0u; index < count; ++index) {
    labels[index] = descriptor.choices[index].data();  // literals and static strings: NUL-terminated
    why[index] = row.options[index].why;
  }
  widgets::RowLabel(label.c_str(), tip.empty() ? nullptr : tip.c_str());
  const int clicked = widgets::Segmented(std::format("##{}", descriptor.key).c_str(), labels.data(),
                                         static_cast<int>(count), static_cast<int>(shown), why.data());
  if (clicked >= 0 && static_cast<size_t>(clicked) != stored) {
    descriptor.set(*settings, static_cast<double>(clicked));
    changed = true;
  }
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    std::string tooltip(descriptor.tooltip);
    if (!view.setup.split_capable) {
      tooltip = std::format("Greyed: {}\n{}", ONE_STAGE_REASON, descriptor.tooltip);
    } else if (shown != stored && (row.options[stored].why.empty() || row.options[stored].temporary)) {  // a fixed cause never clears: no promise
      tooltip += "\n" + SavedNote(descriptor.choices[stored]);
    }
    gui::SetTooltip("%s", tooltip.c_str());
  }
  if (!IsDefault(*settings, descriptor)) {
    const bool reset_clicked = widgets::ResetButton(descriptor.key.data(), descriptor.choices[0].data());
    if (reset_clicked) {
      ResetSetting(settings, descriptor);
      changed = true;
    }
  }
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
  bool changed = false;
  if (const int clicked = RadioRow("Motion vectors", OPTIONS, why, tips, shown); clicked >= 0) {
    SetMotionPick(settings, static_cast<MotionPick>(clicked));
    state->setup_settle.Clicked(SetupRow::MOTION, static_cast<uint32_t>(clicked));
    changed = true;
  }
  if (!IsDefault(*settings, *FindSetting("MotionVectors"))) {
    const bool reset_clicked = widgets::ResetButton("MotionVectors", "DLSS");
    if (reset_clicked) {
      SetMotionPick(settings, MotionPick::DLSS);
      state->setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPick::DLSS), view.setup, true);  // DLSS is stored as Auto
      changed = true;
    }
  }
  gui::PopID();
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
  }
  view->setup = state->setup_settle.Update(ResolveSetup(facts), now);
  if (view->card.working) {
    view->live = BuildLiveCard(facts, view->setup);
  }
}

// The status card as widgets::CardView, the piece of chrome widgets::DrawStatusCard draws: the title, and
// either why it is not working or what is running.
widgets::CardView BuildCardView(const StatusCard& card, const OverlayView& view) {
  widgets::CardView shown;
  shown.title = card.title;
  shown.showing_work = card.working;
  if (!card.working) {
    shown.reason = card.reason;
    shown.fixes = card.fixes;
    return shown;
  }
  // Plan 14 (design §1): the three rows that say what Setup highlights (keys muted on the left, values in
  // normal text from one column), one muted Why line for a temporary fallback (else the note), and the
  // footer. The card's height is unchanged; whatever does not fit is left out.
  shown.lines.push_back({"Running at", view.live.stage});
  shown.lines.push_back({"Motion vectors", view.live.motion});
  shown.lines.push_back({"Resolution", view.live.resolution});
  // Muted and wrapped: two lines in the card's spare height; the child clips anything longer, and the full
  // text is the tooltip.
  shown.note = (view.setup.why.empty() ? view.live.note : std::format("Why: {}", view.setup.why));
  shown.footer = view.live.footer;
  return shown;
}

// The DLSS / Reflex card's sub-captions: the first row of each group names the group. The rows are
// drawn in schema order, so these keys are where a group begins.
constexpr const char* DlssReflexGroup(std::string_view key) {
  if (key == "DLSSPresetDLAA") return "DLSS presets";
  if (key == "DLSSRenderScaleDLAA") return "DLSS render scale";
  if (key == "DLSSFGPreset") return "Frame generation";
  if (key == "ReflexMode") return "Reflex";
  if (key == "RayReconstructionPreset") return "Ray Reconstruction";
  return nullptr;
}

bool DrawOverlay(const OverlayView& view, Settings* settings, OverlayState* state) {
  // The panel draws at BaseFontSize(), one push for the whole overlay: every row measures itself in font
  // sizes, so the layout scales with the text. StyleScope pops it on every return path.
  style::StyleScope panel_font;
  panel_font.FontSize(style::BaseFontSize());
  // The panel draws and colours itself out of style.hpp's tokens; without this the global style stays
  // ReShade's (square corners, ReShade's blues). Every frame, not once: ReShade writes its own style back
  // whenever its settings are touched (see RefreshForDpi).
  style::RefreshForDpi();
  state->drag.pass_slider = false;  // the rows OR into it
  state->language_changed = false;
  const char* languages[] = {"English", "简体中文 / Chinese"};
  const int picked_language = widgets::LanguageCombo(
      "##UpliftLanguage", languages, 2, static_cast<int>(settings->language),
      "English / Simplified Chinese. If Chinese appears as boxes, choose a Chinese-capable Global font in ReShade Settings (for example C:\\Windows\\Fonts\\msyh.ttc). No extra Uplift files are needed.");
  if (picked_language >= 0) {
    FindSetting("Language")->set(*settings, picked_language);
    state->language_changed = true;
  }
  const gui::LanguageScope language_scope(settings->language);
  bool changed = false;

  // 2026-10-22 (owner): the Enable row is one run at the panel's left edge -- its label, its switch and the
  // hotkey button, with the language combo at the right end of that same line -- where it used to be the
  // label in the column and the switch a hand's width to its right. The card below no longer offers its own
  // "Turn on" button either; this switch is the only one.
  changed |= DrawRow(*FindSetting(ENABLED_KEY), view, settings, state, /*inline_label=*/true);
  gui::SameLine();
  const float hotkey_width =
      std::max(1.f, gui::GetContentRegionAvail().x - gui::GetFontSize() * widgets::kLanguageEm - gui::GetStyle().ItemSpacing.x);
  const auto hotkey_button_width = [&](const char* label) {
    return std::min(hotkey_width, gui::CalcTextSize(label).x + gui::GetStyle().FramePadding.x * 2.f);
  };
  // 1.1.0 (a player's report): Esc also closes ReShade's overlay, so it cannot be the way to clear the hotkey. A right-click on the button clears it
  // (capturing or not); Esc only cancels a capture. A capture whose overlay closed ends by itself (ExpireKeyCapture), so the hotkey never stays off.
  if (state->capturing_key) {
    state->capture_seen = std::chrono::steady_clock::now();
    constexpr char capture_label[] = "Press a key (right-click clears)";
    gui::Button(capture_label, ImVec2(hotkey_button_width(capture_label), 0.f));
    if (gui::IsItemClicked(ImGuiMouseButton_Right)) {
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
    if (gui::Button(key_label.c_str(), ImVec2(hotkey_button_width(key_label.c_str()), 0.f))) {
      state->capturing_key = true;
      state->capture_seen = std::chrono::steady_clock::now();
    } else if (gui::IsItemClicked(ImGuiMouseButton_Right) && settings->enable_key != 0u) {
      settings->enable_key = 0u;
      changed = true;
    }
    if (gui::IsItemHovered()) {
      gui::SetTooltip("Click, then press a key. Right-click clears it.");
    }
  }

  // Plan 6 (v2 design §3.18, D1): the status card -- the first failing stage, its reason and its one fix/button.
  // Plan 7 (the controller's addition): a fixed-height region, sized for the card's worst case (a title, a
  // two-line wrapped reason, at most two fix bullets, and one button row), so a transient reason or bullet --
  // e.g. the "resizing" and "Running k of k+1 passes." lines a Pass Count change used to flash -- never
  // shifts the rows below it. Same in D3D12 and D3D11 (bridged): this is ordinary ImGui, backend-agnostic.
  constexpr float CARD_TEXT_LINES = 5.f;  // title + up to 2 reason lines + up to 2 fix bullets
  const StatusCard& card = view.card;
  widgets::BeginCard("##status_card", CARD_TEXT_LINES);
  widgets::DrawStatusCard(BuildCardView(card, view));
  switch (card.button) {
    case CardButton::NONE: break;
    case CardButton::RETRY_NOW:
      if (widgets::CardActionButton(view.dlss_latched ? "Retry now and clear the latch" : "Retry now")) {
        state->retry_now = true;
        if (view.dlss_latched) {
          settings->dlss_placement_blocked = false;
          settings->vulkan_native_nr = true;
          changed = true;
        }
      }
      break;
    case CardButton::CLEAR_LATCH:
      if (widgets::CardActionButton("Clear latch##card")) {
        settings->dlss_placement_blocked = false;
        settings->vulkan_native_nr = true;
        changed = true;
      }
      break;
  }
  widgets::EndCard();

  // Every row of one section. All its call sites are here in DrawOverlay, so it stays a local
  // lambda rather than a file-scope helper.
  const auto draw_rows = [&view, settings, state](SettingSection section) {
    bool section_changed = false;
    bool face_heading = false;
    for (const SettingDescriptor& descriptor : SettingsSchema()) {
      if (descriptor.section != section || descriptor.kind == SettingKind::KEY) {
        continue;
      }
      // Advanced's face controls start at Show the face mask; one sub-caption groups the tuning rows that
      // follow it (they are drawn only while Keep faces is on).
      if (section == SettingSection::ADVANCED && descriptor.key == "ShowFaceMask" && settings->keep_faces && !face_heading) {
        widgets::SubCaption("Face tuning");
        face_heading = true;
      }
      if (section == SettingSection::DLSS_REFLEX) {
        if (const char* const group = DlssReflexGroup(descriptor.key); group != nullptr) {
          widgets::SubCaption(group);
        }
      }
      section_changed |= DrawRow(descriptor, view, settings, state);
    }
    return section_changed;
  };
  // A section title's Reset (L1, P1, F1), shown only once the section has changed. Plan 6 (D6): with
  // Compare = Built-in, every row already reads read-only at its built-in value, so a reset must not
  // rewrite Mine's own settings while they are not even shown -- the callers drop it with that flag.

  // ui-review.md §2 finding 1 / §3: the NR stage and Motion vectors toggles, in pipeline order, at the top
  // of the panel -- the decisions that shape everything else, instead of about the 45th line. Plan 14: Setup.
  const bool placement_changed = SectionChanged(*settings, SettingSection::PLACEMENT);
  if (widgets::SectionCaption("Setup", "Reset", placement_changed && !state->defaults_view)) {
    ResetSection(settings, SettingSection::PLACEMENT);
    const Settings defaults = DefaultSettings();  // the three highlights follow at once, each to the best option that can run (the stage first)
    state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(SourcePickOf(defaults, view.dlss_explicit)), view.setup);
    state->setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPickOf(defaults)), view.setup, true);
    state->setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(defaults.resolution_mode), view.setup);
    changed = true;
  }
  changed |= DrawSourceRow(view, settings, state);
  changed |= DrawMotionRow(view, settings, state);
  changed |= DrawRow(*FindSetting("MaskBlendMode"), view, settings, state);
  changed |= DrawRow(*FindSetting("Preset"), view, settings, state);
  widgets::SubCaption("Resolution");
  changed |= DrawRow(*FindSetting("ResolutionMode"), view, settings, state);
  changed |= DrawRow(*FindSetting("ResolutionScale"), view, settings, state);
  // Anamorphic (2026-10-29): Custom's two axes apart. The switch draws under Custom only, and the height's own
  // slider under the switch, so the default Setup page reads exactly as it did before.
  if (settings->resolution_mode == ResolutionMode::CUSTOM) {
    changed |= DrawRow(*FindSetting("ResolutionAsymmetric"), view, settings, state);
    if (settings->resolution_asymmetric) changed |= DrawRow(*FindSetting("ResolutionScaleY"), view, settings, state);
  }
  changed |= DrawRow(*FindSetting("Upsampling"), view, settings, state);
  // Edge-aware's two band gains, right below the choice they depend on (DrawRow hides them unless it runs below Full).
  changed |= DrawRow(*FindSetting("GuidedBaseGain"), view, settings, state);
  changed |= DrawRow(*FindSetting("GuidedDetailGain"), view, settings, state);
  changed |= DrawRow(*FindSetting("DensityOnBase"), view, settings, state);
  changed |= DrawRow(*FindSetting("EdgeDensityAttenuation"), view, settings, state);
  // Foveated edge scaling (2026-10-21): the five rows below are the centre, its two fall-offs and the density they reach; they only matter while it is on.
  changed |= DrawRow(*FindSetting("Foveation"), view, settings, state);
  if (settings->foveation) {
    widgets::SubCaption("Centre and fall-off");
    changed |= DrawRow(*FindSetting("FoveationWidth"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationHeight"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationFalloffX"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationFalloffY"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationEdge"), view, settings, state);
  }

  // ui-review.md §2 finding 7: Compare (Mine/Built-in) moves here, the first row of Look.
  const bool look_changed = SectionChanged(*settings, SettingSection::LOOK);
  widgets::SectionGap();
  if (widgets::SectionCaption("Look", "Reset", look_changed && !state->defaults_view)) {
    ResetSection(settings, SettingSection::LOOK);
    changed = true;
  }
  gui::PushID("Compare");
  static constexpr std::array<const char*, 2> COMPARE_OPTIONS = {"Mine", "Built-in"};
  const std::array<std::string_view, 2> compare_why = {};
  const std::array<std::string_view, 2> compare_tips = {
      std::string_view(), "The built-in look, read-only; your own settings are kept."};
  if (const int clicked =
          RadioRow("Compare", COMPARE_OPTIONS, compare_why, compare_tips, (state->defaults_view ? 1 : 0));
      clicked >= 0) {
    state->defaults_view = (clicked == 1);
  }
  gui::PopID();
  changed |= draw_rows(SettingSection::LOOK);
  // Plan 5: pass 2..pass_count, each its own collapsible tree (P4-P13), so the per-pass rows do not
  // flood the panel for the common one-pass case. ui-review.md §5: the tree's own label shows its state.
  // Split chain (Task 3, design 6.2): Runs at first; on the first pass of a later stage group, that stage's Resolution (and Custom scale) and, while Keep
  // faces is on, its Keep faces here; while the chain is split the tree's label names where the pass runs.
  for (uint32_t pass = 2u; pass <= settings->pass_count; ++pass) {
    const bool follows = settings->passes[pass - 2u].follow_pass1;
    const PassStageRow& stage_row = view.setup.pass_stages[pass - 2u];
    std::string_view runs_at;
    if (view.setup.chain_split) {
      switch (stage_row.runs) {
        case SourcePick::BEFORE_UPSCALING: runs_at = " - Before upscaling"; break;
        case SourcePick::AFTER_DLSS:       runs_at = " - After DLSS"; break;
        case SourcePick::PRESENT:          runs_at = " - At Present"; break;
      }
    }
    const std::string tree_label =
        std::format("{}{}{}###pass{}", gui::Display(std::format("Pass {}", pass)), gui::Display(follows ? " (same look as pass 1)" : ""), gui::Display(runs_at), pass);
    if (gui::TreeNode(tree_label.c_str())) {
      const auto is_stage_row = [](const SettingDescriptor& descriptor) { return (descriptor.flags & setting_flags::PASS_STAGE) != 0u; };
      for (const SettingDescriptor& descriptor : SettingsSchema()) {
        if (descriptor.pass == pass && is_stage_row(descriptor)) {
          changed |= DrawPassStageRow(descriptor, view, settings);
        }
      }
      changed |= DrawRow(*FindSetting(std::format("Pass{}Preset", pass)), view, settings, state);
      if (stage_row.starts_group && stage_row.runs != SourcePick::BEFORE_UPSCALING) {  // Before upscaling is always pass 1's stage
        const bool after_dlss = (stage_row.runs == SourcePick::AFTER_DLSS);
        for (const std::string_view key : {(after_dlss ? "ResolutionAfterDlss" : "ResolutionPresent"), (after_dlss ? "ResolutionScaleAfterDlss" : "ResolutionScalePresent"),
                                           (after_dlss ? "KeepFacesAfterDlss" : "KeepFacesPresent")}) {
          changed |= DrawRow(*FindSetting(key), view, settings, state);
        }
      }
      for (const SettingDescriptor& descriptor : SettingsSchema()) {
        if (descriptor.pass == pass && !is_stage_row(descriptor) && (descriptor.flags & setting_flags::NETWORK_PRESET) == 0u) {
          changed |= DrawRow(descriptor, view, settings, state);
        }
      }
      gui::TreePop();
    }
  }

  // ui-review.md §2 finding 4: Result and Limits merge under one header, opened at start only for a
  // user who already shapes (SetNextItemOpen(look.enabled, ImGuiCond_Once) keeps the "stay usable" intent).
  const bool shaping_changed = SectionChanged(*settings, SettingSection::RESULT) || SectionChanged(*settings, SettingSection::LIMITS);
  gui::SetNextItemOpen(settings->look.enabled, ImGuiCond_Once);
  widgets::SectionGap();
  bool shaping_reset = false;
  const bool shaping_open =
      widgets::SectionHeader("Result shaping", "changed", "Reset", shaping_changed && !state->defaults_view, &shaping_reset);
  if (shaping_reset) {
    ResetSection(settings, SettingSection::RESULT);
    ResetSection(settings, SettingSection::LIMITS);
    changed = true;
  }
  if (shaping_open) {
    changed |= draw_rows(SettingSection::RESULT);
    if (settings->look.enabled) {
      widgets::SubCaption("Limits and stability");
    }
    changed |= draw_rows(SettingSection::LIMITS);
  }

  // ui-review.md §2 finding 5: Mask and UI correction, which handle the HUD, get their own section.
  const bool mask_changed = SectionChanged(*settings, SettingSection::MASK);
  bool mask_reset = false;
  widgets::SectionGap();
  const bool mask_open =
      widgets::SectionHeader("Mask and HUD", "changed", "Reset", mask_changed && !state->defaults_view, &mask_reset);
  if (mask_reset) {
    ResetSection(settings, SettingSection::MASK);
    changed = true;
  }
  if (mask_open) {
    changed |= draw_rows(SettingSection::MASK);
  }

  const bool fixes_changed = SectionChanged(*settings, SettingSection::FIXES_COLOR)
                             || SectionChanged(*settings, SettingSection::FIXES_GUIDES);
  bool fixes_reset = false;
  widgets::SectionGap();
  const bool fixes_open =
      widgets::SectionHeader("Fixes", "changed", "Reset", fixes_changed && !state->defaults_view, &fixes_reset);
  if (fixes_reset) {
    ResetSection(settings, SettingSection::FIXES_COLOR);
    ResetSection(settings, SettingSection::FIXES_GUIDES);
    changed = true;
  }
  if (fixes_open) {
    gui::TextDisabled("Only change these if you know what you are doing.");
    widgets::SubCaption("Colour");
    changed |= draw_rows(SettingSection::FIXES_COLOR);
    widgets::SubCaption("Motion and game state");
    changed |= draw_rows(SettingSection::FIXES_GUIDES);
  }

  // The live DLSS, frame-generation and Reflex overrides: the switches players come to the panel for,
  // not the diagnostics and tuning that stay in Advanced.
  const bool dlss_reflex_changed = SectionChanged(*settings, SettingSection::DLSS_REFLEX);
  bool dlss_reflex_reset = false;
  widgets::SectionGap();
  const bool dlss_reflex_open =
      widgets::SectionHeader("DLSS / Reflex", "changed", "Reset", dlss_reflex_changed && !state->defaults_view, &dlss_reflex_reset);
  if (dlss_reflex_reset) {
    ResetSection(settings, SettingSection::DLSS_REFLEX);
    changed = true;
  }
  if (dlss_reflex_open) {
    changed |= draw_rows(SettingSection::DLSS_REFLEX);
    // What actually runs, gathered in one place at the bottom of the card: a setting row says what was
    // asked for, and these lines say what the runtime is doing with it.
    const std::array<const std::string*, 7> live = {&view.dlss_live_line,      &view.rr_live_line,      &view.fg_live_line,
                                                    &view.fg_multiplier_line,  &view.dynamic_mfg_line,  &view.frame_rate_line,
                                                    &view.reflex_live_line};
    if (std::ranges::any_of(live, [](const std::string* line) { return !line->empty(); })) {
      widgets::SubCaption("Currently");
      for (const std::string* line : live) {
        if (!line->empty()) {
          widgets::RowNote(line->c_str());
        }
      }
    }
  }

  widgets::SectionGap();
  if (widgets::SectionHeader("Advanced", nullptr, nullptr, false, nullptr)) {
    changed |= draw_rows(SettingSection::ADVANCED);
  }

  // ui-review.md §2 finding 2: the wall of status text moves into one closed Details header. It always lists
  // everything, so its content does not depend on the card's state; a working card repeats its details lines.
  if (widgets::SectionHeader("Details", nullptr, nullptr, false, nullptr)) {
    gui::TextUnformatted(view.status_line.c_str());
    if (!view.network_diagnostic.empty()) {
      gui::TextWrapped("%s", view.network_diagnostic.c_str());
      gui::TextDisabled("Calls count successful command recordings; see ReShade.log for each feature and network change.");
    }
    gui::TextWrapped("Native inputs: depth and bidirectional distortion are not bound. Native guided upsampling is not enabled; reduced resolution uses Uplift's own upsampling.");
    gui::TextWrapped("Native mask blend mode defaults to 1. Full mask-combination behavior on the Ada compatibility runtime remains under validation. Global tone and native Performance have no verified effect.");
    // Plan 14: the engine's own lines, which the rows no longer show under themselves.
    for (const std::string* line : {&view.placement_line, &view.chain_line, &view.dlss_mode_line, &view.motion_line, &view.work_line, &view.exposure_line}) {
      if (!line->empty()) {
        gui::TextUnformatted(line->c_str());
      }
    }
    if (!view.frame_generation_line.empty()) {
      gui::TextUnformatted(view.frame_generation_line.c_str());
    }
    if (!view.api_line.empty()) {
      gui::TextUnformatted(view.api_line.c_str());
    }
    gui::TextDisabled("%s", VramLine(view).c_str());
  }

  // G2: Restore all defaults, confirmed once before it takes effect.
  widgets::SectionGap();
  gui::Separator();
  if (gui::Button("Restore all defaults")) {
    state->confirm_restore = true;
  }
  if (state->confirm_restore) {
    gui::TextWrapped("Every setting returns to its default, the hotkey included; Enable stays as it is.");
    bool restore_all = false;
    {
      style::StyleScope confirm;
      confirm.DangerButton();
      restore_all = gui::Button("Restore all");
    }
    if (restore_all) {
      RestoreAllDefaults(settings);
      changed = true;
      state->confirm_restore = false;
    }
    gui::SameLine();
    if (gui::Button("Keep my settings")) {
      state->confirm_restore = false;
    }
  }

  changed |= DrawEnableWarning(*FindSetting("PresentWithFrameGen"), settings, state);
  changed |= DrawEnableWarning(*FindSetting("AllowCrossQueuePasses"), settings, state);
  return changed;
}

}  // namespace uplift::ui
