#pragma once

// Uplift's panel controls, ported from FoveatedDLSS5's src/addons/dlss5/ui/widgets.hpp: a settings row
// with a fixed label column and a reserved reset slot, a drawn slider, a switch, a segmented row, section
// captions and collapsible section headers, and the status card's lines. Everything draws with style.hpp's
// tokens and icons, and every string goes through the local catalogue (localized_imgui.hpp) exactly once,
// here, so a caller passes English.
//
// Differences from the source file: Tr() is gui::Display(), the capture lane's hooks (force_next_tooltip,
// reset_buttons_drawn, expand_all_sections) are gone, and Slider reports whether its control is being
// dragged -- Uplift's rows gate work on that -- and what it shows a value as, so a row keeps one storage
// form.
//
// Include after <imgui.h> and reshade.hpp (through addon/reshade_api.hpp), like style.hpp.

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "ui/localized_imgui.hpp"
#include "ui/style.hpp"

namespace uplift::ui::widgets {

using namespace style;

// One line of text in a colour that lasts exactly as long as the call: the panel's secondary text is drawn
// in a dozen places, and each used to push and pop the same colour itself.
inline void TextIn(const ImVec4& color, const char* text) {
  StyleScope scope;
  scope.Color(ImGuiCol_Text, color);
  gui::TextUnformatted(text);
}

inline void SecondaryText(const char* text) { TextIn(tokens::kTextSecondary, text); }

inline void SecondaryWrapped(const char* text) {
  StyleScope scope;
  scope.Secondary();
  gui::TextWrapped("%s", text);
}

inline void SmallSecondaryText(const char* text) {
  StyleScope scope;
  scope.FontSize(SmallFontSize()).Secondary();
  gui::TextUnformatted(text);
}

// A tooltip that wraps after kTooltipEm font sizes, as the panel's own tooltips do. The message is drawn
// from what the format came out as; nothing to say, no bubble: ImGui draws an empty one for an empty
// string, which reads as a glitch.
inline void ItemTooltip(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string text = gui::Formatted(fmt, args);
  va_end(args);
  if (text.empty()) {
    return;
  }
  if (!ImGui::BeginItemTooltip()) {
    return;
  }
  {
    StyleScope pad;
    pad.Var(ImGuiStyleVar_WindowPadding, ImVec2(tokens::kTooltipPadX, tokens::kTooltipPadY));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * tokens::kTooltipEm);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::EndTooltip();
}

// `text` cut to fit `width` with "..." at the end, or whole when it fits. The cut falls between UTF-8
// characters: a translated label is Cyrillic, Thai or CJK, and half a character draws as a stray glyph.
inline std::string Ellipsize(const char* text, float width) {
  std::string shown(text);
  if (gui::CalcTextSize(shown.c_str()).x <= width) {
    return shown;
  }
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

// The reset slot of the row RowLabel started, as a screen x, and the x its control starts at.
inline float row_slot_x = 0.f;
inline float row_control_x = 0.f;

// The language combo's width, in font sizes: enough for "简体中文 / Chinese".
constexpr float kLanguageEm = 12.f;

// A settings row: the label in the secondary colour in a fixed-width left column, then the control, then
// the reset slot one frame high at the end. The slot is reserved whether or not the row shows its reset
// button, so every control on the panel starts and ends at the same x. A label wider than its column is
// never cut and never wrapped: it stays on its line in full and pushes the control right, so a row whose
// control is right-aligned anyway (a switch) simply reads as two ends of one line. A row's `tooltip` shows
// over its label as well as its control. On a panel narrower than kStackEm there is no room for a label
// beside a usable control, so the label takes its own line and the control the panel's width under it.
// Call right before a control whose own label is hidden ("##id"): the next item's width is the control
// column.
inline void RowLabel(const char* label, const char* tooltip = nullptr) {
  const std::string text = gui::Display(label);
  const std::string tip = (tooltip != nullptr ? gui::Display(tooltip) : std::string());
  const ImGuiStyle& style = ImGui::GetStyle();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float font = ImGui::GetFontSize();
  const bool stacked = avail < font * tokens::kStackEm;
  if (stacked) {
    ImGui::Dummy(ImVec2(0.f, 0.f));
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  row_slot_x = at.x + avail - ImGui::GetFrameHeight();
  // A little more air than the item spacing between a control and the reset slot, so a slider's value and
  // the undo arrow do not read as one crowded pair.
  const float slot_gap = style.ItemSpacing.x * 1.5f;
  const float column = (std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare);
  const float control_x = (std::max)(at.x + column, at.x + ImGui::CalcTextSize(text.c_str()).x + style.ItemSpacing.x);
  row_control_x = (stacked ? at.x : control_x);
  if (!stacked) {
    ImGui::AlignTextToFramePadding();
  }
  if (!stacked || !text.empty()) {
    SecondaryText(text.c_str());
    if (!tip.empty()) {
      ItemTooltip("%s", tip.c_str());
    }
  }
  if (stacked) {
    gui::SetNextItemWidth(row_slot_x - slot_gap - at.x);
  } else {
    gui::SameLine();
    gui::SetCursorScreenPos(ImVec2(control_x, gui::GetCursorScreenPos().y));
    gui::SetNextItemWidth((std::max)(1.f, row_slot_x - slot_gap - control_x));
  }
}

// A row label with no control column: the control that follows sits right after the label's words instead
// of at the panel's control column. For a row that is only a label and its control -- Enable, whose switch
// and hotkey button are the whole row -- where the column would leave a hand's width of empty panel
// between the two. The label is never cut, since there is no column to cut it to, so its tooltip is the
// caller's own. Call right before the control.
inline void InlineLabel(const char* label, const char* tooltip = nullptr) {
  const std::string text = gui::Display(label);
  ImGui::AlignTextToFramePadding();
  SecondaryText(text.c_str());
  if (tooltip != nullptr) {
    ItemTooltip("%s", gui::Display(tooltip).c_str());
  }
  gui::SameLine();
}

// The reset button in the row RowLabel started: an undo arrow, drawn only for a setting that differs from
// its default (the caller decides). At rest the arrow is dim and turns the accent while the pointer is over
// its cell, so a panel with many changed rows does not turn its whole right edge into a column of arrows.
// `id` keeps the buttons apart; `default_text` names what a click restores. Returns true when clicked.
inline bool ResetButton(const char* id, const char* default_text) {
  ImGui::SameLine();
  const float height = ImGui::GetFrameHeight();
  const ImVec2 slot(row_slot_x, ImGui::GetCursorScreenPos().y);
  const bool over = ImGui::IsWindowHovered()
                    && ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + height, slot.y + height));
  ImGui::SetCursorScreenPos(slot);
  bool clicked = false;
  {
    StyleScope flat;
    flat.FlatButton()
        .Color(ImGuiCol_Text, over ? tokens::kAccent : tokens::Fade(tokens::kTextSecondary, 0.55f))
        .Var(ImGuiStyleVar_FramePadding, ImVec2(0.f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushID(id);
    clicked = gui::Button(icons::kUndo, ImVec2(height, 0.f));
    ImGui::PopID();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_None)) {
    ItemTooltip(gui::Display("Reset to default (%s)").c_str(), gui::Display(default_text).c_str());
  }
  return clicked;
}

// A muted note under the row RowLabel started, starting at that row's control column and in the small size:
// the note belongs to the control above it rather than reading as a line of its own at the panel's edge.
inline void RowNote(const char* text) {
  const std::string shown = gui::Display(text);
  if (shown.empty()) {
    return;
  }
  StyleScope note;
  note.FontSize(SmallFontSize()).Color(ImGuiCol_Text, tokens::kTextSecondary);
  gui::SetCursorScreenPos(ImVec2(row_control_x, gui::GetCursorScreenPos().y));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
  ImGui::TextWrapped("%s", shown.c_str());
  ImGui::PopTextWrapPos();
}

// The rail of a slider over its item's rect: the track, the filled part up to the knob, and the knob, at
// `t` in [0, 1]. `hot` draws the halo while the pointer is on the control or it is held. Both sliders draw
// through this, so an integer row and a float row cannot drift apart.
inline void SliderRail(const ImVec2& lo, const ImVec2& hi, float t, bool hot) {
  const float height = hi.y - lo.y;
  const float knob = height * 0.30f;
  // ImGui's own grab geometry: 2 px of padding, then half the grab.
  const float x0 = lo.x + 2.f + knob;
  const float x1 = hi.x - 2.f - knob;
  const float x = x0 + t * (x1 - x0);
  const float y = (lo.y + hi.y) * 0.5f;
  const float rail = (std::max)(2.f, height * 0.12f);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x1, y + rail * 0.5f), ToU32(tokens::kBgControlHover), rail * 0.5f);
  draw->AddRectFilled(ImVec2(x0, y - rail * 0.5f), ImVec2(x, y + rail * 0.5f), ToU32(tokens::kAccent), rail * 0.5f);
  if (hot) {
    draw->AddCircleFilled(ImVec2(x, y), knob + 2.f, ToU32(tokens::kAccentHalo));
  }
  draw->AddCircleFilled(ImVec2(x, y), knob, ToU32(tokens::kTextPrimary));
  draw->AddCircle(ImVec2(x, y), knob, ToU32(tokens::kHairline), 0, 1.0f);
}

// A slider drawn the modern way: a thin rail filled up to the value, a round knob, and the value in its
// own column on the right, so the number never sits under the knob. ImGui's slider still runs underneath,
// invisible, so dragging, keyboard and gamepad steps and Ctrl+click typing behave as before; while it is
// being typed into, it is drawn as ImGui draws it. Call after RowLabel. `value` is the stored value and
// `display_scale` what the row shows it as (a 0..1 blend shown as 0-100 %), so the caller keeps one
// storage form. Returns true on the frames *value changed; *dragging, when asked for, is ImGui's own "the
// control is held" state for this frame (Uplift's rows hold back work until the release).
inline bool Slider(const char* id, float* value, float min, float max, const char* format, const char* tooltip,
                   bool* dragging = nullptr, float display_scale = 1.f) {
  static ImGuiID typing = 0;
  const ImGuiID item = ImGui::GetID(id);
  const bool was_typing = (typing == item);
  const float height = ImGui::GetFrameHeight();
  const float knob = height * 0.30f;
  float shown = *value * display_scale;
  char text[32];
  std::snprintf(text, sizeof(text), gui::Display(format).c_str(), shown);
  const float text_width = ImGui::CalcTextSize(text).x;
  // The number sits a small gap after its rail, not in a panel-wide column: a column wide enough for the widest
  // value ("500 ms") leaves a short one ("50") most of an em of empty rail. Its right edge still lands on the
  // control column's, so the values line up down the panel.
  const float value_width = text_width + ImGui::GetStyle().ItemSpacing.x * 0.5f;
  const float width = ImGui::CalcItemWidth();
  bool changed = false;
  {
    StyleScope hidden;
    // ImGui centres a grab of GrabMinSize on the value: the knob's diameter, so the knob drawn below sits
    // where the mouse drags it.
    hidden.Var(ImGuiStyleVar_GrabMinSize, knob * 2.f);
    if (!was_typing) {
      hidden.Color(ImGuiCol_FrameBg, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgHovered, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgActive, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrab, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrabActive, tokens::kTransparent)
          .Color(ImGuiCol_Text, tokens::kTransparent);
    }
    ImGui::SetNextItemWidth(was_typing ? width : (std::max)(width - value_width, height * 2.f));
    changed = ImGui::SliderFloat(id, &shown, min, max, format);
  }
  if (changed) {
    *value = shown / display_scale;
  }
  if (tooltip != nullptr) {
    ItemTooltip("%s", tooltip);
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
  if (was_typing) {
    return changed;
  }

  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  const float t = (max > min) ? std::clamp((shown - min) / (max - min), 0.f, 1.f) : 0.f;
  SliderRail(lo, hi, t, active || ImGui::IsItemHovered());

  gui::SameLine(0.f, 0.f);
  gui::SetCursorScreenPos(ImVec2(hi.x + value_width - text_width, gui::GetCursorScreenPos().y));
  gui::TextUnformatted(text);
  return changed;
}

// The integer slider, drawn as Slider: the same rail, knob and value column over ImGui's SliderInt, so a row
// of whole numbers (a percentage, a mode, the pass count) sits in the same visual system as the rest. Call
// after RowLabel. Ctrl+click typing works as on Slider: while the value is typed, ImGui's own slider draws.
// *dragging, when asked for, is ImGui's held state for this frame.
inline bool SliderInt(const char* id, int* value, int min, int max, const char* tooltip, bool* dragging = nullptr) {
  static ImGuiID typing = 0;
  const ImGuiID item = ImGui::GetID(id);
  const bool was_typing = (typing == item);
  const float height = ImGui::GetFrameHeight();
  const float knob = height * 0.30f;
  char text[32];
  std::snprintf(text, sizeof(text), "%d", *value);
  const float text_width = ImGui::CalcTextSize(text).x;
  // The number sits a small gap after its rail, not in a panel-wide column: a column wide enough for the widest
  // value ("500 ms") leaves a short one ("50") most of an em of empty rail. Its right edge still lands on the
  // control column's, so the values line up down the panel.
  const float value_width = text_width + ImGui::GetStyle().ItemSpacing.x * 0.5f;
  const float width = ImGui::CalcItemWidth();
  bool changed = false;
  {
    StyleScope hidden;
    hidden.Var(ImGuiStyleVar_GrabMinSize, knob * 2.f);
    if (!was_typing) {
      hidden.Color(ImGuiCol_FrameBg, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgHovered, tokens::kTransparent)
          .Color(ImGuiCol_FrameBgActive, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrab, tokens::kTransparent)
          .Color(ImGuiCol_SliderGrabActive, tokens::kTransparent)
          .Color(ImGuiCol_Text, tokens::kTransparent);
    }
    ImGui::SetNextItemWidth(was_typing ? width : (std::max)(width - value_width, height * 2.f));
    changed = ImGui::SliderInt(id, value, min, max, "%d");
  }
  if (tooltip != nullptr) {
    ItemTooltip("%s", tooltip);
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
  if (was_typing) {
    return changed;
  }

  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  const float t = (max > min) ? std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.f, 1.f) : 0.f;
  SliderRail(lo, hi, t, active || ImGui::IsItemHovered());

  gui::SameLine(0.f, 0.f);
  gui::SetCursorScreenPos(ImVec2(hi.x + value_width - text_width, gui::GetCursorScreenPos().y));
  gui::TextUnformatted(text);
  return changed;
}

// A group's name inside a section ("Colour", "Motion and game state"): small text in the secondary colour,
// with a little more air above it than a row has, so the rows under it read as its group.
inline void SubCaption(const char* caption) {
  ImGui::Dummy(ImVec2(0.f, tokens::kRowGap * 0.5f));
  SmallSecondaryText(caption);
}

// A link at the right end of the current line (screen x `right`). Returns true when clicked.
inline bool RightLink(const char* label, float right) {
  gui::SameLine();
  gui::SetCursorScreenPos(ImVec2(right - ImGui::CalcTextSize(label).x, gui::GetCursorScreenPos().y));
  return ImGui::TextLink(label);
}

// An always-open section's title: at the panel's own size in the primary colour, on more air than a
// collapsible header gets, since unlike a header it has no row of its own to say where the section starts.
// A changed section carries a small accent dot after the title, and at the right end a link that resets it.
// Returns true on the frame the link was clicked.
inline bool SectionCaption(const char* caption, const char* reset_label, bool modified) {
  ImGui::Dummy(ImVec2(0.f, tokens::kSectionGap * 0.55f));
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  bool clicked = false;
  {
    StyleScope caption_text;
    caption_text.Color(ImGuiCol_Text, tokens::kTextPrimary);
    gui::TextUnformatted(caption);
    if (modified) {
      const ImVec2 lo = ImGui::GetItemRectMin();
      const ImVec2 hi = ImGui::GetItemRectMax();
      const float dot = ImGui::GetFontSize() * 0.13f;
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(hi.x + dot * 2.5f, (lo.y + hi.y) * 0.5f), dot,
                                                 ToU32(tokens::kAccent));
      ImGui::PushID(caption);  // the link's id is its own words: two sections both say "Reset"
      clicked = RightLink(gui::Display(reset_label).c_str(), right);
      ImGui::PopID();
    }
  }
  return clicked;
}

// The gap between two sections: kSectionGap in all, counting the item spacing ImGui adds before and after
// the spacer.
inline void SectionGap() {
  ImGui::Dummy(ImVec2(0.f, (std::max)(0.f, tokens::kSectionGap - 2.f * ImGui::GetStyle().ItemSpacing.y)));
}

// A collapsible section: ImGui's header row, a little taller, a hint in the secondary colour after the
// title, and a reset link at the right end while the section holds a changed setting. A header too narrow
// for the link's words after the title shows the undo arrow instead, with the words on hover; the hint
// shows only while enough of it fits to say something. Returns whether the section is open; *reset is true
// on the frame the link was clicked. The header's id is its English title, so a section keeps its open
// state across a language change.
inline bool SectionHeader(const char* english, const char* hint, const char* reset_label, bool modified, bool* reset) {
  const std::string title = gui::Display(english);
  const std::string hint_text = (hint != nullptr ? gui::Display(hint) : std::string());
  const std::string reset_text = (reset_label != nullptr ? gui::Display(reset_label) : std::string());
  // The header's vertical padding, which is also where the hint's baseline sits: the two must match or the
  // hint drifts off the title line.
  constexpr float kPadY = 7.f;
  const float pad = tokens::kCardPadding * 0.75f;
  const float font = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float right = at.x + ImGui::GetContentRegionAvail().x - pad;
  // ImGui draws a framed header's title after the arrow: one font size plus three paddings in.
  const float title_end = at.x + font + pad * 3.f + ImGui::CalcTextSize(title.c_str()).x;
  bool open = false;
  {
    // A flat header: no filled bar of its own, only a faint wash under the pointer and the accent while
    // held. The title and the section gap already say where a section begins.
    StyleScope tall;
    tall.Var(ImGuiStyleVar_FramePadding, ImVec2(pad, kPadY))
        .Color(ImGuiCol_Header, tokens::kTransparent)
        .Color(ImGuiCol_HeaderHovered, tokens::kSurfaceHover)
        .Color(ImGuiCol_HeaderActive, tokens::kAccentWash);
    open = gui::CollapsingHeader((title + "###" + english).c_str(), ImGuiTreeNodeFlags_AllowOverlap);
  }
  const ImVec2 lo = ImGui::GetItemRectMin();
  float hint_end = right;
  if (reset != nullptr) {
    *reset = false;
  }
  if (modified && !reset_text.empty() && reset != nullptr) {
    const bool words = (title_end + font + ImGui::CalcTextSize(reset_text.c_str()).x <= right);
    const char* shown = (words ? reset_text.c_str() : icons::kUndo);
    hint_end -= ImGui::CalcTextSize(shown).x;
    ImGui::PushID(english);  // the link's id is its own words: every section's link says "Reset"
    *reset = RightLink(shown, right);
    ImGui::PopID();
    if (!words) {
      ItemTooltip("%s", reset_text.c_str());
    }
  }
  if (!hint_text.empty()) {
    const ImVec2 hint_at(title_end + font * 1.25f, lo.y + kPadY);
    const float room = hint_end - font * 1.25f - hint_at.x;
    if (room >= (std::min)(ImGui::CalcTextSize(hint_text.c_str()).x, font * 6.f)) {
      ImGui::GetWindowDrawList()->AddText(hint_at, ToU32(tokens::kTextSecondary),
                                          Ellipsize(hint_text.c_str(), room).c_str());
    }
  }
  return open;
}

// An on/off switch: a rounded track in a quiet accent when on, the knob at the end it points to. By
// default the switch sits at the row's far end, just before the reset slot, whatever the label's width, so
// all rows' switches line up down the panel. A row that already ends in something else (Enable, whose
// hotkey button and whose language combo share the line) passes at_row_end = false: the switch then draws
// where the cursor is, right after the label. Returns true on the frame it flipped *value.
inline bool Switch(const char* id, bool* value, bool at_row_end = true) {
  const float height = ImGui::GetFrameHeight();
  const float width = height * 1.8f;
  const float radius = height * 0.5f;
  if (at_row_end) {
    ImGui::SetCursorScreenPos(
        ImVec2(row_slot_x - ImGui::GetStyle().ItemSpacing.x * 1.5f - width, ImGui::GetCursorScreenPos().y));
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
  if (pressed) {
    *value = !*value;
  }
  const bool hovered = ImGui::IsItemHovered();
  const ImVec4 track = (*value ? (hovered ? tokens::kAccentQuietHover : tokens::kAccentQuiet)
                               : (hovered ? tokens::kBgControlHover : tokens::kBgControl));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ToU32(track), radius);
  draw->AddCircleFilled(ImVec2(*value ? at.x + width - radius : at.x + radius, at.y + radius), radius - 3.5f,
                        ToU32(tokens::kTextPrimary));
  return pressed;
}

// The tooltip of one segment: the option's own reason it cannot be picked, then the option's own text.
// Greyed segments answer a hover as well (ImGuiHoveredFlags_AllowWhenDisabled), since the reason is the
// only thing that explains the grey.
inline void SegmentTip(const std::string_view& why, const std::string_view& tip) {
  std::string text;
  if (!why.empty()) {
    text = std::format("Unavailable here: {}", why);
  }
  if (!tip.empty()) {
    if (!text.empty()) {
      text += "\n";
    }
    text += tip;
  }
  if (!text.empty()) {
    ItemTooltip("%s", text.c_str());
  }
}

// Equal-width buttons in one row, the selected one in the accent colour as a wash, its words in the accent
// and a hairline of the accent around it: the group still says which option is live without a saturated
// block per row. `current` is -1 when no segment matches the setting; `tips` is per segment and shows on
// hover, an empty entry showing nothing. A segment whose `why` is non-empty is greyed and says why on
// hover. The row is the next item's width, which RowLabel sets. Returns the index clicked this frame, or
// -1. A row too narrow for its longest label (ReShade docks the panel at ~450 px) draws a dropdown of the
// same choices instead of clipped words.
inline int Segmented(const char* id, const char* const* english, int count, int current,
                     const std::string_view* why = nullptr, const std::string_view* tips = nullptr) {
  constexpr float kGap = 2.f;
  constexpr int kMaxSegments = 8;
  count = (std::min)(count, kMaxSegments);
  std::string shown[kMaxSegments];  // the local text owns the pointers labels[] hands out
  const char* labels[kMaxSegments] = {};
  for (int i = 0; i < count; ++i) {
    shown[i] = gui::Display(english[i]);
    labels[i] = shown[i].c_str();
  }
  const std::string custom = gui::Display("Custom");  // outlives the combo call below
  const auto reason = [why](int i) -> const std::string_view& {
    static const std::string_view none;
    return (why != nullptr) ? why[i] : none;
  };
  const auto tip = [tips](int i) -> const std::string_view& {
    static const std::string_view none;
    return (tips != nullptr) ? tips[i] : none;
  };
  int clicked = -1;
  const float width = (ImGui::CalcItemWidth() - kGap * static_cast<float>(count - 1)) / static_cast<float>(count);
  float widest = 0.f;
  for (int i = 0; i < count; ++i) {
    widest = (std::max)(widest, ImGui::CalcTextSize(labels[i]).x);
  }
  if (widest + ImGui::GetStyle().FramePadding.x * 2.f > width) {
    // ImGui draws a combo label to the right of the box, so a caller passing a visible id ("NR stage")
    // leaks that text beside the frame; pass only the part of the id ImGui does not draw.
    const std::string combo_id = (id[0] == '#' && id[1] == '#') ? std::string(id) : std::string("##") + id;
    if (gui::BeginCombo(combo_id.c_str(), current >= 0 && current < count ? labels[current] : custom.c_str())) {
      for (int i = 0; i < count; ++i) {
        gui::BeginDisabled(!reason(i).empty());
        if (gui::Selectable(labels[i], i == current)) {
          clicked = i;
        }
        gui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          SegmentTip(reason(i), tip(i));
        }
      }
      gui::EndCombo();
    }
    return clicked;
  }
  gui::PushID(id);
  // One track behind the segments: the group reads as a single control, and an unselected segment has no
  // surface of its own -- the accent marks the live one. The track is the card's own tone with a hairline,
  // quieter than a button, and the border is off inside the row, since a transparent segment would
  // otherwise still draw ImGui's frame hairline.
  const ImVec2 track_at = ImGui::GetCursorScreenPos();
  const float track_width = width * static_cast<float>(count) + kGap * static_cast<float>(count - 1);
  const ImVec2 track_end(track_at.x + track_width, track_at.y + ImGui::GetFrameHeight());
  ImDrawList* track_draw = ImGui::GetWindowDrawList();
  track_draw->AddRectFilled(track_at, track_end, ToU32(tokens::kBgCard), ImGui::GetStyle().FrameRounding);
  track_draw->AddRect(track_at, track_end, ToU32(tokens::kStroke), ImGui::GetStyle().FrameRounding, 0, 1.f);
  StyleScope row;
  row.Var(ImGuiStyleVar_ItemSpacing, ImVec2(kGap, ImGui::GetStyle().ItemSpacing.y))
      .Color(ImGuiCol_Border, tokens::kTransparent);
  for (int i = 0; i < count; ++i) {
    if (i != 0) {
      gui::SameLine();
    }
    const bool greyed = !reason(i).empty();
    bool pressed = false;
    {
      StyleScope segment;
      if (i == current) {
        segment.Color(ImGuiCol_Button, tokens::kAccentWash)
            .Color(ImGuiCol_ButtonHovered, tokens::kAccentHalo)
            .Color(ImGuiCol_ButtonActive, tokens::kAccentStrong)
            .Color(ImGuiCol_Text, tokens::kAccent)
            .Color(ImGuiCol_Border, tokens::kAccentStrong);
      } else {
        segment.Color(ImGuiCol_Button, tokens::kTransparent)
            .Color(ImGuiCol_ButtonHovered, tokens::kSurfaceHover)
            .Color(ImGuiCol_ButtonActive, tokens::kAccentWash);
      }
      gui::BeginDisabled(greyed);
      pressed = gui::Button(labels[i], ImVec2(width, 0.f));
      gui::EndDisabled();
    }
    if (pressed) {
      clicked = i;
    }
    // After the segment's colours pop: a tooltip draws its text in the current ImGuiCol_Text, and the
    // selected segment's is the window background, which on the popup's dark fill left the text unreadable.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      SegmentTip(reason(i), tip(i));
    }
  }
  gui::PopID();
  return clicked;
}

// The language picker in the panel's top-right corner: a combo kLanguageEm font sizes wide, its tooltip,
// and the cursor put back on the line it started at so the panel's rows begin at the panel's edge.
// Returns the newly picked index, or -1.
inline int LanguageCombo(const char* id, const char* const* names, int count, int current, const char* tooltip) {
  const ImVec2 top = ImGui::GetCursorPos();
  const float width = ImGui::GetFontSize() * kLanguageEm;
  ImGui::SetCursorPosX(top.x + (std::max)(0.f, ImGui::GetContentRegionAvail().x - width));
  ImGui::SetNextItemWidth(width);
  int selected = current;
  const bool picked = ImGui::Combo(id, &selected, names, count);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    gui::SetTooltip("%s", tooltip);
  }
  ImGui::SetCursorPos(top);
  return picked ? selected : -1;
}

// The status card: the add-on's state in a fixed-height region, sized for the card's worst case so a
// transient reason or line never shifts the rows below it, and never scrolled. A card is a title, some
// lines, and the caller's own buttons.

// The x the card's values start at, from its longest key, so its keys and its values line up in two
// columns.
inline float CardValueColumn(const char* widest_key) {
  return gui::GetCursorPosX() + gui::CalcTextSize(gui::Display(widest_key).c_str()).x
         + 2.f * ImGui::GetStyle().ItemSpacing.x;
}

// One line of the card, in one piece: cut with "..." at the card's right edge, in full as the tooltip.
// `muted` draws it in the secondary colour.
inline void CardLine(const std::string& source, bool muted) {
  const std::string text = gui::Display(source);
  const std::string shown = Ellipsize(text.c_str(), ImGui::GetContentRegionAvail().x);
  if (muted) {
    SecondaryText(shown.c_str());
  } else {
    gui::TextUnformatted(shown.c_str());
  }
  if (shown.size() != text.size() && ImGui::IsItemHovered()) {
    ItemTooltip("%s", text.c_str());
  }
}

// One key/value row of the card: the key in the secondary colour, the value from `column`.
inline void CardRow(const char* key, const std::string& value, float column) {
  SecondaryText(gui::Display(key).c_str());
  gui::SameLine(column);
  CardLine(value, false);
}

// A card's fix step, in ImGui's own numbering: "1. Restart the game." The step's words are the card's own
// sentence, already in the panel's language, so the line is numbered here and only wrapped there.
inline void CardFixStep(unsigned number, const std::string& text) {
  gui::TextWrapped("%u. %s", number, text.c_str());
}

// The card's muted, wrapped line: the card clips anything longer, and the full text is its tooltip.
inline void CardWrapped(const std::string& text) {
  SecondaryWrapped(text.c_str());
  if (ImGui::IsItemHovered()) {
    ItemTooltip("%s", text.c_str());
  }
}

// The card's own region: its surface, its padding, a hairline edge a step above the panel, and `text_lines`
// lines of room plus a row for its buttons. The padding is inside that height, so the card's contents never
// sit against its rounded edge.
inline void BeginCard(const char* id, float text_lines) {
  StyleScope card;
  card.Var(ImGuiStyleVar_WindowPadding, ImVec2(tokens::kCardPadding, tokens::kCardPadding))
      .Var(ImGuiStyleVar_ChildBorderSize, 1.f)
      .Color(ImGuiCol_ChildBg, tokens::kBgCard)
      .Color(ImGuiCol_Border, tokens::kHairline);
  gui::BeginChild(id,
                  ImVec2(0.f, gui::GetTextLineHeightWithSpacing() * text_lines + gui::GetFrameHeightWithSpacing()
                                      + 2.f * tokens::kCardPadding),
                  ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}
inline void EndCard() { gui::EndChild(); }

// The card's own button: the accent as its fill, the window's colour as its words, so the one thing the card
// asks the player to do stands out from the panel's other controls.
inline bool CardActionButton(const char* label) {
  StyleScope accent;
  accent.AccentButton().Var(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x * 1.5f,
                                                                 ImGui::GetStyle().FramePadding.y));
  return gui::Button(label);
}

// One key/value line of a card: its key in the secondary colour, its value in the panel's.
struct CardLine {
  const char* key;
  std::string value;
};

// Everything the card draws, assembled by the caller from whatever its own state is: the title, an account
// of why it is idle or broken, the fix's steps, and the key/value lines of a working card.
// No model of the add-on's state lives here, so any panel can build a card out of its own facts.
struct CardView {
  bool showing_work = false;        // the lines below describe what runs, not why nothing does
  std::string title;
  std::string reason;               // shown when it is not working
  std::vector<std::string> fixes;   // the fix's steps, numbered, when it is not working
  std::vector<CardLine> lines;      // shown while showing_work
  std::string note;                 // a muted, wrapped line under the lines, else the card's own account of itself
  std::string footer;               // a second muted line
};

// The card's whole body, inside BeginCard: its heading, then either what is running (the key/value lines, its
// note and its footer) or why it is not (the reason, then the fix's steps numbered). The caller draws the
// card's own CardActionButton after this.
inline void DrawStatusCard(const CardView& card) {
  const ImVec2 title_at = ImGui::GetCursorScreenPos();
  gui::TextUnformatted(card.title.c_str());  // the card's first line, at the panel's own size
  // A hairline in the gap under the title: the headline and the card's body read as two layers without
  // spending one of the card's fixed lines on a separator.
  const float rule_y = ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y * 0.5f;
  ImGui::GetWindowDrawList()->AddLine(ImVec2(title_at.x, rule_y),
                                      ImVec2(title_at.x + ImGui::GetContentRegionAvail().x, rule_y),
                                      ToU32(tokens::kStroke), 1.f);
  if (!card.showing_work) {
    if (!card.reason.empty()) {
      CardWrapped(card.reason);
    }
    for (std::size_t step = 0u; step < card.fixes.size(); ++step) {
      CardFixStep(static_cast<unsigned>(step) + 1u, card.fixes[step]);
    }
    return;
  }
  float value_x = 0.f;
  for (const CardLine& line : card.lines) {
    value_x = std::max(value_x, CardValueColumn(line.key));
  }
  for (const CardLine& line : card.lines) {
    CardRow(line.key, line.value, value_x);
  }
  if (!card.note.empty()) {
    CardWrapped(card.note);
  }
  // A muted line, left out when the card's fixed height has no room left for it.
  if (!card.footer.empty() && ImGui::GetContentRegionAvail().y >= ImGui::GetTextLineHeight()) {
    CardLine(card.footer, true);
  }
}

}  // namespace uplift::ui::widgets