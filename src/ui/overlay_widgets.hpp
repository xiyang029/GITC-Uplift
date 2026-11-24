#pragma once

// Settings rows, controls and formatting shared by the Uplift panel's tabs.
// Widget colours and icons come from overlay_style.hpp; labels go through localized_imgui.hpp.

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <imgui_internal.h>

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

// Keep the panel's font face; headings only change size and color.
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

// Screen coordinates shared by the row label, its control and the reset button.
inline float row_slot_x = 0.f;
inline float row_label_left = 0.f;
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
  row_label_left = at.x;
  row_slot_x = bounds.reset_left;
  const float control_x = at.x + column;
  {
    ImGui::AlignTextToFramePadding();
    StyleScope label_color;
    label_color.Color(ImGuiCol_Text, tokens::kTextSecondary);
    ImGui::TextUnformatted(shown.c_str());
    if (!tip.empty()) {
      ItemTooltip("%s", tip.c_str());
    }
  }
  ImGui::PopFont();
  gui::SameLine();
  gui::SetCursorScreenPos(ImVec2(control_x, gui::GetCursorScreenPos().y));
  gui::SetNextItemWidth((std::max)(1.f, bounds.control_right - control_x));
}

// Align the drawn ink to the row midpoint, not the font's line box. A caption keeps its original left edge.
inline void DrawAlignedText(ImDrawList* draw, ImVec2 anchor, ImU32 ink, const char* text, bool center_x = true) {
  const float x = center_x ? anchor.x - ImGui::CalcTextSize(text).x * 0.5f : anchor.x;
  const int first = draw->VtxBuffer.Size;
  draw->AddText({x, anchor.y - ImGui::GetTextLineHeight() * 0.5f}, ink, text);
  if (first == draw->VtxBuffer.Size) return;
  ImVec2 lo = draw->VtxBuffer[first].pos;
  ImVec2 hi = lo;
  for (int i = first + 1; i < draw->VtxBuffer.Size; ++i) {
    const ImVec2 pos = draw->VtxBuffer[i].pos;
    lo = {(std::min)(lo.x, pos.x), (std::min)(lo.y, pos.y)};
    hi = {(std::max)(hi.x, pos.x), (std::max)(hi.y, pos.y)};
  }
  const float dx = center_x ? std::round(anchor.x - (lo.x + hi.x) * 0.5f) : 0.f;
  const float dy = std::round(anchor.y - (lo.y + hi.y) * 0.5f);
  for (int i = first; i < draw->VtxBuffer.Size; ++i) {
    draw->VtxBuffer[i].pos.x += dx;
    draw->VtxBuffer[i].pos.y += dy;
  }
}

inline void DrawActionIcon(ImDrawList* draw, icons::Action icon, ImVec2 center, ImU32 ink) {
  // A 20 px optical box at the reference body size, with one stroke weight for every action.
  const float scale = ImGui::GetFontSize() / tokens::kBodyFont;
  const float stroke = 3.f * scale;
  const auto point = [&](float x, float y) { return ImVec2(center.x + x * scale, center.y + y * scale); };
  switch (icon) {
    case icons::Action::Reset: {
      constexpr float degree = 0.01745329252f;
      draw->PathArcTo(center, 8.25f * scale, -150.f * degree, 139.f * degree);
      draw->PathStroke(ink, ImDrawFlags_None, stroke);
      const ImVec2 head[]{point(-7.15f, -9.25f), point(-7.15f, -4.125f), point(-1.5f, -4.125f)};
      draw->AddPolyline(head, 3, ink, ImDrawFlags_None, stroke);
      break;
    }
    case icons::Action::Previous:
    case icons::Action::Next: {
      const float dir = icon == icons::Action::Previous ? -1.f : 1.f;
      const ImVec2 chevron[]{point(-4.25f * dir, -8.75f), point(4.25f * dir, 0.f), point(-4.25f * dir, 8.75f)};
      draw->AddPolyline(chevron, 3, ink, ImDrawFlags_None, stroke);
      break;
    }
    case icons::Action::Close:
      draw->AddLine(point(-8.5f, -8.5f), point(8.5f, 8.5f), ink, stroke);
      draw->AddLine(point(-8.5f, 8.5f), point(8.5f, -8.5f), ink, stroke);
      break;
  }
}

inline bool ActionButton(icons::Action icon, const char* id, const ImVec2& size,
                         const ImVec4& ink = tokens::kTextSecondary) {
  StyleScope quiet;
  quiet.FlatButton().Var(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
  const bool clicked = ImGui::Button(id, size);
  if (ImGui::IsItemVisible()) {
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    DrawActionIcon(ImGui::GetWindowDrawList(), icon, {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f}, ToU32(ink));
  }
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
  const bool clicked = ActionButton(icons::Action::Reset, "###reset", {width, height},
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

// Draw the same rail and value column for both numeric types over ImGui's input handling.
template <typename Number>
bool DrawSlider(const char* id, Number* value, Number min, Number max, const char* format,
                const char* display_format, bool* dragging) {
  static_assert(std::is_same_v<Number, float> || std::is_same_v<Number, int>);
  constexpr auto type = std::is_same_v<Number, float> ? ImGuiDataType_Float : ImGuiDataType_S32;
  const float height = ImGui::GetFrameHeight();
  char text[32];
  const auto measure = [&](Number n) {
    std::snprintf(text, sizeof(text), display_format, n);
    return ImGui::CalcTextSize(text).x;
  };
  // Reserve the endpoint widths so the rail does not move when the displayed value gains a digit.
  const float value_width = SnapUp((std::max)({measure(min), measure(max), measure(*value)})) + RowGap();
  const float width = ImGui::CalcItemWidth();
  const bool native = ImGui::TempInputIsActive(ImGui::GetID(id)) || width - value_width < height * 1.5f;
  bool changed = false;
  {
    StyleScope hidden;
    hidden.Var(ImGuiStyleVar_GrabMinSize, height * 0.30f * 2.f);
    if (!native) {
      hidden.Color(ImGuiCol_FrameBg, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgHovered, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgActive, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrab, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrabActive, tokens::kTransparent)
          .Color(ImGuiCol_Text, tokens::kTransparent);
    }
    ImGui::SetNextItemWidth(native ? width : width - value_width);
    changed = ImGui::SliderScalar(id, type, value, &min, &max, format);
  }
  const bool active = ImGui::IsItemActive();
  if (dragging != nullptr) {
    *dragging = active;
  }
  if (native) return changed;

  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  const float target_t = (max > min) ? std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.f, 1.f) : 0.f;
  const bool hot = active || ImGui::IsItemHovered();
  const float t = Animate(AnimationID(id), target_t, active ? motion::kSliderFollow : motion::kHover);
  SliderRail(lo, hi, t, hot, active);

  // Draw the number without adding a layout item, so the slider still owns the row height and hit area.
  const float text_width = measure(*value);
  ImGui::GetWindowDrawList()->AddText(ImVec2(hi.x + value_width - text_width, (lo.y + hi.y - gui::GetTextLineHeight()) * 0.5f),
                                      ImGui::GetColorU32(ImGuiCol_Text), text);
  return changed;
}

// Keep scaled values (such as percentages) in their original storage units.
inline bool Slider(const char* id, float* value, float min, float max, const char* format, bool* dragging = nullptr,
                   float display_scale = 1.f) {
  float shown = *value * display_scale;
  const std::string number_format = gui::Display(format);
  if (!DrawSlider(id, &shown, min, max, format, number_format.c_str(), dragging)) return false;
  *value = shown / display_scale;
  return true;
}

inline bool SliderInt(const char* id, int* value, int min, int max, bool* dragging = nullptr, const char* format = "%d") {
  return DrawSlider(id, value, min, max, format, format, dragging);
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
  if (buffer.size() < capacity) buffer.resize(capacity);
  buffer[text->copy(buffer.data(), capacity - 1u)] = '\0';
  if (!gui::InputText(id, buffer.data(), capacity)) {
    return false;
  }
  *text = buffer.data();
  return true;
}

// ---- Layout ----

// Replace the spacing ImGui already added after the previous item.
inline void NextGap(float gap) { gui::SetCursorPosY(gui::GetCursorPosY() + gap - gui::GetStyle().ItemSpacing.y); }

inline bool PrimaryButton(const char* label, const ImVec2& size = {}) {
  StyleScope accent;
  accent.AccentButton();
  return gui::Button(label, size);
}

inline bool CloseButton(const char* id, float size) {
  return ActionButton(icons::Action::Close, id, {size, size});
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
  if (desired > right - at.x) ImGui::NewLine();
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

struct ChoiceSlide {
  int outgoing;
  float progress;
};

// Track the displayed value, not the click: resets and picks in the other column also animate.
inline ChoiceSlide ChoiceTransition(int current, int count) {
  ImGuiStorage& storage = *ImGui::GetStateStorage();
  const ImGuiID value_id = ImGui::GetID("##choice_value");
  const ImGuiID outgoing_id = ImGui::GetID("##choice_outgoing");
  const ImGuiID time_id = ImGui::GetID("##choice_since");
  const float now = static_cast<float>(ImGui::GetTime());
  const int last = storage.GetInt(value_id, -1);
  int outgoing = storage.GetInt(outgoing_id, current);
  float progress = std::clamp((now - storage.GetFloat(time_id, now)) / motion::kChoiceSlide, 0.f, 1.f);
  if (last != current) {
    // On a quick second click, fade out whichever label is more visible.
    if (motion::EaseOut(progress) >= 0.5f || outgoing == last) outgoing = last;
    if (last < 0 || outgoing < 0 || outgoing >= count) outgoing = current;
    storage.SetInt(value_id, current);
    storage.SetInt(outgoing_id, outgoing);
    storage.SetFloat(time_id, now);
    progress = 0.f;
  }
  if (outgoing == current || outgoing < 0 || outgoing >= count) return {current, 1.f};
  return {outgoing, motion::EaseOut(progress)};
}

inline ChoiceHit ChoiceRow(std::string_view id, const char* caption, const char* const* choices, int count, int value,
                           const std::string_view* why, bool blocked) {
  ChoiceHit hit;
  if (count <= 0 || choices == nullptr) return hit;
  const float height = RowHeight();
  const float font = ImGui::GetFontSize();
  const float gap = RowGap();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const RowBounds bounds = MeasureRow();
  const float row_width = (std::max)(1.f, bounds.reset_left - gap * 0.5f - at.x);
  const int current = std::clamp(value, 0, count - 1);
  row_slot_x = bounds.reset_left;
  ImGui::PushID(id.data(), id.data() + id.size());
  const bool clicked = ImGui::InvisibleButton("##choice", ImVec2(row_width, height), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
  const bool held = ImGui::IsItemActive();
  hit.activated = clicked;
  if (ImGui::IsItemFocused()) hit.nav_id = ImGui::GetItemID();
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 plate_lo(lo.x - gap, lo.y);
  const ImVec2 plate_hi(bounds.right, lo.y + height);
  const auto hover_flags = held ? ImGuiHoveredFlags_AllowWhenBlockedByActiveItem : ImGuiHoveredFlags_None;
  hit.hovered = ImGui::IsWindowHovered(hover_flags) && ImGui::IsMouseHoveringRect(plate_lo, plate_hi);
  const float avail = (std::max)(0.f, bounds.control_right - lo.x);
  const float column = SnapDown((std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare));
  const float zone_lo = lo.x + column;
  const float zone_hi = bounds.control_right;
  const float arrow_width = SnapUp(font * 1.25f);
  const bool show_arrows = zone_hi - zone_lo >= 2.f * arrow_width + 2.f * font;
  const float value_left = zone_lo + (show_arrows ? arrow_width : 0.f);
  const float value_right = zone_hi - (show_arrows ? arrow_width : 0.f);
  const auto usable = [why](int option) { return why == nullptr || why[option].empty(); };
  const auto step = [&](int dir) {
    for (int option = current + dir; option >= 0 && option < count; option += dir) {
      if (usable(option)) return option;
    }
    return -1;
  };
  const int previous = step(-1);
  const int next = step(+1);
  const bool inert = blocked || (previous < 0 && next < 0 && !usable(current));
  const float mouse_x = ImGui::GetIO().MousePos.x;
  const bool over_previous = show_arrows && hovered && previous >= 0 && mouse_x >= zone_lo && mouse_x < value_left;
  const bool over_next = show_arrows && hovered && next >= 0 && mouse_x >= value_right && mouse_x < zone_hi;
  if (clicked && !inert && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    hit.picked = over_previous ? previous : over_next ? next : -1;
  }

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float wash = Animate(ImGui::GetID("##choice_hover"), hit.hovered ? (held ? tokens::kSurfaceActive.w : tokens::kSurfaceHover.w) : 0.f,
                             motion::kChoiceFeedback);
  draw->AddRectFilled(plate_lo, plate_hi, ToU32(tokens::Fade(tokens::kTextPrimary, wash)), ControlRadius());
  const ChoiceSlide slide = ChoiceTransition(current, count);
  const float center_y = lo.y + height * 0.5f;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * tokens::kRowLabelScale);
  const ImVec4 value_ink = inert ? tokens::kTextFaint : tokens::kTextPrimary;
  const std::string caption_text = Ellipsize(gui::Display(caption).c_str(), column - gap);
  DrawAlignedText(draw, {lo.x, center_y}, ToU32(inert ? tokens::kTextFaint : tokens::kTextSecondary), caption_text.c_str(), false);
  if (value_right > value_left) {
    const float value_width = value_right - value_left;
    const float center_x = (value_left + value_right) * 0.5f;
    const auto draw_value = [&](int option, float offset, float opacity) {
      const std::string text = Ellipsize(gui::Display(choices[option]).c_str(), value_width);
      DrawAlignedText(draw, {center_x, center_y + offset}, ToU32(tokens::Fade(value_ink, opacity)), text.c_str());
    };
    if (slide.progress < 1.f) {
      const float shift = (current > slide.outgoing ? 0.8f : -0.8f) * font;
      draw->PushClipRect({value_left, lo.y}, {value_right, plate_hi.y}, true);
      draw_value(slide.outgoing, -shift * slide.progress, 1.f - slide.progress);
      draw_value(current, shift * (1.f - slide.progress), slide.progress);
      draw->PopClipRect();
    } else {
      draw_value(current, 0.f, 1.f);
    }
  }
  ImGui::PopFont();

  if (show_arrows) {
    const auto draw_arrow = [&](const char* key, float x, icons::Action icon, bool enabled, bool hot) {
      const float feedback = Animate(ImGui::GetID(key), enabled && hot ? (held ? 1.f : 0.7f) : 0.f, motion::kChoiceFeedback);
      const ImVec4 ink = enabled ? tokens::Mix(tokens::kTextSecondary, tokens::kAccent, feedback)
                                 : tokens::Fade(tokens::kTextTertiary, 0.32f);
      DrawActionIcon(draw, icon, {x, center_y}, ToU32(ink));
    };
    draw_arrow("##choice_previous", zone_lo + arrow_width * 0.5f, icons::Action::Previous, !blocked && previous >= 0, over_previous);
    draw_arrow("##choice_next", zone_hi - arrow_width * 0.5f, icons::Action::Next, !blocked && next >= 0, over_next);
  }
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
  StyleScope popup;
  popup.Color(ImGuiCol_Border, tokens::Fade(tokens::kTextPrimary, 0.10f))
      .Var(ImGuiStyleVar_WindowBorderSize, 1.f).Var(ImGuiStyleVar_WindowPadding, {inset, inset});
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
  const bool held = gui::IsItemActive();
  gui::EndDisabled();

  const float hover = Animate(ImGui::GetID("##radio_hover"), hot || held ? (held ? 1.f : 0.65f) : 0.f, motion::kChoiceFeedback);
  const float selection = Animate(ImGui::GetID("##radio_selection"), selected ? 1.f : 0.f, motion::kChoiceFeedback);
  const ImVec2 lo = gui::GetItemRectMin(), hi = gui::GetItemRectMax();
  ImDrawList* draw = gui::GetWindowDrawList();
  const ImVec4 wash = tokens::Fade(tokens::kSurfaceActive, tokens::kSurfaceActive.w * hover);
  const ImVec4 selected_fill = tokens::Mix(tokens::kSelected, tokens::kSelectedActive, hover);
  draw->AddRectFilled(lo, hi, ToU32(tokens::Mix(wash, selected_fill, selection)), ControlRadius());

  const float label_y = lo.y + (label_band - label_height) * 0.5f;
  const float text_x = lo.x + text_inset;
  const ImVec4 label_ink = disabled ? tokens::kTextTertiary
                                   : tokens::Mix(tokens::kTextSecondary, tokens::kTextPrimary, selection);
  draw->AddText(gui::GetFont(), font, {text_x, label_y}, ToU32(label_ink),
                text.text.c_str(), nullptr, text.native_width);
  const ImVec2 center{lo.x + padding_x + icon_width * 0.5f, label_y + gui::GetTextLineHeight() * 0.5f};
  const float radius = font * 0.34f;
  const ImVec4 accent = disabled ? tokens::kTextTertiary : tokens::kAccent;
  const ImVec4 ring = tokens::Mix(disabled ? tokens::kTextFaint : tokens::kTextSecondary, accent, (std::max)(selection, hover));
  draw->AddCircle(center, radius, ToU32(ring), 0, (std::max)(1.f, Px(1.f)));
  if (selection > 0.f) {
    draw->AddCircleFilled(center, radius * 0.55f * selection, ToU32(tokens::Fade(accent, selection)));
  }
  if (!reason.text.empty()) {
    draw->AddText(gui::GetFont(), font, {text_x, lo.y + label_band - padding_y * 0.5f},
                  ToU32(tokens::kTextTertiary), reason.text.c_str(), nullptr, reason.native_width);
  }
  gui::PopID();
  return clicked && !selected;
}

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
    quiet.FlatButton();
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
    gui::PushID(i);
    const float selection = Animate(ImGui::GetID("##tab_active"), live ? 1.f : 0.f, motion::kTabSlide);
    const float hover = Animate(ImGui::GetID("##tab_hover"), hot[i] || held[i] ? 1.f : 0.f, motion::kHover);
    gui::PopID();
    // One white surface: the hover wash carries on under the moving plate instead of the two being blended twice.
    const float hover_alpha = (held[i] ? 0.08f : 0.05f) * hover;
    const float plate_alpha = hover_alpha + (1.f - hover_alpha) * selection;
    draw->AddRectFilled(lo[i], hi[i], ToU32(tokens::Fade(tokens::kTextPrimary, plate_alpha)), CapsuleRadius());
    const ImVec2 size = gui::CalcTextSize(shown[i].c_str());
    const ImVec4 inactive_text = tokens::Mix(tokens::kTextSecondary, tokens::kTextPrimary, hover);
    const ImVec4 color = tokens::Mix(inactive_text, tokens::kPlateInk, selection);
    draw->AddText(ImVec2((lo[i].x + hi[i].x - size.x) * 0.5f, (lo[i].y + hi[i].y - size.y) * 0.5f), ToU32(color), shown[i].c_str());
  }
  gui::PopID();
  if (clicked < 0 || clicked == *current) return false;
  *current = clicked;
  return true;
}

}  // namespace uplift::ui::widgets
