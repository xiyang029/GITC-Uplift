#include "ui/overlay.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addon/reshade_api.hpp"
#include "memory_units.hpp"
#include "ui/settings_schema.hpp"
#include "nr/foveation.hpp"
#include "sources/work_size.hpp"
#include "ui/overlay_widgets.hpp"

namespace uplift::ui {
namespace {

// The tooltip line of a highlighted option while the highlight is not the stored preference (what runs differs from what ReShade.ini holds).
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

// Only these two rows need to scan the stage groups for a reduced working grid.
bool UpsamplingRowHidden(std::string_view key, const SetupView& setup, const Settings& settings) {
  if (key != "Upsampling" && key != "GuidedBaseGain") return false;
  const auto reduced = [](ResolutionMode mode, float percent, float percent_y) {
    return mode != ResolutionMode::FULL && (mode != ResolutionMode::CUSTOM || percent < 100.f || percent_y < 100.f);
  };
  const auto later_group_scaled = [&](const PassStageRow& row) {
    return row.starts_group
           && ((row.runs == SourcePick::AFTER_DLSS && reduced(settings.resolution_mode_after_dlss, settings.resolution_scale_after_dlss, settings.resolution_scale_after_dlss))
               || (row.runs == SourcePick::PRESENT && reduced(settings.resolution_mode_present, settings.resolution_scale_present, settings.resolution_scale_present)));
  };
  const bool reduced_any =
      settings.foveation || reduced(settings.resolution_mode, settings.resolution_scale,
                                     AsymmetricAxes(settings) ? settings.resolution_scale_y : settings.resolution_scale)
      || std::ranges::any_of(setup.pass_stages, later_group_scaled);
  return !reduced_any || (key == "GuidedBaseGain" && settings.upsampling != color::Upsampling::EDGE_AWARE);
}

// Only keys and input history survive the frame. The detail picker may capture this draw's local values.
struct DetailFocus {
  struct Option {
    std::string label;
    std::string why;
  };
  std::string key;
  std::string title;
  std::string body;
  std::string why;
  std::vector<Option> options;
  int option_value = 0;
  std::function<bool(int)> pick;
};

struct DetailFrame {
  enum class Source { NONE, RETAINED, NAVIGATION, POINTER, ACTIVATION };
  DetailFocus focus;
  Source source = Source::NONE;
  ImGuiID nav_choice = 0;
  std::string_view hovered_row;

  bool Select(const OverlayState& state, std::string_view key, bool hovered, ImGuiID nav_id = 0, bool activated = false) {
    if (hovered) hovered_row = key;
    if (nav_id != 0) nav_choice = nav_id;
    const ImGuiIO& io = gui::GetIO();
    const bool pointer_changed = hovered && (state.panel_hovered_row != key || io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f);
    const bool nav_changed = nav_id != 0 && nav_id != state.panel_nav_choice && io.NavVisible;
    const Source candidate = activated ? Source::ACTIVATION : pointer_changed ? Source::POINTER
                             : nav_changed ? Source::NAVIGATION : key == state.panel_focus ? Source::RETAINED : Source::NONE;
    if (candidate <= source) return false;
    source = candidate;
    return true;
  }
};

DetailFrame& PendingDetail() {
  static thread_local DetailFrame frame;
  return frame;
}

// Both columns use the same setter. Choice details are rebuilt from this row's live value and
// availability, including when the pointer has moved over to the right column.
bool DrawChoice(OverlayState& state, std::string_view key, const char* title, const char* const* names,
                int count, int value, const std::string_view* reasons, std::string_view body,
                std::string_view disabled, std::function<bool(int)> apply) {
  gui::BeginDisabled(!disabled.empty());
  // ChoiceRow owns one invisible hit item. Give each setting its own hidden ID so rows in the same tab do not collide.
  const std::string choice_id = std::string(key) + "##choice_row";
  const auto hit = widgets::ChoiceRow(choice_id.c_str(), title, names, count, value, reasons);
  gui::EndDisabled();
  auto& frame = PendingDetail();
  if (frame.Select(state, key, hit.hovered, hit.nav_id, hit.activated)) {
    auto& detail = frame.focus;
    detail = {std::string(key), gui::Visible(title), std::string(body), std::string(disabled), {}, value, {}};
    detail.options.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      const auto reason = !disabled.empty() ? disabled : reasons != nullptr ? reasons[i] : std::string_view();
      detail.options.push_back({names[i], std::string(reason)});
    }
    if (disabled.empty()) detail.pick = apply;
  }
  return disabled.empty() && hit.picked >= 0 && apply(hit.picked);
}

// Draw a settings-schema row with its normal availability, reset and commit rules.
bool DrawRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state,
             const char* label_override = nullptr) {
  if (descriptor.key == "BeforeFGMotion" && (settings->fg_present != Settings::FgPresent::BEFORE
      || (settings->motion_vectors != MotionVectorSource::LAUNCHPAD && settings->motion_vectors != MotionVectorSource::LUMENITE))) return false;
  std::string label(label_override ? std::string_view(label_override) : descriptor.label);
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
  // The change field's own stabiliser is not part of shaping: it runs with Shape result off, so its rows stay (LIMITS section or not).
  const bool change_row = (descriptor.key == "StabilizeChange" || descriptor.key == "StabilizeChangeMs");
  const bool shaping_off = ((descriptor.section == SettingSection::RESULT || descriptor.section == SettingSection::LIMITS)
                            && descriptor.key != "ShapeResult" && !change_row && !settings->look.enabled);
  const bool stabilize_off = ((descriptor.key == "StabilizeMs" || descriptor.key == "StabilizeDetail")
                              && settings->look.stabilize == look::StabilizeMode::OFF);
  // The Adaptation rows only where the meter can be used; Auto's own rows only under Auto.
  const bool exposure_hidden = ExposureRowHidden(descriptor, *settings);
  // Split chain: a later stage group's Custom scale as Setup's.
  const bool resolution_scale_hidden =
      ((descriptor.key == "ResolutionScale" && settings->resolution_mode != ResolutionMode::CUSTOM)
       || (descriptor.key == "ResolutionScaleAfterDlss" && settings->resolution_mode_after_dlss != ResolutionMode::CUSTOM)
       || (descriptor.key == "ResolutionScalePresent" && settings->resolution_mode_present != ResolutionMode::CUSTOM));
  // Keep faces' own rows, and (fix round 1 addendum) its mask's tuning in Advanced, only while it is on.
  const bool keep_faces_tuning = descriptor.key.starts_with("FaceMask") || descriptor.key.starts_with("FaceFill") || descriptor.key == "FaceEdgeFalloff"
                                 || descriptor.key == "FaceLightingDensity" || descriptor.key == "FaceSpeckSize";
  // Split chain: a later stage's Keep faces here too.
  const bool keep_faces_here = (descriptor.key == "KeepFacesAfterDlss" || descriptor.key == "KeepFacesPresent");
  const bool keep_faces_off =
      ((descriptor.key == "FaceProtection" || descriptor.key == "LightingScale" || keep_faces_tuning || keep_faces_here) && !settings->keep_faces);
  // The fill's radius and tolerance only while it is on.
  const bool fill_off = ((descriptor.key == "FaceFillRadius" || descriptor.key == "FaceFillTolerance") && !settings->face_tuning.fill_skin);
  // These rows matter only after a choice in the row just above them, so they
  // are hidden -- not merely greyed -- until that choice is made.
  if (pass_follows || shaping_off || stabilize_off || exposure_hidden || resolution_scale_hidden || UpsamplingRowHidden(descriptor.key, view.setup, *settings)
      || keep_faces_off || fill_off) {
    return false;
  }

  gui::PushID(descriptor.key.data());
  // The row's note, if it has one, drawn once the reset arrow is placed: RowIconButton centres the arrow on the
  // row's last item, and a note is a line of its own, so drawing it earlier took the arrow down a line on the five
  // rows that have one -- Pass count's frame-generation warning, Mask, UICorrection, UseD3D9Ex and VulkanNr.
  std::string deferred_note;
  bool deferred_note_wraps = false;
  const ImVec2 row_top = gui::GetCursorScreenPos();  // the row's own top: its tooltip is asked for over the whole row
  // On the press frame the control can start blocking window hover: sample before submitting it as well.
  const bool mouse_in_column = descriptor.kind != SettingKind::CHOICE && gui::IsWindowHovered();
  // ui-review.md §3 rule 1 / §2 finding 5: Game state after NR only matters once NR can run inside the
  // game's frame; while no DLSS placement can run this session, it is greyed and says why.
  // The Direct3D 9Ex row is greyed the same way where it does nothing (every game but a Direct3D 9 one).
  // The VulkanNr row likewise, everywhere but a 64-bit Vulkan game.
  // The Diffuse white row on an sRGB image, where it does nothing (and the Manual exposure choice with it, below), and the Colour conversion rows
  // that do nothing on the shown encoding (ColorFixUnavailable).
  // Keep faces: its rows where it cannot run, and the face mask's view (in Advanced, far from the switch) while it is off.
  const bool keep_faces_row = (descriptor.key == "KeepFaces" || descriptor.key == "FaceProtection" || descriptor.key == "LightingScale"
                               || descriptor.key == "ShowFaceMask" || keep_faces_tuning || keep_faces_here);
  const std::string_view white_unavailable = DiffuseWhiteUnavailable(view.encoding);
  const std::string_view fix_unavailable = ColorFixUnavailable(descriptor.key, view.encoding);
  const std::string_view disabled_reason =
      ((descriptor.flags & setting_flags::NETWORK_PRESET) != 0u && view.legacy_runtime)                   ? std::string_view("NR 310.8 only provides the Legacy network.")
      : (descriptor.key == "MaskBlendMode" && view.legacy_runtime)            ? std::string_view("NR 310.8 does not support native mask blend mode.")
      : (descriptor.key == "StateRestore" && !view.dlss_unavailable.empty())   ? std::string_view(view.dlss_unavailable)
      : (descriptor.key == "UseD3D9Ex" && !view.d3d9ex_unavailable.empty())    ? std::string_view(view.d3d9ex_unavailable)
      : (descriptor.key == "VulkanNr" && !view.vulkan_nr_unavailable.empty()) ? std::string_view(view.vulkan_nr_unavailable)
      : (descriptor.key == "DiffuseWhiteNits" && !white_unavailable.empty())  ? white_unavailable
      : (!fix_unavailable.empty())                                               ? fix_unavailable
      : (descriptor.key == "AllowCrossQueuePasses" && !view.cross_queue_passes_available) ? std::string_view("Only for 64-bit Direct3D 12 games")
      : (keep_faces_row && !view.keep_faces_unavailable.empty())               ? std::string_view(view.keep_faces_unavailable)
      : (descriptor.key == "ShowFaceMask" && !settings->keep_faces)            ? std::string_view("Turn on Keep faces (Look) first")
                                                                               : std::string_view();
  gui::BeginDisabled(!disabled_reason.empty() || read_only);
  bool changed = false;
  if (descriptor.key == "SkinStructure") {
    bool follows = (settings->skin_structure < 0.f);
    widgets::RowLabel("Skin follows Structure");
    if (widgets::Switch("##SkinFollows", &follows)) {
      settings->skin_structure = (follows ? SKIN_SAME_AS_STRUCTURE : settings->local_structure);
      changed = true;
    }
  }
  if (descriptor.key == "DiffuseWhiteNits") {
    bool automatic = (settings->diffuse_white_nits <= 0.f);
    widgets::RowLabel("Automatic diffuse white");
    if (widgets::Switch("##AutoDiffuseWhite", &automatic)) {
      settings->diffuse_white_nits = (automatic ? 0.f : std::clamp(view.effective_diffuse_white_nits, 48.f, 500.f));
      changed = true;
    }
  }
  // A choice row draws its own caption inside ChoiceRow, so it needs no caption line of its own.
  const bool captioned = descriptor.kind != SettingKind::BOOL && descriptor.kind != SettingKind::CHOICE;
  const std::string caption = descriptor.key == "ResolutionMode" ? "Working resolution"
                            : descriptor.key == "ResolutionScale" ? "Custom scale"
                                                                  : label;
  if (captioned) {
    // Two rows (Skin follows Structure, Automatic diffuse white) drew their own label and switch just above:
    // their control goes on the line under it, from the row's control column to its end slot, and still names
    // itself there -- the switch's words say "follows", the number below has to say what it is.
    // Nothing is disabled around that label: the only row that can be inherited is SkinStructure, whose own
    // switch already carried the disable when it was drawn above.
    widgets::RowLabel(caption.c_str());
    label = "##value";
  }
  if (descriptor.key == "SkinStructure") {
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
    // An Automatic checkbox, the control greyed at the value
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
        widgets::RowLabel(descriptor.label.data());
        if (widgets::Switch(label.c_str(), &value)) {
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
        const bool typed = descriptor.max > 100.0 && (descriptor.flags & setting_flags::SLIDER) == 0u;  // past a slider's range: a typed field, whose own held state is the release below
        // The render scales are shares of the output, so their number carries the sign: the label names the row
        // ("Quality render scale"), the value says what it is in.
        const char* const format = IntPercentSetting(descriptor.key) ? "%d %%" : "%d";
        const bool edited = typed ? widgets::NumberField(label.c_str(), &value, descriptor.step, descriptor.fast_step, &active)
                                  : widgets::SliderInt(label.c_str(), &value, low, high, &active, format);
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
        // Slider converts the stored value; its bounds are in display units.
        const float shown_scale = static_cast<float>(descriptor.display_scale);
        float value = static_cast<float>(descriptor.get(*settings));
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
          descriptor.set(*settings, static_cast<double>(value));
          changed = true;
        }
        if (descriptor.key == "LocalStructure") {
          state->drag.local_structure = active;
        } else if (descriptor.key == "LocalTone") {
          state->drag.local_tone = active;
        } else if (descriptor.key == "ResolutionScale") {
          state->drag.resolution_scale = active;
        } else if (descriptor.key == "ResolutionScaleY") {
          state->drag.resolution_scale_y = active;
        } else if (descriptor.key == "ResolutionScaleAfterDlss") {
          state->drag.resolution_scale_after_dlss = active;  // split chain: the work size waits for the release, as Setup's
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
        const int count = static_cast<int>(descriptor.choices.size());
        if (count > 0) {
          std::vector<const char*> choices(static_cast<size_t>(count));
          for (int index = 0; index < count; ++index) {
            choices[static_cast<size_t>(index)] = descriptor.choices[static_cast<size_t>(index)].data();  // literals: NUL-terminated
          }
          // One reason per option; an option with one is stepped over, never landed on (the schema carries no
          // per-option text, so the column's radio list says what each choice is and this says which can run).
          std::vector<std::string_view> why(static_cast<size_t>(count), disabled_reason);
          int shown = std::clamp(value, 0, count - 1);
          std::string note;
          if (descriptor.key == "ResolutionMode" || descriptor.key == "ResolutionAfterDlss" || descriptor.key == "ResolutionPresent") {
            // The entries that cannot run now carry their reason, and the preview is the mode that runs.
            // Split chain: a later stage group's Resolution greys the same entries (Match game reads the same DLSS render size),
            // but its preview is its own stored mode, since Setup's highlight is pass 1's.
            const bool setup_row = (descriptor.key == "ResolutionMode");
            const size_t stored_index = std::min(static_cast<size_t>(value), static_cast<size_t>(count - 1));
            const size_t highlighted = std::min(setup_row ? static_cast<size_t>(view.setup.resolution) : stored_index,
                                               static_cast<size_t>(count - 1));
            shown = static_cast<int>(highlighted);
            const OptionState& stored = view.setup.resolution_options[stored_index];
            if (highlighted != stored_index && (stored.why.empty() || stored.temporary)) {  // a fixed cause never clears: no promise
              note = SavedNote(choices[stored_index]);
            }
            for (int index = 0; index < count; ++index) {
              const OptionState& option = view.setup.resolution_options[static_cast<size_t>(index)];
              if (!option.why.empty() && (setup_row || static_cast<size_t>(index) != highlighted)) {
                why[static_cast<size_t>(index)] = option.why;
              }
            }
          } else if (descriptor.key == "PresentWithFrameGen") {
            why[static_cast<size_t>(Settings::FgPresent::BEFORE)] = view.before_fg_unavailable;
          } else if (descriptor.key == "VulkanNr" && disabled_reason.empty()) {
            // Native greyed where it cannot run, with why; the highlight is the route that runs. Direct3D 12 too.
            why[static_cast<size_t>(VulkanNrMode::NATIVE)] = view.vulkan_native_unavailable;
            why[static_cast<size_t>(VulkanNrMode::DIRECT3D_12)] = view.vulkan_d3d12_unavailable;
            if (view.vulkan_nr_running) {
              shown = static_cast<int>(*view.vulkan_nr_running);
            }
          }
          const std::string body = note.empty() ? std::string(descriptor.tooltip)
                                                 : std::format("{}\n{}", descriptor.tooltip, note);
          const auto blocked = read_only ? std::string_view("Built-in settings are read-only.") : disabled_reason;
          changed |= DrawChoice(*state, descriptor.key, caption.c_str(), choices.data(), count, shown, why.data(), body, blocked,
              [settings, state, row = &descriptor](int picked) {
                if (row->key == "PresentWithFrameGen" && picked == static_cast<int>(Settings::FgPresent::AFTER)) {
                  state->confirm_present_stage = false;
                  state->enable_warning_key = row->key;
                  return false;
                }
                row->set(*settings, static_cast<double>(picked));
                if (row->key == "ResolutionMode") state->setup_settle.Clicked(SetupRow::RESOLUTION, static_cast<uint32_t>(picked));
                return true;
              });
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
  if (descriptor.kind != SettingKind::CHOICE) {
    // Test the full row: the last ImGui item may only be the slider value. Exclude the reset cell.
    const float row_bottom = gui::GetCursorScreenPos().y - gui::GetStyle().ItemSpacing.y;
    const float row_right = (std::min)(widgets::row_slot_x, gui::GetWindowPos().x + gui::GetWindowSize().x);
    const bool over_row = (mouse_in_column || gui::IsWindowHovered())
                          && gui::IsMouseHoveringRect(ImVec2(gui::GetWindowPos().x, row_top.y), ImVec2(row_right, row_bottom));
    auto& frame = PendingDetail();
    if (frame.Select(*state, descriptor.key, over_row)) {
      frame.focus = {std::string(descriptor.key), gui::Visible(caption), std::string(descriptor.tooltip),
                     std::string(disabled_reason), {}, 0, {}};
    }
    if (over_row && !disabled_reason.empty()) {
      widgets::MouseTooltip(gui::GetID("##row-tooltip"), "%s", std::format("Greyed: {}", disabled_reason).c_str());
    }
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

bool DrawEnableWarning(Settings* settings, OverlayState* state, bool explicit_dlss) {
  if (state->enable_warning_key.empty()) return false;
  const auto& descriptor = *FindSetting(state->enable_warning_key);
  const bool frame_gen = descriptor.key == "PresentWithFrameGen";
  // The visible title can be translated without changing the ImGui ID.
  const std::string title = std::format("{} - performance warning###enable_warning_{}", descriptor.label, descriptor.key);
  bool open = true;
  const bool enable = widgets::ConfirmDialog(title.c_str(), descriptor.enable_warning,
      frame_gen ? "Keep current mode" : "Keep disabled", "Enable anyway", open, 96.f);
  if (!open) state->enable_warning_key = {};
  if (enable) {
    descriptor.set(*settings, 1.0);
    if (frame_gen && state->confirm_present_stage) {
      SetSourcePick(settings, SourcePick::PRESENT, explicit_dlss);
      state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(SourcePick::PRESENT));
    }
  }
  return enable;
}

// Later passes get their stage and availability from the resolved chain.
bool DrawPassStageRow(const SettingDescriptor& descriptor, const OverlayView& view, Settings* settings, OverlayState* state) {
  const auto& row = view.setup.pass_stages[descriptor.pass - 2u];
  const auto stored = static_cast<size_t>(settings->passes[descriptor.pass - 2u].stage);
  const auto shown = static_cast<size_t>(row.shown);
  const std::array names{descriptor.choices[0].data(), NR_STAGE_NAMES[0], NR_STAGE_NAMES[1], view.SharedPresentName(*settings)};
  const std::string label(descriptor.label);
  std::string tooltip(descriptor.tooltip);
  if (shown != stored && (row.options[stored].why.empty() || row.options[stored].temporary)) {
    tooltip += "\n" + SavedNote(names[stored]);
  }
  std::array<std::string_view, 4> why{};
  for (size_t i = 0; i < why.size(); ++i) why[i] = row.options[i].why;
  const std::string_view blocked = !view.setup.split_capable ? ONE_STAGE_REASON : view.setup.stopped ? "NR has stopped" : "";
  gui::PushID(descriptor.key.data());
  bool changed = DrawChoice(*state, descriptor.key, label.c_str(), names.data(), static_cast<int>(names.size()),
                            static_cast<int>(shown), why.data(), tooltip, blocked,
      [settings, row = &descriptor, stored](int picked) {
        if (static_cast<size_t>(picked) == stored) return false;
        row->set(*settings, static_cast<double>(picked));
        return true;
      });
  gui::BeginDisabled(!blocked.empty());
  changed |= DrawReset(descriptor, *settings, *state, view.setup);
  gui::EndDisabled();
  gui::PopID();
  return changed;
}

// The Motion vectors row -- Off / DLSS / Launchpad / Lumenite -- from Setup, as the NR stage row.
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
  std::string tip_note;
  const OptionState& stored_motion = view.setup.motion_options[static_cast<size_t>(preferred)];
  if (view.setup.motion != preferred && (stored_motion.why.empty() || stored_motion.temporary)) {  // a fixed cause never clears: no promise
    const std::string_view stored = (settings->motion_vectors == MotionVectorSource::AUTO ? std::string_view("Auto") : std::string_view(OPTIONS[static_cast<size_t>(preferred)]));
    tip_note = SavedNote(stored);
  }
  // The row kept the preference because this frame's copy is only in flight (Setup's note): say so here, where the row says nothing.
  if (!view.setup.motion_note.empty()) {
    if (!tip_note.empty()) tip_note += "\n";
    tip_note += view.setup.motion_note;
  }
  if (!tip_note.empty()) {
    tip_note = std::format("{}\n{}", tips[static_cast<size_t>(shown)], tip_note);
    tips[static_cast<size_t>(shown)] = tip_note;
  }
  bool changed = DrawChoice(*state, "MotionVectors", "Motion vectors", OPTIONS.data(), static_cast<int>(OPTIONS.size()),
                            shown, why.data(), tips[static_cast<size_t>(shown)], {},
      [settings, state](int picked) {
        SetMotionPick(settings, static_cast<MotionPick>(picked));
        state->setup_settle.Clicked(SetupRow::MOTION, static_cast<uint32_t>(picked));
        return true;
      });
  changed |= DrawReset(*FindSetting("MotionVectors"), *settings, *state, view.setup);
  gui::PopID();
  return changed;
}

// The NR stage row -- Pre-DLSS / After DLSS / Before Framegen / On Present -- as one ordinary choice row.
// It replaced the pipeline diagram (2026-11-23): the six drawn nodes said nothing these four names do not,
// and the diagram took a third of the card. The last two names share SourcePick::PRESENT, so Present while
// frame generation is on goes through the same warning dialog the diagram opened, because it changes what
// the generated frames see.
bool DrawStageRow(const OverlayView& view, Settings* settings, OverlayState* state) {
  static constexpr std::array<const char*, 4> TIPS = {
      "Run pass 1 on the game's render-resolution input before DLSS upscales it.",
      "Run pass 1 on the upscaled image before the game's remaining post-processing.",
      "Run Present passes on the finished real frame before Streamline frame generation.",
      "Run Present passes at ReShade's Present placement. With downstream ReShade this includes generated frames."};
  gui::PushID("Source");
  bool changed = false;
  const bool before_framegen = view.frame_generation_active && settings->fg_present == Settings::FgPresent::BEFORE;
  const SourcePick preferred = SourcePickOf(*settings, view.dlss_explicit);
  const size_t shown = preferred == SourcePick::PRESENT ? (before_framegen ? 2u : 3u) : static_cast<size_t>(preferred);
  std::array<std::string_view, 4> why = {};
  for (size_t index = 0u; index < why.size(); ++index) {
    const auto source = static_cast<SourcePick>((std::min)(index, size_t{2}));
    std::string_view reason = view.setup.stage_options[static_cast<size_t>(source)].why;
    // Present's "turn Present with frame generation on" note explains a switch, not this row's own choice.
    if (source == SourcePick::PRESENT && reason == FG_PRESENT_OFF_REASON) reason = {};
    // Before Framegen has no setting of its own: what blocks it is the game's Streamline integration.
    if (index == 2u && !view.before_fg_unavailable.empty()) reason = view.before_fg_unavailable;
    why[index] = reason;
  }
  // Owned, because the stored-preference note is appended to the tip and a view into the array would dangle.
  std::array<std::string, 4> tip_text = {};
  std::array<std::string_view, 4> tips = {};
  for (size_t index = 0u; index < tip_text.size(); ++index) {
    tip_text[index] = (index == 0u && view.setup.before_upscaling_on_rr) ? "NR works on Ray Reconstruction's input." : TIPS[index];
  }
  // What runs differs from what is stored (a temporary fallback): say what is stored, as the Motion row does.
  const OptionState& stored = view.setup.stage_options[static_cast<size_t>(preferred)];
  if (view.setup.stage != preferred && (stored.why.empty() || stored.temporary)) {
    tip_text[shown] += std::format("\n{}", SavedNote(StageLabel(preferred, before_framegen)));
  }
  for (size_t index = 0u; index < tips.size(); ++index) {
    tips[index] = tip_text[index];
  }
  changed |= DrawChoice(*state, "Source", "NR stage", NR_STAGE_NAMES.data(), static_cast<int>(NR_STAGE_NAMES.size()),
                        static_cast<int>(shown), why.data(), tips[shown], view.setup.stopped ? "NR has stopped" : "",
      [&view, settings, state](int picked) {
        const auto source = static_cast<SourcePick>((std::min)(picked, 2));
        if (picked == 3 && view.frame_generation_active && settings->fg_present != Settings::FgPresent::AFTER) {
          state->confirm_present_stage = true;
          state->enable_warning_key = "PresentWithFrameGen";
          return false;
        }
        SetSourcePick(settings, source, view.dlss_explicit);
        if (picked == 2) settings->fg_present = Settings::FgPresent::BEFORE;
        state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(source));
        return true;
      });
  if (!IsDefault(*settings, *FindSetting("Source")) || !IsDefault(*settings, *FindSetting("PreUpscale"))) {
    if (widgets::ResetButton("Source", view.dlss_explicit ? "Present on Vulkan" : "After DLSS")) {
      const SourcePick pick = view.dlss_explicit ? SourcePick::PRESENT : SourcePick::AFTER_DLSS;
      SetSourcePick(settings, pick, view.dlss_explicit);
      state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(pick), view.setup);
      changed = true;
    }
  }
  gui::PopID();
  return changed;
}

// The target row shows the grid that will actually run: with edge scaling on, the warped extent.
nr::Size FovAdjustedSize(nr::Size uniform, const OverlayView& view, const Settings& settings) {
  if (!settings.foveation || uniform.Empty()) return uniform;
  const nr::Size out = view.output_size.Empty() ? view.first_stage_input : view.output_size;
  if (out.Empty()) return uniform;
  const nr::foveation::Plan plan = sources::FoveatedWorkPlan(
      settings.foveation,
      {settings.foveation_width, settings.foveation_height, settings.foveation_falloff_x, settings.foveation_falloff_y, settings.foveation_edge_x, settings.foveation_edge_y},
      out, uniform);
  return {plan.width, plan.height};
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
    size = sources::WorkSize(input.width * scale, input.height * scale_y, input);
    size = FovAdjustedSize(size, view, settings);
  } else if (size.Empty() && settings.resolution_mode == ResolutionMode::FULL && view.setup.stage == SourcePick::PRESENT) {
    size = input;
    size = FovAdjustedSize(size, view, settings);
  }
  const bool pending = !size.Empty() && !view.first_stage_canvas.Empty() && size != view.first_stage_canvas;
  widgets::NextGap(style::BlockGap());
  const std::string dimensions = size.Empty() ? gui::Display("Unknown") : std::format("{} × {}", size.width, size.height);
  const float right = gui::GetCursorScreenPos().x + gui::GetContentRegionAvail().x - style::RowGap() * 1.5f;
  widgets::Readout(pending ? "Target NR size" : "NR working size", dimensions.c_str(), right);
  if (!view.output_size.Empty()) {
    const std::string output = std::format("{} × {}", view.output_size.width, view.output_size.height);
    widgets::NextGap(style::RowGap());
    widgets::Readout("Output", output.c_str(), right);
  }
}

// The DLSS / Reflex tab's sub-captions: the first row of each group names the group. The rows are drawn in
// schema order, so these keys are where a group begins.
constexpr const char* DlssReflexGroup(std::string_view key) {
  if (key == "DLSSPresetDLAA") return "DLSS presets";
  if (key == "DLSSRenderScaleDLAA") return "DLSS render scale";
  if (key == "DLSSFGPreset") return "Frame generation";
  if (key == "ReflexMode") return "Reflex";
  if (key == "RayReconstructionPreset") return "Ray Reconstruction";
  if (key == "XmxGenerator") return "XMX";
  return nullptr;
}

bool DrawDlssReflexTab(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  const char* group = nullptr;
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    if (descriptor.section != SettingSection::DLSS_REFLEX || descriptor.kind == SettingKind::KEY) continue;
    if (const char* const next = DlssReflexGroup(descriptor.key); next != nullptr && next != group) {
      if (group != nullptr) widgets::NextGap(style::BlockGap());
      widgets::GroupHeading(next);
      group = next;
    }
    const char* label = nullptr;
    if (descriptor.key == "XmxGenerator") label = "Intel XeFG generator";
    if (descriptor.key == "ReflexMode") label = "Mode";
    if (descriptor.key == "ReflexFpsCap") label = "FPS cap";
    changed |= DrawRow(descriptor, view, settings, state, label);
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
      if (!line->empty()) widgets::SecondaryWrapped(line->c_str());
    }
  }
  return changed;
}

bool DrawSetupGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  widgets::GroupHeading("Frame inputs");
  changed |= DrawMotionRow(view, settings, state);
  changed |= DrawRow(*FindSetting("MaskBlendMode"), view, settings, state);
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Passes");
  changed |= DrawRow(*FindSetting("PassCount"), view, settings, state);
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("NR processing", "Pass 1 defaults");
  changed |= DrawRow(*FindSetting("Preset"), view, settings, state);
  changed |= DrawRow(*FindSetting("ResolutionMode"), view, settings, state);
  changed |= DrawRow(*FindSetting("ResolutionScale"), view, settings, state);
  // Anamorphic (2026-10-29): Custom's two axes apart, under the mode they belong to.
  if (settings->resolution_mode == ResolutionMode::CUSTOM) {
    changed |= DrawRow(*FindSetting("ResolutionAsymmetric"), view, settings, state);
    if (settings->resolution_asymmetric) changed |= DrawRow(*FindSetting("ResolutionScaleY"), view, settings, state);
  }
  changed |= DrawRow(*FindSetting("Upsampling"), view, settings, state);
  // Edge-aware's base band gain, right below the choice it depends on.
  changed |= DrawRow(*FindSetting("GuidedBaseGain"), view, settings, state);
  // The rows stay at every Resolution mode: edge scaling acts on whichever grid the axes land on.
  changed |= DrawRow(*FindSetting("Foveation"), view, settings, state);
  if (settings->foveation) {
    changed |= DrawRow(*FindSetting("FoveationWidth"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationHeight"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationFalloffX"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationFalloffY"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationEdgeX"), view, settings, state);
    changed |= DrawRow(*FindSetting("FoveationEdgeY"), view, settings, state);
  }
  return changed;
}

bool DrawLookGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  static constexpr const char* options[] = {"Mine", "Built-in"};
  bool changed = false;
  DrawChoice(*state, "Compare", "Compare", options, 2, state->defaults_view ? 1 : 0, nullptr,
             "Show your own settings, or the built-in look read-only.", {},
      [state](int picked) {
        state->defaults_view = picked == 1;
        return false;
      });
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Image", "Pass 1");
  for (const char* key : {"Style", "Intensity", "LocalStructure", "LocalTone"}) {
    changed |= DrawRow(*FindSetting(key), view, settings, state);
  }
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Characters");
  for (const char* key : {"AutoMask", "SkinStructure", "KeepFaces", "FaceProtection", "LightingScale"}) {
    changed |= DrawRow(*FindSetting(key), view, settings, state);
  }
  return changed;
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

// ---------------------------------------------------------------------------------------------------------
// The panel (2026-11-23). One window with its own chrome: a title row, capsule tabs, a column of settings and a
// column that explains whatever the pointer is on.
// The tabs are the panel's jobs, not new settings -- every row is still drawn by DrawRow from the schema, so a
// row on this tab and a row on that one cannot drift apart, and no setting exists here that is not in ReShade.ini.
// ---------------------------------------------------------------------------------------------------------
namespace panel {
enum Tab : int { GENERAL, SETUP, LOOK, LAYERS, FIXES, DLSS_REFLEX, ADVANCED, COUNT };
inline constexpr std::array<const char*, COUNT> kNames{"General", "Setup", "Look", "Layers", "Fixes", "DLSS / Reflex", "Advanced"};
// What the detail column says before the pointer rests on a row.
inline constexpr std::array<const char*, COUNT> kBlurb{
    "Turns NR on, sets the two hotkeys and the language, and says what the panel is doing right now.",
    "Where the passes run and what size they work at, with the DLSS stage choices underneath.",
    "Style, intensity, local structure, tone and settings for additional passes.",
    "Result shaping, NR change stabilisation, and mask and HUD settings.",
    "Colour, exposure and motion repair.",
    "DLSS and Reflex overrides, frame generation, and their current runtime status.",
    "Routing, memory, logging, and recorded passes."};
}  // namespace panel

// One schema row group: the rows every tab body draws, in schema order, grouped so the label column is shared.
bool DrawRows(const OverlayView& view, Settings* settings, OverlayState* state, std::initializer_list<std::string_view> keys) {
  bool changed = false;
  for (const auto key : keys) changed |= DrawRow(*FindSetting(key), view, settings, state);
  return changed;
}

// A virtual-key code as the name the keyboard gave it, so both hotkeys and the footer print the same words.
std::string KeyName(uint32_t key) {
  if (key >= VK_F1 && key <= VK_F24) return std::format("F{}", key - VK_F1 + 1u);
  if (key == 0u) return {};
  std::array<wchar_t, 64> key_name = {};
  const auto scan_code = static_cast<LONG>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16u);
  const int length = GetKeyNameTextW(scan_code, key_name.data(), static_cast<int>(key_name.size()));
  if (length <= 0) return std::format("key {:#04x}", key);
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, key_name.data(), length, nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, key_name.data(), length, utf8.data(), bytes, nullptr, nullptr);
  return utf8;
}

// The key capture button and its label, shared by the two hotkeys. Returns true when the captured key changed.
bool DrawHotkeyButton(const OverlayView& view, const char* label, const char* tooltip, uint32_t* key, bool* capturing, OverlayState* state) {
  bool changed = false;
  widgets::RowLabel(label, tooltip);
  if (*capturing) {
    state->capture_seen = std::chrono::steady_clock::now();  // a capture the tab stops drawing ends on its own
    const widgets::KeyCaptureHit hit = widgets::KeyCaptureButton(label, "", /*capturing=*/true);
    if (hit.right_clicked) {
      *key = 0u;
      *capturing = false;
      changed = true;
    } else if (view.last_key_pressed != 0u) {
      if (view.last_key_pressed != VK_ESCAPE) {
        *key = view.last_key_pressed;
        changed = true;
        state->key_captured_this_frame = true;  // this frame's press must not also trigger the key it just bound
      }
      *capturing = false;
    }
    return changed;
  }
  const std::string name = KeyName(*key);
  const std::string key_label = name.empty() ? std::string("Hotkey: none") : "Hotkey: " + name;
  const widgets::KeyCaptureHit hit = widgets::KeyCaptureButton(label, key_label.c_str(), /*capturing=*/false);
  if (hit.clicked) {
    *capturing = true;
    state->capture_seen = std::chrono::steady_clock::now();
  } else if (hit.right_clicked && *key != 0u) {
    *key = 0u;
    changed = true;
  }
  widgets::ItemTooltip("Click, then press a key. Right-click clears it.");
  return changed;
}

// The title row: the name, what it is doing, and the cross. Dragging it moves the window by the mouse delta,
// which DrawPanelHeader reads back into the position the window is placed at next frame.
bool DrawPanelHeader(const OverlayView& view, OverlayState* state) {
  const float font = gui::GetFontSize();
  const float height = style::SnapUp(font * 1.9f);
  const float cross = style::SnapUp(font * 1.75f);
  const ImVec2 at = gui::GetCursorScreenPos();
  const float width = style::SnapDown(gui::GetContentRegionAvail().x);
  // The grab area is the whole band above the close button, not just the content line: the window's padding strip
  // (the very top of the panel, before the header's cursor) is part of it too, or dragging there does nothing.
  const ImVec2 drag_min = gui::GetWindowPos();
  const ImVec2 drag_max(at.x + (std::max)(1.f, width - cross - style::RowGap()), at.y + height);
  gui::SetCursorScreenPos(drag_min);
  ImGui::InvisibleButton("##panel_drag", ImVec2((std::max)(1.f, drag_max.x - drag_min.x), (std::max)(1.f, drag_max.y - drag_min.y)));
  gui::SetCursorScreenPos(at);
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    const ImVec2 delta = gui::GetIO().MouseDelta;
    // Never past the top-left edge: a negative position is also the "not placed yet" sentinel, so letting a drag
    // through there made the next frame take the default corner instead of staying at the edge.
    state->panel_x = (std::max)(0.f, state->panel_x + delta.x);
    state->panel_y = (std::max)(0.f, state->panel_y + delta.y);
  }
  // The name, then its state in the panel's quiet ink.
  gui::SetCursorScreenPos(ImVec2(at.x, at.y + (height - font * style::tokens::kHeadingScale) * 0.5f));
  widgets::Heading("GITC Uplift");
  gui::SameLine(0.f, style::BlockGap());
  const char* state_text = view.card.working ? "Live" : "Idle";
  const ImVec4 state_color = view.card.working ? style::tokens::kAccent : style::tokens::kTextTertiary;
  {
    style::StyleScope scope;
    scope.Color(ImGuiCol_Text, state_color);
    gui::SetCursorScreenPos(ImVec2(gui::GetCursorScreenPos().x, at.y + (height - font) * 0.5f));
    gui::TextUnformatted(state_text);
  }
  gui::SetCursorScreenPos(ImVec2(at.x + width - cross, at.y + (height - cross) * 0.5f));
  const bool close = widgets::CloseButton("###panel_close", cross);
  widgets::NextGap(style::BlockGap());
  return close;
}

// The columns carry NoScrollWithMouse so a wheel notch is handled once, by the column under the pointer.
void ScrollColumnWithWheel() {
  const ImGuiIO& io = gui::GetIO();
  if (io.MouseWheel == 0.f || io.WantTextInput || !gui::IsWindowHovered()) return;
  const float range = gui::GetScrollMaxY();
  if (range <= 0.f) return;
  const float step = gui::GetTextLineHeight() * 3.f;  // a notch and a half rows: RowHeight is two
  gui::SetScrollY(std::clamp(gui::GetScrollY() - io.MouseWheel * step, 0.f, range));
}

// The tab bar owns reset commands so the content can start directly with its first control or group.
std::span<const SettingSection> PanelSections(int tab) {
  static constexpr SettingSection setup[] = {SettingSection::PLACEMENT};
  static constexpr SettingSection look[] = {SettingSection::LOOK};
  static constexpr SettingSection layers[] = {SettingSection::RESULT, SettingSection::LIMITS, SettingSection::MASK};
  static constexpr SettingSection fixes[] = {SettingSection::FIXES_COLOR, SettingSection::FIXES_GUIDES};
  static constexpr SettingSection dlss[] = {SettingSection::DLSS_REFLEX};
  static constexpr SettingSection advanced[] = {SettingSection::ADVANCED};
  switch (tab) {
    case panel::SETUP: return setup;
    case panel::LOOK: return look;
    case panel::LAYERS: return layers;
    case panel::FIXES: return fixes;
    case panel::DLSS_REFLEX: return dlss;
    case panel::ADVANCED: return advanced;
    default: return {};
  }
}

bool DrawPanelTabs(const OverlayView& view, Settings* settings, OverlayState* state) {
  int tab = state->panel_tab;
  const auto sections = PanelSections(tab);
  const bool editable = !state->defaults_view || tab == panel::ADVANCED || tab == panel::DLSS_REFLEX;
  const char* label = "Reset";
  bool enabled = false;
  if (tab == panel::GENERAL) {
    label = "Restore all defaults";
    enabled = true;
  } else if (editable) {
    enabled = std::ranges::any_of(sections, [settings](SettingSection section) { return SectionChanged(*settings, section); });
  }

  bool reset = false;
  if (widgets::CapsuleTabBar("##panel_tabs", panel::kNames.data(), panel::COUNT, &tab, label, enabled, &reset)) {
    state->panel_tab = tab;
    state->panel_focus.clear();
    if (tab == panel::SETUP) state->panel_focus = "Source";
  }
  widgets::NextGap(style::BlockGap());
  if (!reset) return false;
  if (tab == panel::GENERAL) {
    state->confirm_restore = true;
    return false;
  }
  for (SettingSection section : sections) ResetSection(settings, section);
  if (tab == panel::SETUP) {
    const Settings& defaults = DefaultSettings();
    state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(SourcePickOf(defaults, view.dlss_explicit)), view.setup);
    state->setup_settle.ClickedBest(SetupRow::MOTION, static_cast<uint32_t>(MotionPickOf(defaults)), view.setup, true);
    state->setup_settle.ClickedBest(SetupRow::RESOLUTION, static_cast<uint32_t>(defaults.resolution_mode), view.setup);
  }
  return true;
}

// The status card: what the panel says about the running frames, in the same words it used before the tabs.
// Prefer recorded stages to the live estimate in both status summaries.
std::string RunningStages(const OverlayView& view) {
  std::string recorded;
  for (size_t i = 0; i < view.stage_runs.size(); ++i) {
    if (view.stage_runs[i].count == 0) continue;
    if (!recorded.empty()) recorded += " · ";
    recorded += gui::Display(NR_STAGE_NAMES[i]);
  }
  return recorded.empty() ? view.live.stage : recorded;
}

// The networks and styles of the passes that ran, as one row of the working card.
std::string NetworkStyles(const OverlayView& view) {
  std::string text;
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
      if (!text.empty()) text += " · ";
      if (group.count > 1u) text += std::format("P{} ", pass + 1u);
      text += model.style < 3u ? std::format("{} / {}", gui::Display(network), static_cast<char>('A' + model.style)) : gui::Display(network);
    }
  }
  return text;
}

void DrawStatusPanel(const OverlayView& view, OverlayState* state) {
  const StatusCard& card = view.card;
  // The plain off state says nothing the panel's own state pill does not already say.
  if (card.idle) return;
  if (card.title.empty() && card.reason.empty() && card.before_fg_note.empty() && card.fixes.empty()
      && !card.working && card.button == CardButton::NONE) return;
  widgets::Section("##status_card", [&] {
    // While running, the panel's own state pill already says so.
    if (!card.working && !card.title.empty()) {
      widgets::Heading(card.title.c_str());
      widgets::NextGap(style::RowGap());
    }
    if (card.working) {
      const bool split = std::count_if(view.stage_runs.begin(), view.stage_runs.end(), [](const auto& run) { return run.planned != 0; }) > 1;
      std::string nr_time;
      for (size_t i = 0; i < view.stage_runs.size(); ++i) {
        const auto evaluation = view.stage_runs[i].gpu_ms;
        const auto total = view.stage_runs[i].gpu_total_ms;
        if (!evaluation && !total) continue;
        if (!nr_time.empty()) nr_time += " · ";
        if (split) nr_time += gui::Display(NR_STAGE_NAMES[i]) + " ";
        if (evaluation && total) {
          nr_time += std::format("{:.2f} · {:.2f} ms", *evaluation, *total);
        } else {
          nr_time += std::format("{:.2f} ms", total ? *total : *evaluation);
        }
      }
      const float timing_width = nr_time.empty() ? 0.f : gui::CalcTextSize("NR GPU time (inference · chain)").x;
      const float key_width = (std::max)({timing_width, gui::CalcTextSize("Running at").x, gui::CalcTextSize("Motion vectors").x,
                                          gui::CalcTextSize("Backend").x, gui::CalcTextSize("Resolution").x,
                                          gui::CalcTextSize("Frame generation").x, gui::CalcTextSize("Network / style").x});
      const float value_offset = style::SnapUp(key_width) + style::BlockGap();
      const float value_x = gui::GetCursorPosX() + value_offset;
      const bool inline_values = value_offset <= gui::GetContentRegionAvail().x * 0.40f;
      const auto row = [inline_values, value_x](const char* key, const std::string& value) {
        widgets::SecondaryWrapped(key);
        if (inline_values) gui::SameLine(value_x);
        gui::TextWrapped("%s", value.c_str());
      };
      row("Running at", RunningStages(view));
      row("Motion vectors", view.live.motion);
      if (!view.details_backend.empty()) row("Backend", view.details_backend);
      row("Resolution", view.live.resolution);
      if (!view.live.frame_generation.empty()) row("Frame generation", view.live.frame_generation);
      const std::string styles = NetworkStyles(view);
      if (!styles.empty()) row("Network / style", styles);
      if (!nr_time.empty()) {
        row("NR GPU time (inference · chain)", nr_time);
        if (gui::IsItemHovered()) {
          widgets::ItemTooltip("%s", gui::Display("Per stage, the latest completed recording: NR's evaluation alone, then the whole chain (copy and remap, encode and exposure meter, evaluation, compose/Look, decode). Includes all passes and the extra Keep faces run when active.").c_str());
        }
      }
      if (!view.setup.why.empty()) {
        const std::string why = "Why: " + view.setup.why;
        widgets::SecondaryWrapped(why.c_str());
        widgets::ItemTooltip("%s", why.c_str());
      } else {
        if (!view.live.note.empty()) widgets::SecondaryText(view.live.note.c_str());
        if (!card.before_fg_note.empty()) gui::TextDisabled("%s", card.before_fg_note.c_str());
      }
      if (!view.live.footer.empty()) widgets::SecondaryText(view.live.footer.c_str());
    } else {
      if (!card.reason.empty()) gui::TextWrapped("%s", card.reason.c_str());
      if (!card.before_fg_note.empty()) gui::TextDisabled("%s", card.before_fg_note.c_str());
      for (const std::string& fix : card.fixes) gui::BulletText("%s", fix.c_str());
    }
    switch (card.button) {
      case CardButton::NONE: break;
      case CardButton::RETRY_NOW:
        widgets::NextGap(style::BlockGap());
        if (widgets::PrimaryButton("Retry now")) state->retry_now = true;
        break;
      case CardButton::CLEAR_LATCH:
        break;
    }
  });
}

void DrawSafetyPanel(const OverlayView& view, Settings* settings, OverlayState* state, bool* changed) {
  if (view.safety_locks.empty()) return;
  widgets::Section("##safety_locks", [&] {
    widgets::Heading("SAFETY LATCH ACTIVE", style::tokens::kBad);
    for (const auto& lock : view.safety_locks) {
      gui::PushID(lock.feature.data());
      widgets::NextGap(style::BlockGap());
      widgets::GroupHeading(lock.feature.data());
      gui::TextWrapped("%s", lock.reason.c_str());
      if (lock.setting != nullptr) {
        if (settings->*(lock.setting) == lock.restore_value) {
          gui::TextWrapped("Latch cleared for next launch. Restart the game to retry.");
        } else if (widgets::QuietAction("Clear latch for next launch")) {
          settings->*(lock.setting) = lock.restore_value;
          *changed = true;
        }
      } else if (lock.retry_now) {
        if (widgets::QuietAction("Retry now")) state->retry_now = true;
      } else {
        gui::TextWrapped("Restart the game to retry this feature.");
      }
      gui::PopID();
    }
  });
}

bool DrawGeneralTab(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  gui::PushID("##general_body");
  changed |= DrawRow(*FindSetting(ENABLED_KEY), view, settings, state);
  widgets::NextGap(style::BlockGap());
  changed |= DrawHotkeyButton(view, "NR hotkey", "Switches NR on and off without opening this panel.", &settings->enable_key, &state->capturing_key, state);
  widgets::NextGap(style::BlockGap());
  changed |= DrawHotkeyButton(view, "Panel hotkey", "Opens and closes this panel. ReShade's own panel keeps its own key.", &settings->panel_key, &state->capturing_panel_key, state);
  if (settings->panel_key == 0x24u) {
    // ReShade consumes its own overlay key before an add-on sees the press, so Home can never reach this panel.
    style::StyleScope scope;
    scope.Color(ImGuiCol_Text, style::tokens::kBad);
    gui::TextWrapped("Home is ReShade's own overlay key, so it cannot open this panel. Pick another key here.");
  }
  widgets::NextGap(style::BlockGap());
  static constexpr const char* languages[] = {"English", "简体中文 / Chinese"};
  changed |= DrawChoice(*state, "Language", "Language", languages, 2, static_cast<int>(settings->language),
                        nullptr, FindSetting("Language")->tooltip, {},
      [settings, state](int picked) {
        FindSetting("Language")->set(*settings, static_cast<double>(picked));
        state->language_changed = true;
        return true;
      });
  gui::PopID();
  DrawStatusPanel(view, state);
  DrawSafetyPanel(view, settings, state, &changed);
  if (widgets::ConfirmDialog("Restore all defaults###restore_defaults",
                             "Every setting returns to its default, the hotkey included; Enable stays as it is.",
                             "Keep my settings", "Restore all", state->confirm_restore, 88.f)) {
    RestoreAllDefaults(settings);
    changed = true;
  }
  return changed;
}

bool DrawSetupTab(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  changed |= DrawStageRow(view, settings, state);
  changed |= DrawSetupGroups(view, settings, state);
  return changed;
}

bool DrawLayersTab(const OverlayView& view, Settings* settings, OverlayState* state) {
  widgets::GroupHeading("Result");
  bool changed = false;
  changed |= DrawRows(view, settings, state, {"ShapeResult"});
  const bool shaping_enabled = state->defaults_view ? DefaultSettings().look.enabled : settings->look.enabled;
  if (shaping_enabled) {
    widgets::NextGap(style::BlockGap());
    widgets::GroupHeading("Strength and colour");
    changed |= DrawRows(view, settings, state, {"EditStrength", "Brighten", "Darken", "ResultColor", "HueShift"});
    widgets::NextGap(style::BlockGap());
    widgets::GroupHeading("Tonal range");
    changed |= DrawRows(view, settings, state, {"Shadows", "Midtones", "Highlights"});
    widgets::NextGap(style::BlockGap());
    widgets::GroupHeading("Limits");
    changed |= DrawRows(view, settings, state, {"MaxBrighten", "MaxDarken", "MaxColorChange"});
    widgets::NextGap(style::BlockGap());
    widgets::GroupHeading("Detail and stability");
    changed |= DrawRows(view, settings, state, {"LargeScaleTone", "FineDetail", "DetailRadius", "HaloSuppression", "Stabilize", "StabilizeMs", "StabilizeDetail"});
  }
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("NR change");
  changed |= DrawRows(view, settings, state, {"StabilizeChange", "StabilizeChangeMs"});
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Mask and HUD");
  changed |= DrawRows(view, settings, state, {"Mask", "UICorrection"});
  return changed;
}

bool DrawLookTab(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = DrawLookGroups(view, settings, state);
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Passes");
  changed |= DrawRows(view, settings, state, {"PassCount"});
  if (state->defaults_view) return changed;
  if (settings->pass_count < 2u) {
    state->edited_pass = 2u;
    widgets::SecondaryWrapped("Increase Pass count to configure additional passes.");
    return changed;
  }

  const uint32_t count = std::clamp(settings->pass_count, 2u, MAX_PASSES);
  const uint32_t previous = std::clamp(state->edited_pass, 2u, count);
  state->edited_pass = previous;
  std::array<std::string, MAX_PASSES - 1u> pass_labels;
  std::array<const char*, MAX_PASSES - 1u> names{};
  for (uint32_t pass = 2u; pass <= count; ++pass) {
    pass_labels[pass - 2u] = std::format("Pass {}", pass);
    names[pass - 2u] = pass_labels[pass - 2u].c_str();
  }
  DrawChoice(*state, "EditedPass", "Edit pass", names.data(), static_cast<int>(count - 1u), static_cast<int>(previous - 2u),
             nullptr, "Select which additional pass to edit. Pass 1 uses Setup and Look.", {},
      [state](int picked) {
        state->edited_pass = static_cast<uint32_t>(picked) + 2u;
        return false;
      });
  const uint32_t pass = state->edited_pass;
  const auto pass_key = [pass](const char* suffix) { return std::format("Pass{}{}", pass, suffix); };
  const PassStageRow& stage_row = view.setup.pass_stages[pass - 2u];
  const char* stage = view.RecordedStageName(pass - 1u);
  if (!stage) stage = stage_row.runs == SourcePick::PRESENT ? view.SharedPresentName(*settings) : StageLabel(stage_row.runs);
  gui::PushID(static_cast<int>(pass));
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Placement and network", stage);
  changed |= DrawPassStageRow(*FindSetting(pass_key("Stage")), view, settings, state);
  changed |= DrawRow(*FindSetting(pass_key("Preset")), view, settings, state);
  if (stage_row.starts_group && stage_row.runs != SourcePick::BEFORE_UPSCALING) {
    widgets::SecondaryWrapped("Resolution and face protection are shared within this stage.");
    if (stage_row.runs == SourcePick::AFTER_DLSS) {
      changed |= DrawRows(view, settings, state, {"ResolutionAfterDlss", "ResolutionScaleAfterDlss", "KeepFacesAfterDlss"});
    } else {
      changed |= DrawRows(view, settings, state, {"ResolutionPresent", "ResolutionScalePresent", "KeepFacesPresent"});
    }
  }
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Pass look");
  for (const SettingDescriptor& descriptor : SettingsSchema()) {
    if (descriptor.pass == pass && (descriptor.flags & (setting_flags::PASS_STAGE | setting_flags::NETWORK_PRESET)) == 0u) {
      changed |= DrawRow(descriptor, view, settings, state);
    }
  }
  const auto pass_settings = SettingsSchema();
  if (std::ranges::any_of(pass_settings, [settings, pass](const SettingDescriptor& descriptor) {
        return descriptor.pass == pass && !IsDefault(*settings, descriptor);
      })) {
    style::StyleScope quiet;
    quiet.FlatButton();
    if (gui::Button("Reset pass")) {
      for (const SettingDescriptor& descriptor : pass_settings) {
        if (descriptor.pass == pass) ResetSetting(settings, descriptor);
      }
      changed = true;
    }
  }
  gui::PopID();
  return changed;
}

bool DrawFixesTab(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  widgets::GroupHeading("Colour");
  changed |= DrawRows(view, settings, state, {"Encoding"});
  const bool white_in_exposure = DiffuseWhiteInExposureGroup(*settings);
  if (!white_in_exposure) changed |= DrawRows(view, settings, state, {"DiffuseWhiteNits"});
  changed |= DrawRows(view, settings, state, {"TransferStrength", "ColorStrength"});
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Exposure");
  changed |= DrawRows(view, settings, state, {"InputExposure"});
  if (white_in_exposure) changed |= DrawRows(view, settings, state, {"DiffuseWhiteNits"});
  changed |= DrawRows(view, settings, state, {"AutoExposureMode", "AutoExposureBlend", "ExposureAdapt", "AdaptBrighterStops", "AdaptDarkerStops"});
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Colour conversion");
  changed |= DrawRows(view, settings, state, {"LinearUnitNits", "SourcePrimaries", "NeuralTransfer", "ChromaClamp", "NearBlackGuard"});
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Motion and game state");
  changed |= DrawRows(view, settings, state, {"MotionScaleX", "MotionScaleY", "ChainedHistory", "StateRestore"});
  return changed;
}

bool DrawAdvancedTab(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  widgets::GroupHeading("Routing");
  changed |= DrawRows(view, settings, state, {"PresentWithFrameGen", "BeforeFGMotion", "VulkanNr", "AllowCrossQueuePasses", "UseD3D9Ex"});
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Memory and recovery");
  changed |= DrawRows(view, settings, state, {"VramCheck", "BudgetMarginMB", "GraceSeconds", "AutoResume", "AutoRetry"});
  widgets::NextGap(style::BlockGap());
  widgets::GroupHeading("Runtime and logging");
  changed |= DrawRows(view, settings, state, {"SnippetPath", "NgxHooks", "ForeignNr", "LogLevel", "ShowNvIndicator"});
  if (settings->keep_faces) {
    widgets::NextGap(style::BlockGap());
    widgets::GroupHeading("Face mask tuning");
    changed |= DrawRows(view, settings, state, {"ShowFaceMask", "FaceMaskThreshold", "FaceMaskFull", "FaceMaskStrength", "FaceMaskSoftness", "FaceEdgeFalloff",
                                               "FaceLightingDensity", "FaceSpeckSize", "FaceFillSkin", "FaceFillRadius", "FaceFillTolerance"});
  }
  return changed;
}

// A choice row's metadata and setter stay alive until the detail pane consumes them in this draw.
bool DrawDetailColumn(const OverlayView& view, const Settings& settings, const OverlayState& state) {
  const int tab = std::clamp(state.panel_tab, 0, static_cast<int>(panel::COUNT) - 1);
  const auto& focus = PendingDetail().focus;
  bool changed = false;
  if (focus.title.empty()) {
    widgets::DetailText(panel::kBlurb[tab]);
  } else {
    widgets::Heading(focus.title.c_str());
    if (!focus.options.empty()) {
      widgets::NextGap(style::BlockGap());
      gui::PushID(focus.key.c_str());
      for (size_t index = 0; index < focus.options.size(); ++index) {
        const auto& option = focus.options[index];
        const int value = static_cast<int>(index);
        if (widgets::DetailOption(option.label.c_str(), option.why, value, value == focus.option_value) && focus.pick) {
          changed |= focus.pick(value);
        }
      }
      gui::PopID();
    }
    if (focus.key == "ResolutionMode" || focus.key == "ResolutionScale" || focus.key == "ResolutionScaleY" ||
        focus.key == "ResolutionAsymmetric") {
      DrawWorkingSize(view, settings);
    }
    if (!focus.body.empty()) {
      widgets::NextGap(style::BlockGap());
      widgets::DetailText(focus.body);
    }
    if (!focus.why.empty()) {
      widgets::NextGap(style::RowGap());
      widgets::DetailText(focus.why, style::tokens::kBad);
    }
  }
  return changed;
}

bool DrawPanel(const OverlayView& view, Settings* settings, OverlayState* state) {
  state->drag.pass_slider = false;  // the rows OR into it
  state->language_changed = false;
  const gui::LanguageScope language_scope(settings->language);
  bool changed = false;
  if (DrawPanelHeader(view, state)) state->panel_open = false;
  // The focused modal handles its own Escape; do not close the panel behind it.
  constexpr auto panel_focus_flags = ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy;
  if (gui::IsWindowFocused(panel_focus_flags) && gui::IsKeyPressed(ImGuiKey_Escape) && !state->capturing_key
      && !state->capturing_panel_key) {
    state->panel_open = false;
  }
  auto& detail = PendingDetail();
  detail = {};
  changed |= DrawPanelTabs(view, settings, state);
  const double now = ImGui::GetTime();
  const float tab_progress = state->panel_tab_transition_started < 0.0
                                 ? 1.f
                                 : std::clamp(static_cast<float>((now - state->panel_tab_transition_started) / style::motion::kTabSlide), 0.f, 1.f);
  const float tab_eased = 1.f - (1.f - tab_progress) * (1.f - tab_progress);
  {
    style::StyleScope transition;
    transition.Var(ImGuiStyleVar_Alpha, tab_eased);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style::Units(1.5f) * (1.f - tab_eased));
    const float total = style::SnapDown(gui::GetContentRegionAvail().x);
    const float gutter = style::SnapDown((std::min)(style::Units(5.f), total * 0.04f));
    const float right_width = style::SnapDown((total - gutter) * 0.41f);
    const float left_width = (std::max)(1.f, total - right_width - gutter);
    const float body_height = (std::max)(1.f, style::SnapDown(gui::GetContentRegionAvail().y));

    // The wheel is handled by hand inside each column (ScrollColumnWithWheel); NoScrollWithMouse keeps ImGui from
    // scrolling them a second time under the pointer.
    {
      style::StyleScope column;
      column.Var(ImGuiStyleVar_WindowPadding, {style::CardInset(), style::CardInset()})
          .Color(ImGuiCol_ScrollbarBg, style::tokens::kTransparent);
      constexpr auto flags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened;
      gui::PushID(state->panel_tab);  // each tab keeps its own scroll offset and navigation history
      if (gui::BeginChild("##panel_left", ImVec2(left_width, body_height), flags,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse)) {
        ScrollColumnWithWheel();
        switch (state->panel_tab) {
          case panel::GENERAL: changed |= DrawGeneralTab(view, settings, state); break;
          case panel::SETUP: changed |= DrawSetupTab(view, settings, state); break;
          case panel::LOOK: changed |= DrawLookTab(view, settings, state); break;
          case panel::LAYERS: changed |= DrawLayersTab(view, settings, state); break;
          case panel::FIXES: changed |= DrawFixesTab(view, settings, state); break;
          case panel::DLSS_REFLEX: changed |= DrawDlssReflexTab(view, settings, state); break;
          case panel::ADVANCED: changed |= DrawAdvancedTab(view, settings, state); break;
          default: break;
        }
      }
      gui::EndChild();
      gui::PopID();
    }
    // Resolve once after all rows have offered their detail. A stale nav cursor and a mouse exit are not new hits.
    state->panel_focus = detail.focus.key;
    state->panel_hovered_row = detail.hovered_row;
    state->panel_nav_choice = detail.nav_choice;
    gui::SameLine(0.f, gutter);
    {
      // The detail pane shares the panel background. Its child window only provides clipping and scrolling.
      style::StyleScope column;
      column.Var(ImGuiStyleVar_WindowPadding, {0.f, style::CardInset()});
      constexpr auto flags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened;
      if (gui::BeginChild("##panel_right", ImVec2(right_width, body_height), flags,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse)) {
        ScrollColumnWithWheel();
        changed |= DrawDetailColumn(view, *settings, *state);
      }
      gui::EndChild();
    }
    detail = {};
    changed |= DrawEnableWarning(settings, state, view.dlss_explicit);
  }
  return changed;
}

bool DrawOverlayWindow(const OverlayView& view, Settings* settings, OverlayState* state) {
  // ReShade draws its own windows from the same global style, so the panel's look is lent for exactly this frame.
  const style::PanelStyle panel_style;
  // ReShade's in-game backend may hide the platform cursor while drawing its overlay. Draw ImGui's arrow in the
  // same command list as the panel so the standalone window remains usable inside a game.
  gui::GetIO().MouseDrawCursor = true;
  // ImGui::GetMainViewport() and IO.DisplayPos are not available in this build (no viewport support), so the panel is
  // clamped to the same rectangle the dialogs are centred in: the IO display, whose origin is (0, 0).
  const ImVec2 origin(0.f, 0.f);
  const ImVec2 room = gui::GetIO().DisplaySize;
  const double now = ImGui::GetTime();
  if (state->panel_intro_started < 0.0) state->panel_intro_started = now;
  if (state->panel_animation_tab < 0) {
    state->panel_animation_tab = state->panel_tab;  // the tab it opens on is already in place, under the intro
  } else if (state->panel_animation_tab != state->panel_tab) {
    state->panel_animation_tab = state->panel_tab;
    state->panel_tab_transition_started = now;
  }
  const float intro_progress = std::clamp(static_cast<float>((now - state->panel_intro_started) / style::motion::kIntro), 0.f, 1.f);
  const float intro_eased = 1.f - (1.f - intro_progress) * (1.f - intro_progress);
  // Wide enough for both columns at ReShade's default font, and never wider than the screen minus a margin.
  const float width = std::clamp(room.x - style::Units(4.f), 1.f, style::Units(200.f));
  const float height = std::clamp(room.y - style::Units(4.f), 1.f, style::Units(120.f));
  if (state->panel_x < 0.f || state->panel_y < 0.f) {
    state->panel_x = origin.x + style::Units(6.f);
    state->panel_y = origin.y + style::Units(10.f);
  }
  state->panel_x = std::clamp(state->panel_x, origin.x, (std::max)(origin.x, origin.x + room.x - width));
  state->panel_y = std::clamp(state->panel_y, origin.y, (std::max)(origin.y, origin.y + room.y - height));
  gui::SetNextWindowPos(ImVec2(state->panel_x, state->panel_y + style::Units(2.f) * (1.f - intro_eased)), ImGuiCond_Always);
  gui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  bool changed = false;
  {
    style::StyleScope window;
    window.Color(ImGuiCol_Border, style::tokens::Fade(style::tokens::kTextPrimary, 0.07f))
        .Var(ImGuiStyleVar_WindowBorderSize, (std::max)(1.f, style::Px(1.f)))
        .Var(ImGuiStyleVar_WindowPadding, ImVec2(style::Units(5.f), style::Units(4.f)))
        .Var(ImGuiStyleVar_Alpha, intro_eased);
    const bool visible = gui::Begin("GITC Uplift###UpliftPanel", nullptr,
                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
                                        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (visible) {
      state->panel_drew = true;  // the present handler blocks the game's input only while this window really draws
      changed = DrawPanel(view, settings, state);
    }
    gui::End();
  }
  if (!state->panel_open) {
    gui::GetIO().MouseDrawCursor = false;
    // The next open starts over: the intro replays and its first tab arrives without a tab transition.
    state->panel_intro_started = -1.0;
    state->panel_tab_transition_started = -1.0;
    state->panel_animation_tab = -1;
  }
  return changed;
}

}  // namespace uplift::ui
