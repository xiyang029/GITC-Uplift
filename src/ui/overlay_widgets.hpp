#pragma once

// Settings rows, controls and formatting shared by the Uplift panel's tabs.
// Widget colours and icons come from overlay_style.hpp; labels go through localized_imgui.hpp.

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/overlay_style.hpp"

namespace uplift::ui::widgets {

using namespace style;

// One line of text in a colour that lasts exactly as long as the call: the panel's secondary text is drawn
// in a dozen places, and each used to push and pop the same colour itself.
inline void SecondaryText(const char* text) {
  StyleScope scope;
  scope.Color(ImGuiCol_Text, tokens::kTextSecondary);
  gui::TextUnformatted(text);
}

// Keep the host's real font face, including for Chinese. Headings only change size and color.
inline void Heading(const char* text, const ImVec4& color = tokens::kTextPrimary) {
  StyleScope scope;
  scope.Color(ImGuiCol_Text, color);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * tokens::kHeadingScale);
  gui::TextWrapped("%s", text);
  ImGui::PopFont();
}

// The secondary colour, wrapped to the panel: a reason that runs past one line.
inline void SecondaryWrapped(const char* text) {
  StyleScope scope;
  scope.Color(ImGuiCol_Text, tokens::kTextSecondary);
  gui::TextWrapped("%s", text);
}
// A tooltip opens only once the pointer has rested kTooltipDelay on the same target: sweeping the panel used
// to open a bubble on every row and diagram node it crossed, and close it again in the gaps between them.
inline constexpr float kTooltipDelay = 0.4f;

// The target being rested on, since when, and the frame it last asked in: a frame without a request (the
// pointer between two rows) starts the wait over.
inline ImGuiID tooltip_target = 0;
inline double tooltip_since = 0.0;
inline int tooltip_target_frame = -1;

inline bool TooltipReady(ImGuiID target) {
  const int frame = ImGui::GetFrameCount();
  const double now = ImGui::GetTime();
  if (target != tooltip_target || frame - tooltip_target_frame > 1) {
    tooltip_target = target;
    tooltip_since = now;
  }
  tooltip_target_frame = frame;
  return now - tooltip_since >= kTooltipDelay;
}

// The bubble itself, at the pointer and wrapped after kTooltipEm. An empty string draws nothing: ImGui would
// otherwise show an empty bubble.
inline void Tooltip(ImGuiID target, const std::string& text) {
  if (text.empty() || !TooltipReady(target) || !ImGui::BeginTooltip()) {
    return;
  }
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * tokens::kTooltipEm);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

// A tooltip on the item just drawn. Greyed items answer too: their tooltip is what explains the grey.
inline void ItemTooltip(const char* fmt, ...) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  const std::string text = gui::Formatted(fmt, args);
  va_end(args);
  Tooltip(ImGui::GetItemID(), text);
}

// The same bubble, anchored to the pointer: a row asks once the whole row is drawn and the last item is the
// control's value label, which the pointer is usually not on -- BeginItemTooltip would refuse. The target is
// what the pointer rests on, so moving onto another row or node starts the wait over.
inline void MouseTooltip(ImGuiID target, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string text = gui::Formatted(fmt, args);
  va_end(args);
  Tooltip(target, text);
}

// `text` cut to fit `width` with "..." at the end, or whole when it fits. The cut falls between UTF-8
// characters: a translated label is Cyrillic, Thai or CJK, and half a character draws as a stray glyph.
inline std::string Ellipsize(const char* text, float width) {
  std::string shown(text);
  if (gui::CalcTextSize(shown.c_str()).x <= width) {
    return shown;
  }
  if (ImGui::CalcTextSize("...").x > width) return {};
  while (!shown.empty() && gui::CalcTextSize((shown + "...").c_str()).x > width) {
    while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) {
      shown.pop_back();
    }
    if (!shown.empty()) {
      shown.pop_back();
    }
  }
  return shown + "...";
}

// The left column holds a scrollbar gutter even when its scrollbar is hidden.
inline float SettingsRight() {
  const auto& style = ImGui::GetStyle();
  return ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - style.WindowPadding.x - style.ScrollbarSize;
}

struct RowBounds {
  float right;
  float control_right;
  float reset_left;
};

inline RowBounds MeasureRow() {
  const float left = gui::GetCursorScreenPos().x;
  const float right = left + SnapDown((std::min)(gui::GetContentRegionAvail().x, SettingsRight() - left));
  const float reset_left = right - SnapUp(gui::GetFrameHeight());
  return {right, (std::max)(left + 1.f, reset_left - RowGap()), reset_left};
}

// The reset slot of the row RowLabel started, as a screen x, and the x its control starts at.
inline float row_slot_x = 0.f;
// A right-aligned switch can reuse the label line when it fits.
inline bool row_stacked = false;
inline float row_label_left = 0.f;
inline float row_label_right = 0.f;
// A settings row: a single-line, ellipsised label in the fixed-width left column, then the control, then
// the reset slot one frame high at the end. The slot is reserved whether or not the row shows its reset
// button. The full label remains available in the detail column.
// Call right before a control whose own label is hidden ("##id"): the next item's width is the control
// column.
inline void RowLabel(const char* label, const char* tooltip = nullptr) {
  const std::string text = gui::Display(label);
  const std::string tip = (tooltip != nullptr ? gui::Display(tooltip) : std::string());
  const ImGuiStyle& style = ImGui::GetStyle();
  const float font = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const RowBounds bounds = MeasureRow();
  const float avail = bounds.right - at.x;
  const float column = SnapDown((std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare));
  // The reference panel's rows are labelled in white at a size above the body's; the fetch below measures in
  // the same font it draws in, or a label that fits would still be clipped.
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * tokens::kRowLabelScale);
  const std::string shown = Ellipsize(text.c_str(), (std::max)(1.f, column - style.ItemSpacing.x));
  row_stacked = false;
  row_label_left = at.x;
  row_label_right = at.x;
  row_slot_x = bounds.reset_left;
  const float control_x = at.x + column;
  {
    ImGui::AlignTextToFramePadding();
    StyleScope label_color;
    label_color.Color(ImGuiCol_Text, tokens::kTextSecondary);
    ImGui::TextUnformatted(shown.c_str());
    row_label_right = ImGui::GetItemRectMax().x;
    if (!tip.empty()) {
      ItemTooltip("%s", tip.c_str());
    }
  }
  ImGui::PopFont();
  gui::SameLine();
  gui::SetCursorScreenPos(ImVec2(control_x, gui::GetCursorScreenPos().y));
  gui::SetNextItemWidth((std::max)(1.f, bounds.control_right - control_x));
}

inline bool GlyphButton(const char* glyph, const char* id, const ImVec2& size, float scale,
                        const ImVec4& ink = tokens::kTextSecondary) {
  StyleScope quiet;
  quiet.FlatButton(ink).Var(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
  const std::string label = std::string(glyph) + id;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
  const bool clicked = ImGui::Button(label.c_str(), size);
  ImGui::PopFont();
  return clicked;
}

// The reset arrow uses the slot reserved by its row. Centre it on the last control, before any notes.
inline bool ResetButton(const char* id, const char* default_text) {
  const float height = ImGui::GetFrameHeight();
  const float width = SnapUp(height);
  // This is an overlay control in the row's reserved slot, not another item in the row's layout.
  // Preserve the cursor used by the following note/row so the reset button cannot change their height.
  const ImVec2 layout_cursor = ImGui::GetCursorScreenPos();
  const ImVec2 last_min = ImGui::GetItemRectMin(), last_max = ImGui::GetItemRectMax();
  const float y = last_max.y > last_min.y ? (last_min.y + last_max.y - height) * 0.5f
                                          : layout_cursor.y;
  const float window_right = layout_cursor.x + ImGui::GetContentRegionAvail().x;
  const ImVec2 slot((std::min)(row_slot_x, window_right - width), y);
  const bool over = ImGui::IsWindowHovered()
                    && ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + width, slot.y + height));
  ImGui::SetCursorScreenPos(slot);
  ImGui::PushID(id);
  const bool clicked = GlyphButton(icons::Undo(), "###reset", {width, height}, 1.25f,
                                   over ? tokens::kTextPrimary : tokens::kTextSecondary);
  ImGui::PopID();
  ItemTooltip(gui::Display("Reset to default (%s)").c_str(), gui::Display(default_text).c_str());
  ImGui::SetCursorScreenPos(layout_cursor);
  return clicked;
}

// The rail of a slider over its item's rect: the track, the filled part up to the knob, and the knob, at
// `t` in [0, 1]. `hot` draws the halo while the pointer is on the control or it is held. Both sliders draw
// through this, so an integer row and a float row cannot drift apart.
inline void SliderRail(const ImVec2& lo, const ImVec2& hi, float t, bool hot, bool active) {
  const float height = hi.y - lo.y;
  const float knob = height * 0.30f;
  // ImGui's own grab geometry: 2 px of padding, then half the grab.
  const float inset = (std::min)(2.f + knob, (hi.x - lo.x) * 0.5f);
  const float x0 = lo.x + inset;
  const float x1 = hi.x - inset;
  const float x = x0 + t * (x1 - x0);
  const float y = (lo.y + hi.y) * 0.5f;
  const float rail = (std::max)(Px(2.f), height * 0.12f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x1, y + rail * 0.5f), ToU32(tokens::kBgPopup), rail * 0.5f);
  if (x > x0) {
    draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x, y + rail * 0.5f),
                        ToU32(hot ? tokens::kAccent : tokens::kTextSecondary), rail * 0.5f);
  }
  const float thumb = knob * (active ? 1.18f : hot ? 1.10f : 1.f);
  if (hot) {
    draw->AddCircleFilled(ImVec2(x, y), thumb + Px(active ? 2.8f : 2.f), ToU32(tokens::kAccentHalo));
  }
  draw->AddCircleFilled(ImVec2(x, y), thumb, ToU32(hot ? tokens::kAccent : tokens::kTextPrimary));
}

// A slider drawn the modern way: a thin rail filled up to the value, a round knob, and the value in its
// own column on the right, so the number never sits under the knob. ImGui's slider still runs underneath,
// invisible, so dragging, keyboard and gamepad steps and Ctrl+click typing behave as before; while it is
// being typed into, it is drawn as ImGui draws it. Call after RowLabel. `value` is the stored value and
// `display_scale` what the row shows it as (a 0..1 blend shown as 0-100 %), so the caller keeps one
// storage form. Returns true on the frames *value changed; *dragging, when asked for, is ImGui's own "the
// control is held" state for this frame (Uplift's rows hold back work until the release).
inline bool Slider(const char* id, float* value, float min, float max, const char* format, bool* dragging = nullptr,
                   float display_scale = 1.f) {
  static ImGuiID typing = 0;
  const ImGuiID item = ImGui::GetID(id);
  const bool was_typing = (typing == item);
  const float height = ImGui::GetFrameHeight();
  const float knob = height * 0.30f;
  float shown = *value * display_scale;
  char text[32];
  const std::string number_format = gui::Display(format);
  const auto measure = [&](float n) {
    std::snprintf(text, sizeof(text), number_format.c_str(), n);
    return ImGui::CalcTextSize(text).x;
  };
  // Reserve the endpoint widths so the rail does not move when the displayed value gains a digit.
  const float value_width = SnapUp((std::max)({measure(min), measure(max), measure(shown)})) + RowGap();
  const float width = ImGui::CalcItemWidth();
  const bool native = was_typing || width - value_width < height * 1.5f;
  bool changed = false;
  {
    StyleScope hidden;
    // ImGui centres a grab of GrabMinSize on the value: the knob's diameter, so the knob drawn below sits
    // where the mouse drags it.
    hidden.Var(ImGuiStyleVar_GrabMinSize, knob * 2.f);
    if (!native) {
      hidden.Color(ImGuiCol_FrameBg, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgHovered, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgActive, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrab, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrabActive, tokens::kTransparent)
          .Color(ImGuiCol_Text, tokens::kTransparent);
    }
    ImGui::SetNextItemWidth(native ? width : width - value_width);
    changed = ImGui::SliderFloat(id, &shown, min, max, format);
  }
  if (changed) {
    *value = shown / display_scale;
  }
  const bool active = ImGui::IsItemActive();
  if (dragging != nullptr) {
    *dragging = active;
  }
  // A Ctrl+click turns ImGui's slider into a text field; the text field raises WantTextInput from the next
  // frame on.
  if (active && ImGui::GetIO().WantTextInput) {
    typing = item;
  } else if (was_typing && !active) {
    typing = 0;
  }
  if (native) {
    return changed;
  }

  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  const float target_t = (max > min) ? std::clamp((shown - min) / (max - min), 0.f, 1.f) : 0.f;
  const bool hot = active || ImGui::IsItemHovered();
  const float t = Animate(AnimationID(id), target_t, active ? motion::kSliderFollow : motion::kHover);
  SliderRail(lo, hi, t, hot, active);

  // The value is drawn on the rail's own line, not laid out as an item of the row: it is centred on the rail
  // as the knob is (the rail's top edge is a knob-radius above the row's centre, so anchoring to it left the
  // number sitting visibly high), the next row's gap stays the slider's own height, and the row's "last item"
  // stays the slider instead of a shorter line of text.
  const float text_width = measure(shown);
  ImGui::GetWindowDrawList()->AddText(ImVec2(hi.x + value_width - text_width, (lo.y + hi.y - gui::GetTextLineHeight()) * 0.5f),
                                      ImGui::GetColorU32(ImGuiCol_Text), text);
  return changed;
}

// The integer slider, drawn as Slider: the same rail, knob and value column over ImGui's SliderInt, so a row
// of whole numbers (a percentage, a mode, the pass count) sits in the same visual system as the rest. Call
// after RowLabel. Ctrl+click typing works as on Slider: while the value is typed, ImGui's own slider draws.
// *dragging is ImGui's held state. `format` also applies to the compact, native slider.
inline bool SliderInt(const char* id, int* value, int min, int max, bool* dragging = nullptr, const char* format = "%d") {
  static ImGuiID typing = 0;
  const ImGuiID item = ImGui::GetID(id);
  const bool was_typing = (typing == item);
  const float height = ImGui::GetFrameHeight();
  const float knob = height * 0.30f;
  char text[32];
  const auto measure = [&](int n) {
    std::snprintf(text, sizeof(text), format, n);
    return ImGui::CalcTextSize(text).x;
  };
  const float value_width = SnapUp((std::max)({measure(min), measure(max), measure(*value)})) + RowGap();
  const float width = ImGui::CalcItemWidth();
  const bool native = was_typing || width - value_width < height * 1.5f;
  bool changed = false;
  {
    StyleScope hidden;
    hidden.Var(ImGuiStyleVar_GrabMinSize, knob * 2.f);
    if (!native) {
      hidden.Color(ImGuiCol_FrameBg, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgHovered, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgActive, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrab, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrabActive, tokens::kTransparent)
          .Color(ImGuiCol_Text, tokens::kTransparent);
    }
    ImGui::SetNextItemWidth(native ? width : width - value_width);
    changed = ImGui::SliderInt(id, value, min, max, format);
  }
  const bool active = ImGui::IsItemActive();
  if (dragging != nullptr) {
    *dragging = active;
  }
  if (active && ImGui::GetIO().WantTextInput) {
    typing = item;
  } else if (was_typing && !active) {
    typing = 0;
  }
  if (native) {
    return changed;
  }

  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  const float target_t = (max > min) ? std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.f, 1.f) : 0.f;
  const bool hot = active || ImGui::IsItemHovered();
  const float t = Animate(AnimationID(id), target_t, active ? motion::kSliderFollow : motion::kHover);
  SliderRail(lo, hi, t, hot, active);

  // The value is drawn on the rail's own line, not laid out as an item of the row: it is centred on the rail
  // as the knob is (the rail's top edge is a knob-radius above the row's centre, so anchoring to it left the
  // number sitting visibly high), the next row's gap stays the slider's own height, and the row's "last item"
  // stays the slider instead of a shorter line of text.
  const float text_width = measure(*value);
  ImGui::GetWindowDrawList()->AddText(ImVec2(hi.x + value_width - text_width, (lo.y + hi.y - gui::GetTextLineHeight()) * 0.5f),
                                      ImGui::GetColorU32(ImGuiCol_Text), text);
  return changed;
}

// A typed whole number: the row's control, for a setting whose range is past a slider's (Pass count). ImGui
// draws and edits the field; this says what the caller needs to know about its last frame -- whether the value
// changed, and whether the field is held, which an APPLY_ON_RELEASE row waits for (a typed value applies once on
// release, exactly as a dragged one does). Call after RowLabel.
inline bool NumberField(const char* id, int* value, int step = 1, int fast = 100, bool* active = nullptr) {
  const bool edited = gui::InputInt(id, value, step, fast);
  if (active != nullptr) {
    *active = ImGui::IsItemActive();
  }
  return edited;
}

// A typed text setting: the row's control. The caller's string is copied into a buffer ImGui can edit and written
// back only when it changed; `capacity` is the number of characters the field accepts. Returns true on the frame
// the text changed.
inline bool TextField(const char* id, std::string* text, size_t capacity = 1024) {
  // One buffer for the frame's edit, not one per call: a text row redraws every frame, and this is its only use
  // of <vector>. Grown once, then reused; the copy below always writes the bytes ImGui reads.
  static std::vector<char> buffer;
  if (buffer.size() < capacity) {
    buffer.assign(capacity, '\0');
  } else {
    buffer[0] = '\0';
  }
  text->copy(buffer.data(), capacity - 1u);
  buffer[std::min(text->size(), capacity - 1u)] = '\0';
  if (!gui::InputText(id, buffer.data(), capacity)) {
    return false;
  }
  *text = buffer.data();
  return true;
}

// ---- Layout ----

// The preceding item has already advanced by ItemSpacing.y. Replace that gap instead of adding a second margin.
// The total air between the previous item and the next one. ImGui has already advanced the cursor by one
// ItemSpacing when it ended that item, so that much is subtracted back out: a call site asks for the distance it
// wants to see, not for a number added to a gap it cannot measure.
inline void NextGap(float gap) { gui::SetCursorPosY(gui::GetCursorPosY() + gap - gui::GetStyle().ItemSpacing.y); }

inline bool PrimaryButton(const char* label, const ImVec2& size = {}) {
  StyleScope accent;
  accent.AccentButton();
  return gui::Button(label, size);
}

inline bool CloseButton(const char* id, float size) {
  return GlyphButton(icons::Close(), id, {size, size}, tokens::kHeadingScale);
}

inline bool QuietAction(const char* label) {
  return ImGui::TextLink(gui::Label(label).c_str());
}

inline void Readout(const char* title, const char* value, float right) {
  SecondaryWrapped(title);
  const float x = right - gui::CalcTextSize(value).x;
  if (gui::GetItemRectMax().x + gui::GetStyle().ItemSpacing.x <= x) {
    gui::SameLine();
    gui::SetCursorScreenPos({x, gui::GetCursorScreenPos().y});
    SecondaryText(value);
  } else {
    SecondaryWrapped(value);
  }
}

// Groups use the primary ink. A heading paired with a value is a readout and stays muted.
inline void GroupHeading(const char* title, const char* note = nullptr) {
  if (note) Readout(title, note, MeasureRow().control_right);
  else gui::TextWrapped("%s", title);
}

// A table with the panel's cell padding for exactly as long as it is drawn, and the caller's setup and rows in
// between: the two tables the panel builds (the runtime facts and the executed passes) each pushed and popped
// that padding around their own BeginTable, and each had to check whether ImGui refused the table.
template <typename Body>
void Table(const char* id, int columns, ImGuiTableFlags flags, const ImVec2& padding, Body body) {
  StyleScope cell;
  cell.Var(ImGuiStyleVar_CellPadding, padding);
  if (ImGui::BeginTable(id, columns, flags)) {
    body();
    ImGui::EndTable();
  }
}

// A flat section shares the column's scroll, padding and row grid.
template <typename Contents>
void Section(const char* id, Contents contents) {
  NextGap(BlockGap());
  gui::PushID(id);
  contents();
  gui::PopID();
}

// Place switches flush with the shared control edge, before the reset slot.
inline bool Switch(const char* id, bool* value) {
  const float height = ImGui::GetFrameHeight();
  const float width = SnapUp(height * 1.8f);
  const float center = height * 0.5f;
  const float x = row_slot_x - RowGap() - width;
  if (row_stacked && row_label_right + RowGap() <= x) gui::SameLine();
  gui::SetCursorScreenPos(ImVec2((std::max)(x, row_label_left), ImGui::GetCursorScreenPos().y));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
  if (pressed) {
    *value = !*value;
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const float target = *value ? 1.f : 0.f;
  const float progress = Animate(AnimationID(id), target, motion::kToggle);
  // A small overshoot gives the thumb a soft, spring-like arrival without changing the stored value.
  const float thumb_progress = progress + 0.12f * std::sin(progress * 3.14159265f);
  const ImVec4 active_track = held ? tokens::kSelectedActive : hovered ? tokens::kSelectedHover : tokens::kSelected;
  const ImVec4 track = tokens::Mix(tokens::kBgControl, active_track, progress);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ToU32(track), CardRadius());
  const float thumb_width = held ? center * 0.82f : center * 0.68f;
  const float thumb_x = at.x + center + thumb_progress * (width - 2.f * center);
  draw->AddRectFilled(ImVec2(thumb_x - thumb_width, at.y + center - center * 0.68f),
                      ImVec2(thumb_x + thumb_width, at.y + center + center * 0.68f),
                      ToU32(tokens::kTextPrimary), center * 0.68f);
  return pressed;
}

// A field that captures the next key pressed: the Enable hotkey's button, in both its states. `capturing` draws
// the button that asks for a key, `0` the one that names the key already set (`label` is the caller's, since it
// is a key name and not a sentence to translate). The button goes onto a line of its own when its words do not
// fit the rest of the row, which it measures itself. Returns what the pointer did to it this frame; the caller
// keeps the capture's own state (when it started, whether the overlay closed under it) and decides what a
// right-click means.
struct KeyCaptureHit {
  bool clicked = false;
  bool right_clicked = false;
};
inline KeyCaptureHit KeyCaptureButton(const char* id, const char* label, bool capturing) {
  const char* text = capturing ? "Press a key (right-click clears)" : label;
  const float padding = ImGui::GetStyle().FramePadding.x * 2.f;
  const float desired = SnapUp(gui::CalcTextSize(text).x + padding);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float right = row_slot_x - RowGap();
  if (desired > right - at.x && !row_stacked) ImGui::NewLine();
  const float width = (std::min)(desired, (std::max)(1.f, right - row_label_left));
  ImGui::SetCursorScreenPos({right - width, ImGui::GetCursorScreenPos().y});
  const std::string shown = Ellipsize(gui::Display(text).c_str(), (std::max)(1.f, width - padding)) + "###capture_key";
  ImGui::PushID(id);
  const bool pressed = ImGui::Button(shown.c_str(), {width, 0.f});
  ImGui::PopID();
  return {pressed, ImGui::IsItemClicked(ImGuiMouseButton_Right)};
}

// One row owns the hit area for the caption and both chevrons. A chevron skips unavailable options.
struct ChoiceHit {
  int picked = -1;
  bool hovered = false;
  ImGuiID nav_id = 0;
  bool activated = false;
};

// Measure the glyph quads that ImGui actually drew. The line box and advance include font bearings,
// which differ between the body font and its merged icons.
inline ImVec2 InkCenter(const ImDrawList& draw, int first, ImVec2 fallback) {
  if (first == draw.VtxBuffer.Size) return fallback;
  ImVec2 lo = draw.VtxBuffer[first].pos;
  ImVec2 hi = lo;
  for (int i = first + 1; i < draw.VtxBuffer.Size; ++i) {
    const ImVec2 pos = draw.VtxBuffer[i].pos;
    lo = {(std::min)(lo.x, pos.x), (std::min)(lo.y, pos.y)};
    hi = {(std::max)(hi.x, pos.x), (std::max)(hi.y, pos.y)};
  }
  return {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f};
}

inline void DrawCenteredGlyph(ImDrawList* draw, ImVec2 center, ImU32 color, const char* glyph) {
  const ImVec2 size = ImGui::CalcTextSize(glyph);
  const int first = draw->VtxBuffer.Size;
  draw->AddText({center.x - size.x * 0.5f, center.y - size.y * 0.5f}, color, glyph);
  const ImVec2 ink = InkCenter(*draw, first, center);
  for (int i = first; i < draw->VtxBuffer.Size; ++i) {
    draw->VtxBuffer[i].pos.x += std::round(center.x - ink.x);
    draw->VtxBuffer[i].pos.y += std::round(center.y - ink.y);
  }
}

inline ChoiceHit ChoiceRow(const char* id, const char* caption, const char* const* choices, int count, int value,
                           const std::string_view* why) {
  ChoiceHit hit;
  if (count <= 0 || choices == nullptr) return hit;
  const float height = RowHeight();
  const float font = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const RowBounds bounds = MeasureRow();
  const float avail = bounds.right - at.x;
  const float row_width = (std::max)(1.f, bounds.reset_left - RowGap() * 0.5f - at.x);
  const int current = std::clamp(value, 0, count - 1);
  row_slot_x = bounds.reset_left;
  row_stacked = false;
  row_label_left = at.x;
  ImGui::PushID(id);
  const bool clicked = ImGui::InvisibleButton("##choice", ImVec2(row_width, height), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
  const bool held = ImGui::IsItemActive();
  if (ImGui::IsItemFocused()) hit.nav_id = ImGui::GetItemID();
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 plate_lo(lo.x - RowGap(), lo.y);
  const ImVec2 plate_hi(bounds.right, lo.y + height);
  const auto hover_flags = held ? ImGuiHoveredFlags_AllowWhenBlockedByActiveItem : ImGuiHoveredFlags_None;
  const bool row_hovered = ImGui::IsWindowHovered(hover_flags) && ImGui::IsMouseHoveringRect(plate_lo, plate_hi);
  hit.hovered = row_hovered;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * tokens::kRowLabelScale);
  const float icon_base_size = ImGui::GetStyle().FontSizeBase;
  const float gap = RowGap();
  const float column = SnapDown((std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare));
  const float zone_lo = lo.x + column;
  const float zone_hi = bounds.control_right;
  const float icon_half = std::ceil(font * 0.4f);
  const float hit_half = font * 0.65f;
  // Each icon has the same distance to the text. Only the hit areas extend beyond that spacing.
  const float next_x = zone_hi - icon_half;
  const float value_right = next_x - icon_half - gap;
  const float value_room = value_right - zone_lo - 2.f * icon_half - gap;
  const std::string text = Ellipsize(gui::Display(choices[current]).c_str(), (std::max)(0.f, value_room));
  const float value_x = value_right - ImGui::CalcTextSize(text.c_str()).x;
  const float previous_x = value_x - gap - icon_half;
  const bool show_arrows = previous_x - icon_half >= zone_lo;
  const std::string caption_text = Ellipsize(gui::Display(caption).c_str(), (std::max)(0.f, zone_lo - lo.x - gap));
  const auto usable = [why](int option) { return why == nullptr || why[option].empty(); };
  const auto step = [&](int from, int dir) {
    for (int option = from + dir; option >= 0 && option < count; option += dir) {
      if (usable(option)) return option;
    }
    return -1;
  };
  const int previous = step(current, -1);
  const int next = step(current, +1);
  bool inert = true;  // no option can run: the row greys out the way a disabled control does
  for (int option = 0; option < count && inert; ++option) inert = !usable(option);
  if (clicked) {
    hit.activated = true;
    if (hovered && show_arrows && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const float x = ImGui::GetIO().MousePos.x;
      if (std::abs(x - previous_x) <= hit_half && x <= value_x) hit.picked = previous;
      if (std::abs(x - next_x) <= hit_half && x >= value_right) hit.picked = next;
    }
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  // Hover spans the reset cell. Keyboard focus uses the InvisibleButton's own navigation cursor.
  if (row_hovered) {
    draw->AddRectFilled(plate_lo, plate_hi, ToU32(held ? tokens::kSurfaceActive : tokens::kSurfaceHover), ControlRadius());
  }
  const ImVec4 caption_ink = inert ? tokens::kTextFaint : tokens::kTextSecondary;
  const ImVec4 value_ink = inert ? tokens::kTextFaint : tokens::kTextPrimary;
  const float text_y = lo.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
  draw->AddText({lo.x, text_y}, ToU32(caption_ink), caption_text.c_str());
  if (show_arrows) {
    // The value is drawn where it lands: no transition, so a change is visible on the very next frame.
    const int value_first = draw->VtxBuffer.Size;
    draw->AddText({value_x, text_y}, ToU32(value_ink), text.c_str());
    const float center_y = InkCenter(*draw, value_first, {0.f, lo.y + height * 0.5f}).y;
    ImGui::PushFont(nullptr, icon_base_size * 1.1f);
    const ImVec4 arrow_ink = row_hovered ? tokens::kTextPrimary : tokens::kTextSecondary;
    const ImVec4 unavailable_ink = tokens::Fade(tokens::kTextTertiary, 0.32f);
    DrawCenteredGlyph(draw, {previous_x, center_y}, ToU32(previous >= 0 ? arrow_ink : unavailable_ink), icons::Previous());
    DrawCenteredGlyph(draw, {next_x, center_y}, ToU32(next >= 0 ? arrow_ink : unavailable_ink), icons::Next());
    ImGui::PopFont();
  }
  ImGui::PopFont();
  ImGui::PopID();
  return hit;
}

namespace detail_text {

inline std::pair<char32_t, size_t> ReadUtf8(std::string_view text, size_t at) {
  const auto first = static_cast<unsigned char>(text[at]);
  if (first < 0x80) return {first, 1};
  const size_t n = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : 2;
  if (first < 0xC2 || first > 0xF4 || n > text.size() - at) return {0xFFFD, 1};
  char32_t cp = first & (0x7F >> n);
  for (size_t i = 1; i < n; ++i) {
    const auto byte = static_cast<unsigned char>(text[at + i]);
    if ((byte & 0xC0) != 0x80) return {0xFFFD, 1};
    cp = (cp << 6) | (byte & 0x3F);
  }
  return {cp, n};
}

inline bool IsHan(char32_t cp) {
  return (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)
         || (cp >= 0x20000 && cp <= 0x323AF);
}

struct Wrapped {
  std::string text;
  float native_width;  // zero when the breaks were inserted explicitly
};

inline Wrapped Wrap(std::string text, float width) {
  // All supported Han ranges start with a UTF-8 lead byte of 0xE3 or higher.
  if (std::ranges::none_of(text, [](unsigned char byte) { return byte >= 0xE3; })) {
    return {std::move(text), width};
  }
  struct Glyph { size_t offset; char32_t cp; };
  std::vector<Glyph> chars;
  bool has_han = false;
  for (size_t at = 0; at < text.size();) {
    const auto [cp, size] = ReadUtf8(text, at);
    chars.push_back({at, cp});
    has_han |= IsHan(cp);
    at += size;
  }
  if (!has_han) return {std::move(text), width};
  chars.push_back({text.size(), 0});
  width = (std::max)(1.f, width);

  // ImGui groups unspaced Chinese into a word, which can leave a short "NR" alone on a line.
  // Break between Han characters and words, but not inside "NR"/"5%" or against closing punctuation.
  constexpr std::u32string_view opening = U"（《〈【「『〔［｛“‘([{";
  constexpr std::u32string_view closing = U"，。；：！？、）》〉】」』〕］｝”’％…,.?!:;%)]}";
  const auto blank = [](char32_t cp) { return cp == U' ' || cp == U'\t' || cp == U'　'; };
  std::string out;
  out.reserve(text.size());
  const size_t end = chars.size() - 1;
  size_t start = 0;
  while (start < end) {
    if (chars[start].cp == U'\n') {
      out += '\n';
      ++start;
      continue;
    }
    size_t last_break = start, line_end = start, next = start;
    const char* const line_begin = text.data() + chars[start].offset;
    for (size_t i = start + 1; i <= end; ++i) {
      if (ImGui::CalcTextSize(line_begin, text.data() + chars[i].offset).x > width) {
        line_end = last_break > start ? last_break : (std::max)(start + 1, i - 1);
        next = line_end;
        break;
      }
      const char32_t left = chars[i - 1].cp, right = chars[i].cp;
      if (i == end || right == U'\n') {
        line_end = next = i;
        break;
      }
      if (opening.find(left) == std::u32string_view::npos && closing.find(right) == std::u32string_view::npos
          && (blank(right) || blank(left) || IsHan(left) || IsHan(right) || closing.find(left) != std::u32string_view::npos)) {
        last_break = i;
      }
    }
    while (line_end > start && blank(chars[line_end - 1].cp)) --line_end;
    out.append(line_begin, chars[line_end].offset - chars[start].offset);
    while (next < end && blank(chars[next].cp)) ++next;
    if (next < end && chars[next].cp != U'\n') out += '\n';
    start = next;
  }
  return {std::move(out), 0.f};
}

}  // namespace detail_text

inline void WrappedText(std::string text, float width) {
  const auto wrapped = detail_text::Wrap(std::move(text), width);
  gui::PushTextWrapPos(wrapped.native_width > 0.f ? gui::GetCursorPosX() + width : -1.f);
  ImGui::TextUnformatted(wrapped.text.c_str());
  gui::PopTextWrapPos();
}

inline void DetailText(std::string_view text, const ImVec4& color = tokens::kTextSecondary) {
  StyleScope scope;
  scope.Color(ImGuiCol_Text, color);
  WrappedText(gui::Display(text), gui::GetContentRegionAvail().x);
}

// Keeps the request alive until a button, the close glyph, or Escape dismisses the modal.
inline bool ConfirmDialog(const char* id, std::string_view body, const char* cancel_label, const char* confirm_label,
                          bool& open, float width_units) {
  if (!open) return false;
  const std::string name = gui::Label(id);
  if (!ImGui::IsPopupOpen(name.c_str())) ImGui::OpenPopup(name.c_str());
  const ImVec2 display = gui::GetIO().DisplaySize;
  const float margin = CardGap();
  const float width = std::clamp(display.x - 2.f * margin, 1.f, Units(width_units));
  const float max_height = (std::max)(1.f, display.y - 2.f * margin);
  gui::SetNextWindowSizeConstraints({width, 0.f}, {width, max_height});
  gui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
  const float inset = CardInset();
  const ImVec4 rule = tokens::Fade(tokens::kTextPrimary, 0.10f);
  StyleScope popup;
  popup.Color(ImGuiCol_Border, rule).Color(ImGuiCol_Separator, rule)
      .Var(ImGuiStyleVar_PopupBorderSize, 1.f).Var(ImGuiStyleVar_WindowPadding, {inset, inset});
  constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize
                       | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
  if (!ImGui::BeginPopupModal(name.c_str(), nullptr, flags)) {
    open = false;
    return false;
  }

  const ImVec2 top = gui::GetCursorScreenPos();
  const float content_width = (std::max)(1.f, gui::GetContentRegionAvail().x);
  const float close_size = (std::min)(content_width, SnapUp(gui::GetFontSize() * 1.75f));
  gui::BeginGroup();
  gui::SetCursorScreenPos({top.x + content_width - close_size, top.y});
  const bool close = CloseButton("###dialog_close", close_size);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * tokens::kHeadingScale);
  gui::SetCursorScreenPos({top.x, top.y + (std::max)(0.f, (close_size - gui::GetTextLineHeight()) * 0.5f)});
  WrappedText(name.substr(0, name.find("###")), (std::max)(1.f, content_width - close_size - RowGap()));
  ImGui::PopFont();
  gui::EndGroup();
  NextGap(BlockGap());

  const std::string copy = gui::Display(body);
  for (size_t start = 0;;) {
    const size_t next = copy.find("\n\n", start);
    if (start != 0) NextGap(BlockGap());
    StyleScope paragraph;
    if (start != 0) paragraph.Color(ImGuiCol_Text, tokens::kTextSecondary);
    WrappedText(copy.substr(start, next == std::string::npos ? next : next - start), content_width);
    if (next == std::string::npos) break;
    start = next + 2;
  }

  NextGap(BlockGap());
  gui::Separator();
  NextGap(BlockGap());
  const float height = SnapUp(gui::GetFrameHeight());
  const float gap = RowGap();
  const auto action_width = [content_width](const char* label) {
    const float measured = SnapUp(gui::CalcTextSize(label).x + 2.f * gui::GetStyle().FramePadding.x);
    return (std::min)(content_width, (std::max)(Units(16.f), measured));
  };
  const float cancel_width = action_width(cancel_label), accept_width = action_width(confirm_label);
  const float actions = cancel_width + gap + accept_width;
  const bool same_line = actions <= content_width;
  const float footer_x = gui::GetCursorPosX();
  gui::SetCursorPosX(footer_x + content_width - (same_line ? actions : cancel_width));
  bool cancel;
  {
    StyleScope quiet;
    quiet.FlatButton();
    cancel = gui::Button(cancel_label, {cancel_width, height});
  }
  gui::SetItemDefaultFocus();
  if (same_line) gui::SameLine(0.f, gap);
  else gui::SetCursorPosX(footer_x + content_width - accept_width);
  const bool accept = PrimaryButton(confirm_label, {accept_width, height});
  const bool dismiss = close || cancel || gui::IsKeyPressed(ImGuiKey_Escape);
  if (dismiss || accept) {
    open = false;
    gui::CloseCurrentPopup();
  }
  gui::EndPopup();
  return accept && !dismiss;
}

inline bool DetailOption(const char* label, std::string_view why, int index, bool selected) {
  const bool disabled = !why.empty();
  const float font = gui::GetFontSize();
  const float padding_x = RowGap() * 1.5f;
  const float padding_y = RowGap() * 1.25f;
  const float width = (std::max)(1.f, gui::GetContentRegionAvail().x);
  // Keep one icon slot for every state, so neither the label nor its reason shifts on selection.
  const float icon_width = std::ceil(font * 1.2f);
  const float text_inset = padding_x + icon_width + RowGap();
  const float label_wrap = (std::max)(1.f, width - text_inset - padding_x);
  const auto text = detail_text::Wrap(gui::Display(label), label_wrap);
  const auto reason = detail_text::Wrap(disabled ? gui::Display(why) : std::string(), label_wrap);
  const float label_height = ImGui::CalcTextSize(text.text.c_str(), nullptr, false, text.native_width).y;
  const float reason_height = reason.text.empty() ? 0.f : ImGui::CalcTextSize(reason.text.c_str(), nullptr, false, reason.native_width).y;
  const float label_band = (std::max)(SnapUp(font * 1.85f), label_height + 2.f * padding_y);
  const float height = label_band + (reason.text.empty() ? 0.f : reason_height + padding_y);

  gui::PushID(index);
  gui::BeginDisabled(disabled);
  const bool clicked = gui::InvisibleButton("##option", {width, height}, ImGuiButtonFlags_EnableNav);
  const bool hot = gui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
  gui::EndDisabled();

  const ImVec2 lo = gui::GetItemRectMin(), hi = gui::GetItemRectMax();
  ImDrawList* draw = gui::GetWindowDrawList();
  // The selected row is filled on the frame the choice changes: the highlight is state, not a transition.
  if (selected || hot) {
    const ImVec4 fill = selected ? (hot ? tokens::kSelectedHover : tokens::kSelected) : tokens::kSurfaceHover;
    draw->AddRectFilled(lo, hi, ToU32(fill), ControlRadius());
  }
  const float label_y = lo.y + (label_band - label_height) * 0.5f;
  const float text_x = lo.x + text_inset;
  const int label_first = draw->VtxBuffer.Size;
  draw->AddText(gui::GetFont(), font, {text_x, label_y},
                ToU32(disabled ? tokens::kTextTertiary : selected ? tokens::kTextPrimary : tokens::kTextSecondary),
                text.text.c_str(), nullptr, text.native_width);
  const float line_height = gui::GetTextLineHeight();
  ImVec2 icon_center{lo.x + padding_x + icon_width * 0.5f, label_y + line_height * 0.5f};
  if (label_height <= line_height) icon_center.y = InkCenter(*draw, label_first, icon_center).y;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
  DrawCenteredGlyph(draw, icon_center,
                    ToU32(disabled ? tokens::kTextFaint : selected ? tokens::kAccent : tokens::kTextSecondary),
                    selected ? icons::RadioOn() : icons::RadioOff());
  ImGui::PopFont();
  if (!reason.text.empty()) {
    draw->AddText(gui::GetFont(), font, {text_x, lo.y + label_band - padding_y * 0.5f},
                  ToU32(tokens::kTextTertiary), reason.text.c_str(), nullptr, reason.native_width);
  }
  gui::PopID();
  return clicked && !selected;
}

// A choice in the detail column: a hollow dot, filled with the accent when it is the live one.
inline void DetailLine(const char* text, bool selected) {
  const float font = gui::GetFontSize();
  const ImVec2 at = gui::GetCursorScreenPos();
  const float line = gui::GetTextLineHeight();
  ImDrawList* draw = gui::GetWindowDrawList();
  const ImVec2 center(at.x + font * 0.4f, at.y + line * 0.5f);
  const float radius = std::max(2.f, font * 0.16f);
  const ImU32 ring = ToU32(selected ? tokens::kAccent : tokens::kTextFaint);
  draw->AddCircle(center, radius, ring, 0, std::max(1.f, Px(1.f)));
  if (selected) draw->AddCircleFilled(center, radius * 0.55f, ToU32(tokens::kAccent));
  gui::SetCursorScreenPos(ImVec2(at.x + font * 1.1f, at.y));
  DetailText(text, selected ? tokens::kTextPrimary : tokens::kTextSecondary);
}

// ---- The panel's own controls (2026-11-23) ----

// A tab strip with a white pill for the active tab. It wraps on narrow windows.
inline bool CapsuleTabBar(const char* id, const char* const* english, int count, int* current,
                          const char* action, bool action_enabled, bool* action_pressed) {
  constexpr int kMaxTabs = 12;
  if (current == nullptr || count <= 0 || count > kMaxTabs) return false;
  const float height = SnapUp(RowHeight() * 0.78f);
  const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  const float pad = SnapUp(2.2f * RowGap());
  const float gap = RowGap();
  gui::PushID(id);
  std::string shown[kMaxTabs];
  ImVec2 lo[kMaxTabs] = {};
  ImVec2 hi[kMaxTabs] = {};
  bool hot[kMaxTabs] = {};
  bool held[kMaxTabs] = {};
  float hover_amount[kMaxTabs] = {};
  float active_amount[kMaxTabs] = {};
  float widths[kMaxTabs] = {};
  float largest = 0.f;
  for (int i = 0; i < count; ++i) {
    shown[i] = gui::Display(english[i]);
    widths[i] = SnapUp(ImGui::CalcTextSize(shown[i].c_str()).x + 2.f * pad);
    largest = (std::max)(largest, widths[i]);
  }
  const float left_edge = ImGui::GetCursorScreenPos().x;
  const float action_width = SnapUp(gui::CalcTextSize(action).x + 2.f * pad);
  const bool fits_side = largest + action_width + BlockGap() <= right_edge - left_edge;
  const float tab_edge = fits_side ? right_edge - action_width - BlockGap() : right_edge;
  int clicked = -1;
  for (int i = 0; i < count; ++i) {
    const float width = (std::min)(widths[i], tab_edge - left_edge);
    shown[i] = Ellipsize(shown[i].c_str(), width - 2.f * pad);
    if (i != 0 && hi[i - 1].x + gap + width <= tab_edge) gui::SameLine(0.f, gap);
    gui::PushID(i);
    if (ImGui::InvisibleButton("##tab", ImVec2(width, height), ImGuiButtonFlags_EnableNav)) clicked = i;
    hot[i] = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
    held[i] = ImGui::IsItemActive();
    gui::PopID();
    lo[i] = ImGui::GetItemRectMin();
    hi[i] = ImGui::GetItemRectMax();
  }
  const float action_slot = (std::min)(action_width, right_edge - left_edge);
  if (fits_side) ImGui::SameLine(0.f, 0.f);
  ImGui::SetCursorScreenPos({right_edge - action_slot, ImGui::GetCursorScreenPos().y});
  {
    StyleScope quiet;
    quiet.FlatButton().Var(ImGuiStyleVar_FrameRounding, ControlRadius());
    const std::string shown_action = Ellipsize(gui::Display(action).c_str(), action_slot - 2.f * gap) + "###panel_reset";
    ImGui::BeginDisabled(!action_enabled);
    *action_pressed = ImGui::Button(shown_action.c_str(), {action_slot, height});
    ImGui::EndDisabled();
  }
  ImDrawList* draw = gui::GetWindowDrawList();
  const float baseline = ImGui::GetItemRectMax().y + RowGap();
  const ImVec2 left(lo[0].x, baseline);
  const ImVec2 right(right_edge, baseline);
  draw->AddLine(left, right, ToU32(tokens::Fade(tokens::kTextPrimary, 0.06f)), std::max(1.f, Px(1.f)));
  for (int i = 0; i < count; ++i) {
    const bool live = i == (clicked >= 0 ? clicked : *current);
    gui::PushID(i);  // the per-tab animation ids, without a string per tab per frame
    active_amount[i] = Animate(AnimationID("##tab_active"), live ? 1.f : 0.f, motion::kTabSlide);
    hover_amount[i] = Animate(AnimationID("##tab_hover"), hot[i] || held[i] ? 1.f : 0.f, motion::kHover);
    gui::PopID();
    // One white surface: the hover wash carries on under the moving plate instead of the two being blended twice.
    const float hover_alpha = (held[i] ? 0.08f : 0.05f) * hover_amount[i];
    const float plate_alpha = hover_alpha + (1.f - hover_alpha) * active_amount[i];
    draw->AddRectFilled(lo[i], hi[i], ToU32(tokens::Fade(tokens::kTextPrimary, plate_alpha)), CapsuleRadius());
    const ImVec2 size = gui::CalcTextSize(shown[i].c_str());
    const ImVec4 inactive_text = tokens::Mix(tokens::kTextSecondary, tokens::kTextPrimary, hover_amount[i]);
    const ImVec4 color = tokens::Mix(inactive_text, tokens::kPlateInk, active_amount[i]);
    draw->AddText(ImVec2((lo[i].x + hi[i].x - size.x) * 0.5f, (lo[i].y + hi[i].y - size.y) * 0.5f), ToU32(color), shown[i].c_str());
  }
  gui::PopID();
  if (clicked < 0 || clicked == *current) return false;
  *current = clicked;
  return true;
}

}  // namespace uplift::ui::widgets
