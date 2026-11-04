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
#include "sources/work_size.hpp"
#include "ui/overlay_widgets.hpp"

namespace uplift::ui {
namespace {

// A panel taller than this is scrolled rather than run off the bottom of the screen.
constexpr float kMenuMaxScreenFraction = 0.8f;

// Frame skip's carry does not follow the foveation warp, so the two never run together and the foveated rows
// are hidden rather than shown and quietly ignored. The Vulkan route records through its own pipeline and never
// skips, so its rows stay.
bool FoveationHidden(const OverlayView& view, const Settings& settings) {
  return !view.dlss_explicit && settings.half_rate != HalfRate::OFF;
}

// Plan 14 (R90): the tooltip line of a highlighted option while the highlight is not the stored preference (what runs differs from what ReShade.ini holds).
std::string SavedNote(std::string_view preference) {
  return std::format("Saved: {}; it runs again by itself when it can.", preference);
}

bool DrawReset(const SettingDescriptor& descriptor, Settings& settings, OverlayState& state, bool align_right, const SetupView& setup) {
  if (descriptor.section == SettingSection::TOP || IsDefault(settings, descriptor)) return false;
  gui::SameLine();
  if (align_right) {
    if (gui::GetContentRegionAvail().x + 1.f < gui::ResetButtonWidth()) gui::NewLine();
    const float remaining = gui::GetContentRegionAvail().x - gui::ResetButtonWidth();
    if (remaining > 0.f) gui::SetCursorPosX(gui::GetCursorPosX() + remaining);
  }
  const bool reset = gui::ResetButton();
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    const std::string value = descriptor.key == "MotionVectors" ? "DLSS" : FormatSettingValue(descriptor, DefaultSettings());
    gui::SetTooltip("Reset to default (%s)", value.c_str());
  }
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
  if (descriptor.key == "ResolutionScalePresent") label = std::format("Custom scale at {} (%)", view.SharedPresentName(*settings));
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
  const bool guided_gains_hidden = ((descriptor.key == "GuidedBaseGain" || descriptor.key == "GuidedDetailGain") && edge_aware_off);
  // The ceiling and the band it lands on say nothing a gain cannot while the work grid is uniform: its density is
  // one number screen-wide. Only a foveated grid, thinning toward the edges, gives the two rows an effect of their
  // own (LookConfigFrom clears them when Foveated edges is off).
  const bool density_rows_hidden = ((descriptor.key == "DensityOnBase" || descriptor.key == "EdgeDensityAttenuation")
                                    && (!settings->foveation || edge_aware_off || FoveationHidden(view, *settings)));
  const bool foveation_hidden = (descriptor.key.starts_with("Foveation") && FoveationHidden(view, *settings));
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
      || density_rows_hidden || foveation_hidden || keep_faces_off || fill_off) {
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
  gui::BeginDisabled(!disabled_reason.empty() || read_only);
  bool changed = false;
  if (descriptor.key == "SkinStructure") {
    bool follows = (settings->skin_structure < 0.f);
    if (gui::Checkbox("Skin follows Structure", &follows)) {
      settings->skin_structure = (follows ? SKIN_SAME_AS_STRUCTURE : settings->local_structure);
      changed = true;
    }
  }
  if (descriptor.key == "DiffuseWhiteNits") {
    bool automatic = (settings->diffuse_white_nits <= 0.f);
    if (gui::Checkbox("Automatic diffuse white", &automatic)) {
      settings->diffuse_white_nits = (automatic ? 0.f : std::clamp(view.effective_diffuse_white_nits, 48.f, 500.f));
      changed = true;
    }
  }
  const bool captioned = grouped && descriptor.kind != SettingKind::BOOL;
  if (captioned) {
    const char* caption = descriptor.key == "ResolutionMode" ? "Working resolution" : descriptor.key == "ResolutionScale" ? "Custom scale" : label.c_str();
    std::string value;
    const bool inherited_skin = descriptor.key == "SkinStructure" && settings->skin_structure < 0.f;
    if (descriptor.key == "ResolutionScale") value = std::format("{:.0f}%", settings->resolution_scale);
    else if (descriptor.key == "DiffuseWhiteNits") {
      const float nits = settings->diffuse_white_nits <= 0.f ? view.effective_diffuse_white_nits : settings->diffuse_white_nits;
      value = nits > 0.f ? std::format("{:.0f}", nits) : gui::Display("Unknown");
    }
    else if (descriptor.key == "AutoExposureBlend") value = std::format("{:.0f}%", settings->auto_exposure_blend * 100.f);
    else if (inherited_skin) value = std::format("{:.2f}", settings->local_structure);
    else if (descriptor.kind == SettingKind::FLOAT) value = FormatSettingValue(descriptor, *settings);
    else if (descriptor.kind == SettingKind::UINT && descriptor.key != "MaskBlendMode" && descriptor.max <= 100.0)
      value = state->pending_key == descriptor.key && state->pending_release ? std::to_string(*state->pending_release) : FormatSettingValue(descriptor, *settings);
    gui::BeginDisabled(inherited_skin);
    widgets::SettingCaption(caption, value, !IsDefault(*settings, descriptor));
    gui::EndDisabled();
    changed |= DrawReset(descriptor, *settings, *state, true, view.setup);
    widgets::NextGap(style::LabelGap());
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
    const bool edited = grouped ? widgets::Slider(label.c_str(), &skin, static_cast<float>(descriptor.min),
                                                  static_cast<float>(SliderMax(descriptor)), "%.2f", nullptr, &state->drag.skin_structure)
                                : gui::SliderFloat(label.c_str(), &skin, 0.f, 1.f);
    if (edited) {
      settings->skin_structure = skin;
      changed = true;
    }
    state->drag.skin_structure = gui::IsItemActive();
    gui::EndDisabled();
  } else if (descriptor.key == "DiffuseWhiteNits") {
    // ui-review.md §3 rule 2 (the Skin pattern): an Automatic checkbox, the control greyed at the value
    // in use -- shown, not hidden, so the effective value always reads even while it is automatic.
    const bool automatic = settings->diffuse_white_nits <= 0.f;
    gui::BeginDisabled(automatic);
    float nits = (automatic ? view.effective_diffuse_white_nits : settings->diffuse_white_nits);
    if (widgets::Slider(label.c_str(), &nits, 48.f, 500.f, "%.0f", nullptr, nullptr) && !automatic) {
      settings->diffuse_white_nits = nits;
      changed = true;
    }
    gui::EndDisabled();
  } else {
    switch (descriptor.kind) {
      case SettingKind::BOOL: {
        bool value = (descriptor.get(*settings) != 0.0);
        if (widgets::Switch(label.c_str(), &value, /*at_row_end=*/false)) {
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
        bool active = false;  // the slider's own held state: APPLY_ON_RELEASE waits for the release, not for every step
        const bool edited = descriptor.max > 100.0 ? gui::InputInt(label.c_str(), &value, 64, 512)
            : grouped ? widgets::SliderInt(label.c_str(), &value, low, high, nullptr, &active)
                      : gui::SliderInt(label.c_str(), &value, low, high);
        if (descriptor.max > 100.0) active = gui::IsItemActive();
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
          if (gui::IsItemDeactivatedAfterEdit() && state->pending_key == descriptor.key && state->pending_release) {
            const int released = *std::exchange(state->pending_release, std::nullopt);
            if (released != applied) {
              descriptor.set(*settings, std::clamp(static_cast<double>(released), descriptor.min, descriptor.max));
              changed = true;
            }
          } else if (gui::IsItemDeactivated() && state->pending_key == descriptor.key) {
            state->pending_release.reset();  // released without an edit
          }
        }
        if (descriptor.key == "PassCount" && !view.frame_generation_warning.empty()) {
          gui::PushTextWrapPos(0.f);
          gui::TextDisabled("%s", view.frame_generation_warning.c_str());
          gui::PopTextWrapPos();
        }
        break;
      }
      case SettingKind::FLOAT: {
        // 2026-10-09: AutoExposureBlend is stored 0..1 and shown as 0-100 %.
        const float shown_scale = (descriptor.key == "AutoExposureBlend" ? 100.f : 1.f);
        float value = static_cast<float>(descriptor.get(*settings)) * shown_scale;
        const float high = static_cast<float>(SliderMax(descriptor)) * shown_scale;  // Ctrl+click may type past it (DescriptorMax)
        const bool resolution_scale = (descriptor.key == "ResolutionScale" || descriptor.key == "ResolutionScaleAfterDlss"
                                       || descriptor.key == "ResolutionScalePresent");
        const char* const format = (descriptor.key == "GraceSeconds" ? "%.1f"
                                    : resolution_scale                      ? "%.0f %%"
                                    : descriptor.key == "AutoExposureBlend" ? "%.0f %%"
                                    : descriptor.key == "StabilizeMs"       ? "%.0f ms"
                                                                            : "%.2f");
        bool active = false;  // the slider's own held state, for the rows that gate work on the release
        const bool edited = grouped
            ? widgets::Slider(label.c_str(), &value, static_cast<float>(descriptor.min) * shown_scale, high, format, nullptr, &active, shown_scale)
            : gui::SliderFloat(label.c_str(), &value, static_cast<float>(descriptor.min) * shown_scale, high, format);
        if (!grouped) active = gui::IsItemActive();
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
          // Show FG placements in pipeline order while retaining the stored values used by existing presets.
          static constexpr size_t fg_order[]{static_cast<size_t>(Settings::FgPresent::OFF), static_cast<size_t>(Settings::FgPresent::BEFORE), static_cast<size_t>(Settings::FgPresent::AFTER)};
          const std::span<const size_t> order = descriptor.key == "PresentWithFrameGen" ? std::span<const size_t>{fg_order} : std::span<const size_t>{};
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
          if (gui::BeginCombo(label.c_str(), labels[shown])) {
            for (size_t index = 0u; index < labels.size(); ++index) {
              const OptionState& option = view.setup.resolution_options[index];
              // Fix round 1 (M5): Setup's row greys every entry that cannot run, its highlight included (as before the split chain); a later stage's row
              // keeps its stored mode selectable, since its preview is that mode and not what runs.
              gui::BeginDisabled(!option.why.empty() && (setup_row || index != shown));
              if (gui::Selectable(labels[index], shown == index)) {
                descriptor.set(*settings, static_cast<double>(index));
                if (setup_row) {
                  state->setup_settle.Clicked(SetupRow::RESOLUTION, static_cast<uint32_t>(index));
                }
                changed = true;
              }
              gui::EndDisabled();
              if (!option.why.empty() && gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                gui::SetTooltip("Unavailable here: %s", std::string(option.why).c_str());
              }
            }
            gui::EndCombo();
          }
        } else {
          std::array<const char*, 8> labels = {};
          const size_t count = std::min(descriptor.choices.size(), labels.size());
          for (size_t index = 0u; index < count; ++index) {
            labels[index] = descriptor.choices[index].data();  // literals: NUL-terminated
          }
          if (gui::Combo(label.c_str(), &value, labels.data(), static_cast<int>(count))) {
            descriptor.set(*settings, static_cast<double>(value));
            changed = true;
          }
        }
        break;
      }
      case SettingKind::TEXT: {
        std::array<char, 1024> text = {};
        descriptor.get_text(*settings).copy(text.data(), text.size() - 1u);
        if (gui::InputText(label.c_str(), text.data(), text.size())) {
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
  if (descriptor.key == "Mask" && !view.mask_note.empty()) {
    gui::TextDisabled("%s", view.mask_note.c_str());
  }
  if (descriptor.key == "UICorrection" && !view.ui_correction_note.empty()) {
    gui::TextDisabled("%s", view.ui_correction_note.c_str());
  }
  if (descriptor.key == "UseD3D9Ex") {
    const std::string& note = (view.d3d9ex_unavailable.empty() ? view.d3d9ex_readout : view.d3d9ex_unavailable);
    if (!note.empty()) {
      gui::TextDisabled("%s", note.c_str());
    }
  }
  if (descriptor.key == "VulkanNr" && view.vulkan_nr_unavailable.empty() && !view.vulkan_nr_note.empty()) {
    gui::TextDisabled("%s", view.vulkan_nr_note.c_str());
  }
  if (grouped && descriptor.key == "ResolutionScale") widgets::PercentTicks(descriptor.min, descriptor.max);
  if (!captioned) changed |= DrawReset(descriptor, *settings, *state, grouped, view.setup);
  gui::EndDisabled();
  gui::PopID();
  return changed && !read_only;
}

bool DrawEnableWarning(const SettingDescriptor& descriptor, Settings* settings, OverlayState* state, bool explicit_dlss) {
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
  const bool cancel = gui::Button(descriptor.key == "PresentWithFrameGen" ? "Keep current mode" : "Keep disabled");
  gui::SetItemDefaultFocus();
  gui::SameLine();
  const bool enable = gui::Button("Enable anyway");
  if (cancel || enable || gui::IsKeyPressed(ImGuiKey_Escape)) {
    gui::CloseCurrentPopup();
  }
  if (enable) {
    descriptor.set(*settings, 1.0);
    if (descriptor.key == "PresentWithFrameGen" && state->confirm_present_stage) {
      SetSourcePick(settings, SourcePick::PRESENT, explicit_dlss);
      state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(SourcePick::PRESENT));
    }
  }
  gui::EndPopup();
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
  gui::AlignTextToFramePadding();
  gui::TextUnformatted(label.c_str());
  changed |= DrawReset(descriptor, *settings, *state, true, view.setup);
  widgets::NextGap(style::LabelGap());
  if (gui::BeginCombo("##stage", names[shown])) {  // literals and static strings: NUL-terminated
    for (size_t index = 0u; index < row.options.size(); ++index) {
      const PassStageOption& option = row.options[index];
      gui::BeginDisabled(!option.why.empty());
      if (gui::Selectable(names[index], shown == index) && index != stored) {
        descriptor.set(*settings, static_cast<double>(index));
        changed = true;
      }
      gui::EndDisabled();
      if (!option.why.empty() && gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        gui::SetTooltip("Unavailable here: %s", option.why.c_str());
      }
    }
    gui::EndCombo();
  }
  if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    std::string tooltip(descriptor.tooltip);
    if (!view.setup.split_capable) {
      tooltip = std::format("Greyed: {}\n{}", ONE_STAGE_REASON, descriptor.tooltip);
    } else if (shown != stored && (row.options[stored].why.empty() || row.options[stored].temporary)) {  // a fixed cause never clears: no promise
      tooltip += "\n" + SavedNote(names[stored]);
    }
    gui::SetTooltip("%s", tooltip.c_str());
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
  gui::AlignTextToFramePadding();
  gui::TextUnformatted("Motion vectors");
  bool changed = DrawReset(*FindSetting("MotionVectors"), *settings, *state, true, view.setup);
  widgets::NextGap(style::LabelGap());
  if (const int clicked = widgets::Segmented("##MotionVectors", OPTIONS.data(), 4, shown, why.data(), tips.data()); clicked >= 0) {
    SetMotionPick(settings, static_cast<MotionPick>(clicked));
    state->setup_settle.Clicked(SetupRow::MOTION, static_cast<uint32_t>(clicked));
    changed = true;
  }
  gui::PopID();
  return changed;
}

void DrawWorkingSize(const OverlayView& view, const Settings& settings) {
  nr::Size input = view.first_stage_input;
  if (input.Empty() && view.setup.stage == SourcePick::PRESENT) input = view.output_size;
  nr::Size size = view.first_stage_canvas;
  if (settings.resolution_mode == ResolutionMode::CUSTOM && !input.Empty()) {
    const double scale = settings.resolution_scale / 100.0;
    const bool asymmetric = settings.resolution_asymmetric;
    const double scale_y = (asymmetric ? settings.resolution_scale_y / 100.0 : scale);
    size = sources::WorkSize(input.width * scale, input.height * scale_y, scale, scale_y, input);
    if (view.setup.stage == SourcePick::BEFORE_UPSCALING) size = sources::CanvasSize(size);
  } else if (size.Empty() && settings.resolution_mode == ResolutionMode::FULL && view.setup.stage == SourcePick::PRESENT) {
    size = input;
  }
  const bool pending = !size.Empty() && !view.first_stage_canvas.Empty() && size != view.first_stage_canvas;
  gui::Separator();
  gui::TextUnformatted(pending ? "Target NR size" : "NR working size");
  const std::string dimensions = size.Empty() ? gui::Display("Unknown") : std::format("{} × {}", size.width, size.height);
  const float remaining = gui::GetContentRegionAvail().x - ImGui::CalcTextSize(dimensions.c_str()).x;
  const float x = gui::GetCursorPosX() + remaining;
  if (remaining >= gui::CalcTextSize(pending ? "Target NR size" : "NR working size").x + gui::GetStyle().ItemSpacing.x) {
    gui::SameLine(); gui::SetCursorPosX(x);
  }
  ImGui::TextUnformatted(dimensions.c_str());
  if (!view.output_size.Empty()) {
    const std::string output = std::format("{}  {} × {}", gui::Display("Output"), view.output_size.width, view.output_size.height);
    widgets::NextGap(style::LabelGap());
    gui::TextDisabled("%s", output.c_str());
  }
}

// Frame skip (MFG-Unlocker's Boost, ported): NR's network on one frame of the setting's cycle, the frames
// between carrying the last change it made. It sits under the NR stage row it qualifies. The Vulkan route
// records through its own pipeline and never skips, so the row is not drawn there at all.
bool DrawHalfRateRow(const OverlayView& view, Settings* settings) {
  if (view.dlss_explicit) return false;
  static constexpr std::array<const char*, 4> options{"Off", "Every 2nd frame", "Every 3rd frame", "Every 4th frame"};
  static constexpr std::array<std::string_view, 4> why{};
  static constexpr std::array<std::string_view, 4> tips{
      "NR's network runs on every frame.",
      "NR's network runs on every other frame. A skipped frame carries the last change the network made along the motion vectors, so it still shows "
      "NR's result rather than the game's raw image.",
      "NR's network runs on one frame in three. The other two carry the last change the network made along the motion vectors, so they still show "
      "NR's result rather than the game's raw image, but an approximation two frames old is on screen two frames in three.",
      "NR's network runs on one frame in four. The other three carry the last change the network made along the motion vectors, so they still show "
      "NR's result rather than the game's raw image, but an approximation three frames old is on screen three frames in four.",
  };
  bool changed = false;
  widgets::SettingCaption("Frame skip", std::string(), false);
  if (const int clicked = widgets::Segmented("##HalfRate", options.data(), 4, static_cast<int>(settings->half_rate), why.data(), tips.data());
      clicked >= 0 && clicked != static_cast<int>(settings->half_rate)) {
    settings->half_rate = static_cast<HalfRate>(clicked);
    changed = true;
  }
  return changed;
}

// The DLSS / Reflex card's sub-captions: the first row of each group names the group. The rows are drawn in
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

// The live DLSS, frame-generation and Reflex overrides: the switches players come to the panel for, not the
// diagnostics and tuning that stay in Advanced.
bool DrawDlssReflexSection(const OverlayView& view, Settings* settings, OverlayState* state, bool& changed) {
  const bool section_changed = SectionChanged(*settings, SettingSection::DLSS_REFLEX);
  widgets::NextGap(style::SectionGap());
  bool reset = false;
  const bool open = widgets::Disclosure(section_changed ? "DLSS / Reflex (changed)###dlss" : "DLSS / Reflex###dlss",
                                        "Presets · Frame generation · Reflex", "dlss",
                                        section_changed && !state->defaults_view ? &reset : nullptr);
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
        if (!line->empty()) gui::TextDisabled("%s", line->c_str());
      }
    }
  }, style::GroupSurface());
  return open;
}

bool DrawSetupGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  bool changed = false;
  widgets::PairedGroups("##setup_groups", [&] {
    widgets::GroupHeading("Frame inputs");
    changed |= DrawMotionRow(view, settings, state);
    changed |= DrawHalfRateRow(view, settings);
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
    // Edge-aware's two band gains and the foveated grid's density rows, right below the choice they depend on.
    changed |= DrawRow(*FindSetting("GuidedBaseGain"), view, settings, state, true);
    changed |= DrawRow(*FindSetting("GuidedDetailGain"), view, settings, state, true);
    changed |= DrawRow(*FindSetting("DensityOnBase"), view, settings, state, true);
    changed |= DrawRow(*FindSetting("EdgeDensityAttenuation"), view, settings, state, true);
    changed |= DrawRow(*FindSetting("Foveation"), view, settings, state, true);
    if (settings->foveation && !FoveationHidden(view, *settings)) {
      changed |= DrawRow(*FindSetting("FoveationWidth"), view, settings, state, true);
      changed |= DrawRow(*FindSetting("FoveationHeight"), view, settings, state, true);
      changed |= DrawRow(*FindSetting("FoveationFalloffX"), view, settings, state, true);
      changed |= DrawRow(*FindSetting("FoveationFalloffY"), view, settings, state, true);
      changed |= DrawRow(*FindSetting("FoveationEdge"), view, settings, state, true);
    }
  });
  return changed;
}

bool DrawLookGroups(const OverlayView& view, Settings* settings, OverlayState* state) {
  static constexpr std::array<const char*, 2> options{"Mine", "Built-in"};
  static constexpr std::array<std::string_view, 2> why{}, tips{"", "The built-in look, read-only; your own settings are kept."};
  {
    if (gui::BeginTable("##compare", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
      gui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, gui::CalcTextSize("Compare").x);
      const float choices_width = gui::CalcTextSize(options[0]).x + gui::CalcTextSize(options[1]).x + 4.f * gui::GetStyle().FramePadding.x + gui::GetStyle().ItemInnerSpacing.x;
      gui::TableSetupColumn("##choices", ImGuiTableColumnFlags_WidthFixed, std::max(choices_width, 12.f * gui::GetFontSize()));
      gui::TableNextRow();
      gui::TableSetColumnIndex(0);
      gui::AlignTextToFramePadding();
      gui::TextUnformatted("Compare");
      gui::TableSetColumnIndex(1);
      gui::PushID("Compare");
      const int pick = widgets::Segmented("##Compare", options.data(), 2, state->defaults_view ? 1 : 0, why.data(), tips.data());
      if (pick >= 0) state->defaults_view = pick == 1;
      gui::PopID();
      gui::EndTable();
    }
  }

  bool changed = false;
  const auto row = [&](const char* key, bool grouped = true) { changed |= DrawRow(*FindSetting(key), view, settings, state, grouped); };
  widgets::NextGap(style::BlockGap());
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
    gui::Separator();
    row("KeepFaces");
    row("FaceProtection");
    row("LightingScale");
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
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, style::LabelGap() * 0.5f});
  const auto fact = [](const char* key, const std::string& value) {
    gui::TableNextRow();
    gui::TableSetColumnIndex(0); gui::TextDisabled("%s", gui::Display(key).c_str());
    gui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", value.c_str());
  };
  if (gui::BeginTable("##runtime_facts", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingStretchProp)) {
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
    gui::EndTable();
  }

  gui::PopStyleVar();
  widgets::NextGap(style::BlockGap());
  const bool compact = gui::GetContentRegionAvail().x < style::Units(112);
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, {style::RowGap(), style::LabelGap()});
  if (gui::BeginTable("##executed_passes", compact ? 2 : 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
    const auto column = [](const char* label, float weight) { gui::TableSetupColumn(gui::Display(label).c_str(), ImGuiTableColumnFlags_WidthStretch, weight); };
    column("Pass", 0.5f);
    if (compact) column("NR evaluation", 3.f);
    else {
      column("Ran at", 1.15f); column("Network / style", 1.3f); column("NR size", 1.f); column("Motion used", 1.25f);
    }
    if (!compact) gui::TableHeadersRow();
    bool any = false;
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
          if (compact) ImGui::TextWrapped("%s  %s", gui::Display(label).c_str(), value.c_str());
          else ImGui::TextWrapped("%s", value.c_str());
        };
        if (compact) gui::TableNextColumn();
        record("Ran at", gui::Display(NR_STAGE_NAMES[i]));
        record("Network / style", name);
        record("NR size", group.resolution.Empty() ? gui::Display("Unknown") : std::format("{} × {}", group.resolution.width, group.resolution.height));
        record("Motion used", gui::Display(motion));
        any = true;
      }
    }
    gui::EndTable();
    if (!any) gui::TextDisabled("No pass recorded.");
  }
  gui::PopStyleVar();
  if (!view.details_note.empty()) gui::TextWrapped("%s", view.details_note.c_str());
  for (const auto& group : view.stage_runs) {
    if (group.skip.empty() || group.planned <= group.count) continue;
    const uint32_t first = group.first + group.count + 1u, last = group.first + group.planned;
    const std::string passes = first == last ? std::format("P{}", first) : std::format("P{}-{}", first, last);
    ImGui::TextWrapped("%s: %s", passes.c_str(), gui::Display(group.skip).c_str());
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
      ImGui::TextWrapped("%s", line.c_str());
    }
  } else {
    state.activity_since = {};
    state.activity_seconds = 0.0;
  }
  gui::TextWrapped("%s", VramLine(view).c_str());
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
    if (gui::CalcTextSize("Press a key (right-click clears)").x + 2.f * gui::GetStyle().FramePadding.x > gui::GetContentRegionAvail().x) gui::NewLine();
    gui::Button("Press a key (right-click clears)");
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
    if (gui::CalcTextSize(key_label.c_str()).x + 2.f * gui::GetStyle().FramePadding.x > gui::GetContentRegionAvail().x) gui::NewLine();
    if (gui::Button(key_label.c_str())) {
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

  gui::SameLine();
  const float min_language_width = ImGui::CalcTextSize(settings->language == Language::ENGLISH ? "English" : "简体中文 / Chinese").x + gui::GetFrameHeight() + style::RowGap();
  if (gui::GetContentRegionAvail().x < min_language_width) gui::NewLine();
  const float language_width = std::min(gui::GetContentRegionAvail().x, std::max(min_language_width, 12.f * gui::GetFontSize()));
  gui::SetCursorPosX(gui::GetCursorPosX() + gui::GetContentRegionAvail().x - language_width);
  gui::SetNextItemWidth(language_width);
  int selected_language = static_cast<int>(settings->language);
  const char* languages[] = {"English", "简体中文 / Chinese"};
  if (ImGui::Combo("##UpliftLanguage", &selected_language, languages, 2)) {
    FindSetting("Language")->set(*settings, selected_language);
    state->language_changed = true;
  }
  if (ImGui::IsItemHovered()) {
    const gui::LanguageScope tooltip_language(settings->language);
    gui::SetTooltip("English / Simplified Chinese. If Chinese appears as boxes, choose a Chinese-capable Global font in ReShade Settings (for example C:\\Windows\\Fonts\\msyh.ttc). No extra Uplift files are needed.");
  }

  widgets::NextGap(style::BlockGap());
  if (!view.safety_locks.empty()) {
    const ImVec4 red = style::SafetyRed();
    const ImVec2 warning_start = gui::GetCursorScreenPos();
    const float inset = style::PanelInset();
    widgets::Inset("##safety_locks", {inset, inset}, [&] {
      gui::PushStyleColor(ImGuiCol_Text, red);
      gui::SeparatorText("SAFETY LATCH ACTIVE");
      gui::PopStyleColor();
      for (const auto& lock : view.safety_locks) {
        gui::PushID(lock.feature.data());
        gui::TextColored(red, "%s", lock.feature.data());
        gui::TextWrapped("%s", lock.reason.c_str());
        if (lock.setting != nullptr) {
          if (settings->*(lock.setting) == lock.restore_value) {
            gui::TextWrapped("Latch cleared for next launch. Restart the game to retry.");
          } else if (gui::Button("Clear latch for next launch")) {
            settings->*(lock.setting) = lock.restore_value;
            changed = true;
          }
        } else if (lock.retry_now) {
          if (gui::Button("Retry now")) state->retry_now = true;
        } else {
          gui::TextWrapped("Restart the game to retry this feature.");
        }
        gui::PopID();
      }
    }, gui::GetColorU32(style::Tint(red, 0.075f)));
    const float bottom = gui::GetCursorScreenPos().y - gui::GetStyle().ItemSpacing.y;
    gui::GetWindowDrawList()->AddRectFilled(warning_start, {warning_start.x + 0.75f * style::LabelGap(), bottom}, gui::GetColorU32(red));
    widgets::NextGap(style::BlockGap());
  }

  const StatusCard& card = view.card;
  const float card_inset = style::PanelInset();
  gui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {style::RowGap(), style::LabelGap()});
  widgets::Inset("##status_card", {card_inset, card_inset}, [&] {
    if (card.working) {
      // Plan 14 (design §1): green while NR works, three rows that say what Setup highlights (keys muted on the left, values in normal text from one column),
      // one muted Why line for a temporary fallback (else the note), and the footer. The height above is unchanged; whatever does not fit is left out.
      gui::TextColored(style::tokens::kAccent, "%s", card.title.c_str());
      const float value_x = gui::GetCursorPosX() + gui::CalcTextSize("Motion vectors").x + 2.f * gui::GetStyle().ItemSpacing.x;
      const auto row = [value_x](const char* key, const std::string& value) {
        gui::TextDisabled("%s", key);
        gui::SameLine(value_x);
        gui::TextWrapped("%s", value.c_str());
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
        gui::PushStyleColor(ImGuiCol_Text, style::tokens::kTextSecondary);
        gui::TextWrapped("%s", why.c_str());
        gui::PopStyleColor();
        if (gui::IsItemHovered()) {
          gui::SetTooltip("%s", why.c_str());
        }
      } else if (!view.live.note.empty()) {
        gui::TextDisabled("%s", view.live.note.c_str());
      }
      if (!view.live.footer.empty()) gui::TextDisabled("%s", view.live.footer.c_str());
    } else {
      gui::TextColored(style::tokens::kWarn, "%s", card.title.c_str());
      if (!card.reason.empty()) {
        gui::TextWrapped("%s", card.reason.c_str());
      }
      for (const std::string& fix : card.fixes) {
        gui::BulletText("%s", fix.c_str());
      }
    }
    switch (card.button) {
      case CardButton::NONE: break;
      case CardButton::RETRY_NOW:
        if (gui::Button("Retry now")) state->retry_now = true;
        break;
      case CardButton::CLEAR_LATCH:  // Each blocked feature has its own explicit recovery action above the card.
        break;
    }
  }, style::GroupSurface(), style::Units(27));
  gui::PopStyleVar();

  const auto rows = [&](std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) changed |= DrawRow(*FindSetting(key), view, settings, state, true);
  };
  // ui-review.md §2 finding 1 / §3: the NR stage and Motion vectors toggles, in pipeline order, at the top
  // of the panel -- the decisions that shape everything else, instead of about the 45th line. Plan 14: Setup.
  widgets::NextGap(style::SectionGap());
  const bool placement_changed = SectionChanged(*settings, SettingSection::PLACEMENT);
  if (widgets::SectionHeading(placement_changed ? "Setup (changed)" : "Setup", "placement", placement_changed && !state->defaults_view)) {
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

  // ui-review.md §2 finding 7: Compare (Mine/Built-in) moves here, the first row of Look.
  widgets::NextGap(style::SectionGap());
  const bool look_changed = SectionChanged(*settings, SettingSection::LOOK);
  if (widgets::SectionHeading(look_changed ? "Look (changed)" : "Look", "look", look_changed && !state->defaults_view)) {
    ResetSection(settings, SettingSection::LOOK);
    changed = true;
  }
  widgets::NextGap(style::BlockGap());
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
    const std::string label = std::format("{}###pass{}", gui::Display(std::format("Pass {}", pass)), pass);
    if (widgets::Disclosure(label.c_str(), summary.c_str())) {
      gui::PushID(static_cast<int>(pass));
      widgets::Inset("##pass_body", {style::PanelInset(), style::PanelInset()}, [&] {
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
        }, style::WellSurface());
      }, style::GroupSurface());
      gui::PopID();
    }
  }

  widgets::NextGap(style::SectionGap());
  bool reset = false;
  const bool shaping_changed = SectionChanged(*settings, SettingSection::RESULT) || SectionChanged(*settings, SettingSection::LIMITS);
  const bool shaping_enabled = state->defaults_view ? DefaultSettings().look.enabled : settings->look.enabled;
  gui::SetNextItemOpen(shaping_enabled, ImGuiCond_Once);
  const bool shaping = widgets::Disclosure(shaping_changed ? "Result shaping (changed)###shaping" : "Result shaping###shaping",
                                           shaping_enabled ? "On" : "Off", "shaping", shaping_changed && !state->defaults_view ? &reset : nullptr);
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
      }, style::WellSurface());
      widgets::NextGap(style::BlockGap());
      widgets::PairedGroups("##limit_groups", [&] {
        widgets::GroupHeading("Limits");
        rows({"MaxBrighten", "MaxDarken", "MaxColorChange"});
      }, [&] {
        widgets::GroupHeading("Detail and stability");
        rows({"LargeScaleTone", "FineDetail", "DetailRadius", "HaloSuppression", "Stabilize", "StabilizeMs", "StabilizeDetail"});
      }, style::WellSurface());
    }
  });

  reset = false;
  const bool mask_changed = SectionChanged(*settings, SettingSection::MASK);
  const bool mask = widgets::Disclosure(mask_changed ? "Mask and HUD (changed)###mask" : "Mask and HUD###mask",
                                        nullptr, "mask", mask_changed && !state->defaults_view ? &reset : nullptr);
  if (reset) { ResetSection(settings, SettingSection::MASK); changed = true; }
  if (mask) widgets::Panel("##mask_body", [&] {
    widgets::PairedGroups("##mask_groups", [&] { widgets::GroupHeading("Mask"); rows({"Mask"}); },
                                         [&] { widgets::GroupHeading("HUD"); rows({"UICorrection"}); }, style::WellSurface());
  });

  reset = false;
  const bool fixes_changed = SectionChanged(*settings, SettingSection::FIXES_COLOR) || SectionChanged(*settings, SettingSection::FIXES_GUIDES);
  const bool fixes = widgets::Disclosure(fixes_changed ? "Fixes (changed)###fixes" : "Fixes###fixes", "Colour · Exposure · Motion",
                                         "fixes", fixes_changed && !state->defaults_view ? &reset : nullptr);
  if (reset) {
    ResetSection(settings, SettingSection::FIXES_COLOR);
    ResetSection(settings, SettingSection::FIXES_GUIDES);
    changed = true;
  }
  if (fixes) widgets::Panel("##fixes_body", [&] {
    gui::TextDisabled("Only change these if you know what you are doing.");
    widgets::PairedGroups("##colour_groups", [&] {
      widgets::GroupHeading("Colour");
      rows({"Encoding", "DiffuseWhiteNits", "TransferStrength", "ColorStrength"});
    }, [&] {
      widgets::GroupHeading("Exposure");
      rows({"InputExposure", "AutoExposureMode", "AutoExposureBlend", "ExposureAdapt", "AdaptBrighterStops", "AdaptDarkerStops"});
    }, style::WellSurface());
    widgets::NextGap(style::BlockGap());
    widgets::PairedGroups("##guide_groups", [&] {
      widgets::GroupHeading("Colour conversion");
      rows({"LinearUnitNits", "SourcePrimaries", "NeuralTransfer", "ChromaClamp", "NearBlackGuard"});
    }, [&] {
      widgets::GroupHeading("Motion and game state");
      rows({"MotionScaleX", "MotionScaleY", "ChainedHistory", "StateRestore"});
    }, style::WellSurface());
  });

  if (widgets::Disclosure("Advanced", "Routing · Runtime · Diagnostics")) widgets::Panel("##advanced_body", [&] {
    widgets::PairedGroups("##advanced_groups", [&] {
      widgets::GroupHeading("Routing");
      rows({"PresentWithFrameGen", "BeforeFGMotion", "VulkanNr", "AllowCrossQueuePasses", "UseD3D9Ex"});
    }, [&] {
      widgets::GroupHeading("Memory and recovery");
      rows({"VramCheck", "BudgetMarginMB", "GraceSeconds", "AutoResume", "AutoRetry"});
    }, style::WellSurface());
    widgets::NextGap(style::BlockGap());
    widgets::Panel("##runtime_group", [&] {
      widgets::GroupHeading("Runtime and logging");
      rows({"SnippetPath", "NgxHooks", "ForeignNr", "LogLevel", "ShowNvIndicator"});
    }, style::WellSurface());
    if (settings->keep_faces) {
      widgets::NextGap(style::BlockGap());
      if (widgets::Disclosure("Face mask tuning", "Keep faces")) widgets::Panel("##face_controls", [&] {
        rows({"ShowFaceMask", "FaceMaskThreshold", "FaceMaskFull", "FaceMaskStrength", "FaceMaskSoftness", "FaceEdgeFalloff",
              "FaceLightingDensity", "FaceSpeckSize", "FaceFillSkin", "FaceFillRadius", "FaceFillTolerance"});
      });
    }
  });
  if (widgets::Disclosure("Details", "Recorded passes")) widgets::Panel("##details_body", [&] { DrawDetails(view, *settings, *state); });
  widgets::NextGap(style::SectionGap());

  // G2: Restore all defaults, confirmed once before it takes effect.
  gui::Separator();
  if (gui::Button("Restore all defaults")) {
    state->confirm_restore = true;
  }
  if (state->confirm_restore) {
    gui::TextWrapped("Every setting returns to its default, the hotkey included; Enable stays as it is.");
    if (gui::Button("Restore all")) {
      RestoreAllDefaults(settings);
      changed = true;
      state->confirm_restore = false;
    }
    gui::SameLine();
    if (gui::Button("Keep my settings")) {
      state->confirm_restore = false;
    }
  }

  changed |= DrawEnableWarning(*FindSetting("PresentWithFrameGen"), settings, state, view.dlss_explicit);
  changed |= DrawEnableWarning(*FindSetting("AllowCrossQueuePasses"), settings, state, view.dlss_explicit);
  return changed;
}

bool DrawOverlay(const OverlayView& view, Settings* settings, OverlayState* state) {
  const style::OverlayStyle theme;
  const auto host = gui::GetStyle().WindowPadding;
  const ImVec2 inset{std::max(0.f, style::BlockGap() - host.x), std::max(0.f, style::BlockGap() - host.y)};
  bool changed = false;
  widgets::Inset("##uplift_sheet", inset, [&] { changed = DrawOverlayContent(view, settings, state); });
  return changed;
}

void BeginPanel() {
  // Where the panel sits: the screen's top-right corner a narrow margin in from both edges, clear of ReShade's
  // own window on the left. The width is 40 font sizes (the font is already this monitor's scale, since the
  // style was applied last frame), and the height follows its content. FirstUseEver for the position, so a
  // player's own placement sticks from then on.
  const float unit = gui::GetFontSize();
  const float margin = unit * 0.5f;
  const float width = unit * 40.f;
  const ImVec2 display = gui::GetIO().DisplaySize;
  gui::SetNextWindowPos(ImVec2(display.x - width - margin, margin), ImGuiCond_FirstUseEver);
  gui::SetNextWindowSize(ImVec2(width, 0.f), ImGuiCond_Always);  // 0 height: fit whatever the content is
  // The height cap: the size above fits the content, which on a long panel is taller than the screen, and the
  // rows past the cap are scrolled to instead. It has to be a constraint -- ImGui applies one to the auto-fit
  // size, while a SetWindowSize after Begin only sets the next frame's size, so the panel still draws full
  // height for that frame and the cap looks like it did nothing.
  gui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, display.y * kMenuMaxScreenFraction), nullptr,
                                    nullptr);
  // NoDocking: ReShade's Dear ImGui is the docking branch, so without it the panel could be dragged into
  // ReShade's own window and become one of its tabs. The panel is a floating window and stays one.
  gui::Begin("GITC Uplift", nullptr,
             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                 | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking);
  // Where the title bar was: a strip across the top that carries the panel's name and drags the window, as the
  // bar it replaces did.
  const float strip_height = gui::GetFrameHeight();
  gui::InvisibleButton("##UpliftTitleBar", ImVec2(gui::GetContentRegionAvail().x, strip_height));
  if (gui::IsItemActive()) {
    const ImVec2 window = gui::GetWindowPos();
    const ImVec2 moved = gui::GetIO().MouseDelta;
    gui::SetWindowPos(ImVec2(window.x + moved.x, window.y + moved.y), ImGuiCond_Always);
  }
  const ImVec2 strip = gui::GetItemRectMin();
  gui::GetWindowDrawList()->AddText(ImVec2(strip.x, strip.y + (strip_height - gui::GetFontSize()) * 0.5f),
                                    style::ToU32(style::tokens::kTextPrimary), "GITC Uplift");
}

void EndPanel() {
  // No resize grip: BeginPanel sizes the window, so the title bar's lower-right corner, which was one, went
  // with the bar.
  gui::End();
}

}  // namespace uplift::ui
