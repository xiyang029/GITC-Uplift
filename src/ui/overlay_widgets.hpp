#pragma once

// Uplift's panel controls, ported from FoveatedDLSS5's src/addons/dlss5/ui/widgets.hpp: a settings row with a
// fixed label column and a reserved reset slot, a drawn slider, a switch, a segmented row, section captions
// and collapsible section headers, and the status card's lines. Everything draws with the panel's own tokens
// and icons (ui/overlay_style.hpp), and every string goes through the local catalogue
// (ui/localized_imgui.hpp) exactly once, here, so a caller passes English.
//
// Differences from that source file: Tr() is gui::Display(), the capture lane's hooks (force_next_tooltip,
// reset_buttons_drawn, expand_all_sections) are gone, and Slider reports whether its control is being
// dragged -- Uplift's rows gate work on that -- and what it shows a value as, so a row keeps one storage form.
//
// Include after <imgui.h> and reshade.hpp (through addon/reshade_api.hpp), like ui/overlay_style.hpp.

#include <cstdarg>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

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
// The row RowLabel started put its label on a line of its own (stacked), and where that line is: a
// right-aligned control (a switch) belongs on it, not on the line under it, which is where the cursor has
// already moved by the time the control is drawn.
inline bool row_stacked = false;
inline float row_label_y = 0.f;

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
  row_stacked = stacked;
  row_label_y = at.y;
  row_slot_x = at.x + avail - ImGui::GetFrameHeight();
  // A little more air than the item spacing between a control and the reset slot, so a slider's value and
  // the undo arrow do not read as one crowded pair.
  const float slot_gap = style.ItemSpacing.x * 1.5f;
  // The label column: kLabelEm font sizes wide, never more than kLabelShare of the panel. A label wider than
  // the column keeps its words and pushes its own control right, but it is clipped to the column so one long
  // label in a group cannot move every other row's control.
  const float column = (std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare);
  const float text_width = ImGui::CalcTextSize(text.c_str()).x;
  const float control_x = at.x + (std::min)((std::max)(column, text_width + style.ItemSpacing.x), avail * 0.55f);
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
  row_stacked = false;
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
// `id` keeps the buttons apart; the printf-style tooltip names what a click restores. Returns true when
// clicked. ResetButton below is the setting-row spelling of it.
inline bool RowIconButton(const char* id, const char* tooltip_fmt, ...) {
  const float height = ImGui::GetFrameHeight();
  // The slot RowLabel reserved, on the line the row's control is on. SameLine(0, 0) rather than SameLine():
  // the cursor is then moved to the slot by hand, and a slot that lands outside the visible region -- a
  // narrow panel with a long label -- must not make ImGui wrap the button onto a line of its own.
  ImGui::SameLine(0.f, 0.f);
  const float y = ImGui::GetCursorScreenPos().y;
  const float window_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  const ImVec2 slot((std::min)(row_slot_x, window_right - height), y);
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
    va_list args; va_start(args, tooltip_fmt);
    const std::string tip = gui::Formatted(tooltip_fmt, args);
    va_end(args);
    ItemTooltip("%s", tip.c_str());
  }
  return clicked;
}

inline bool ResetButton(const char* id, const char* default_text) {
  return RowIconButton(id, gui::Display("Reset to default (%s)").c_str(), gui::Display(default_text).c_str());
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

  // The value sits on the rail's own line: SameLine moves the cursor to the next item's y, which after a
  // slider whose width shrank is a line below the rail it belongs to.
  // The value is centred on the rail's own line, as the knob is: the rail's top edge is a knob-radius above
  // the row's centre, so anchoring to it left the number sitting visibly high.
  gui::SetCursorScreenPos(ImVec2(hi.x + value_width - text_width, (lo.y + hi.y - gui::GetTextLineHeight()) * 0.5f));
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

  // The value sits on the rail's own line: SameLine moves the cursor to the next item's y, which after a
  // slider whose width shrank is a line below the rail it belongs to.
  // The value is centred on the rail's own line, as the knob is: the rail's top edge is a knob-radius above
  // the row's centre, so anchoring to it left the number sitting visibly high.
  gui::SetCursorScreenPos(ImVec2(hi.x + value_width - text_width, (lo.y + hi.y - gui::GetTextLineHeight()) * 0.5f));
  gui::TextUnformatted(text);
  return changed;
}

// A link at the right end of the current line (screen x `right`). The caller keeps the line's baseline; the
// link itself neither opens a new line nor advances past it. Returns true when clicked.
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
  // No gap of its own: the call site's NextGap puts the same space above a caption as above a collapsible
  // header, so the two kinds of section start at one rhythm down the panel. The inset is SectionHeader's, so
  // a caption's link and a header's end at one x down the panel.
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - tokens::kCardPadding * 0.75f;
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

// ---- Group boxes: the framed regions the panel's sections are built from. ----

// The preceding item has already advanced by ItemSpacing.y. Replace that gap instead of adding a second margin.
// The total air between the previous item and the next one. ImGui has already advanced the cursor by one
// ItemSpacing when it ended that item, so that much is subtracted back out: a call site asks for the distance it
// wants to see, not for a number added to a gap it cannot measure.
inline void NextGap(float gap) { gui::SetCursorPosY(gui::GetCursorPosY() + gap - gui::GetStyle().ItemSpacing.y); }

// A group's title inside a section, with an optional muted note at the far end and a hairline above both: the
// rows under it read as one group, and the rule says where the group starts. A card's first heading draws
// none: the card's own hairline is already that edge, and a second line on top of it reads as a stray rule.
inline void GroupHeading(const char* title, const char* note = nullptr) {
  const bool first_in_card = ImGui::GetCursorPosY() <= ImGui::GetCursorStartPos().y + 0.5f;
  gui::AlignTextToFramePadding();
  gui::TextUnformatted(title);
  if (note != nullptr) {
    gui::SameLine();
    const float space = gui::GetContentRegionAvail().x - gui::CalcTextSize(note).x;
    if (space >= 0.f) gui::SetCursorPosX(gui::GetCursorPosX() + space);
    else gui::NewLine();
    SecondaryText(note);
  }
  if (!first_in_card) {
    ImGui::GetWindowDrawList()->AddLine(
        ImGui::GetItemRectMin(), ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y), ToU32(tokens::kStroke), 1.f);
  }
}

// A surface sunk into the panel holding `contents`: the panel's card tone, its radius, a hairline edge and the
// panel's own padding. `min_height` keeps a group of one row level with the taller group beside it.
template <typename Contents>
void Panel(const char* id, Contents contents, float min_height = 0.f) {
  StyleScope panel;
  panel.Var(ImGuiStyleVar_WindowPadding, ImVec2(tokens::kCardPadding, tokens::kCardPadding))
      .Var(ImGuiStyleVar_ChildRounding, tokens::kRadiusCard)
      .Var(ImGuiStyleVar_ChildBorderSize, 1.f)
      .Color(ImGuiCol_ChildBg, tokens::kBgCard)
      .Color(ImGuiCol_Border, tokens::kHairline);
  gui::BeginChild(id, ImVec2(0.f, min_height), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
  contents();
  // The card's own bottom edge, measured while its window is still open: after EndChild the parent's last item
  // is not the child, so its rect cannot be asked for afterwards.
  const float card_bottom = gui::GetWindowPos().y + gui::GetWindowSize().y;
  gui::EndChild();
  // A child ends with the cursor further below its own bottom edge than a row ends below its rect, so a card
  // followed by a collapsible header left that header lower than one that follows another header -- which is
  // the gap the DLSS / Reflex card showed. Put the cursor back on the card's edge and let the next item's own
  // advance be the only gap after it, exactly as it is after a row.
  gui::SetCursorScreenPos(ImVec2(gui::GetCursorScreenPos().x, card_bottom + gui::GetStyle().ItemSpacing.y));
}

// Two halves of a section side by side while the panel is wide enough for both, and stacked when it is not:
// ReShade docks the panel narrow, and two columns there leave neither side room for a label beside its control.
// `id` keeps the two halves' ImGui ids apart.
template <typename Left, typename Right>
void PairedGroups(const char* id, Left left, Right right) {
  if (!FitPairedGroups()) {
    gui::PushID(id);
    left();
    NextGap(BlockGap());
    right();
    gui::PopID();
    return;
  }
  // The columns bring no padding of their own: a group's own heading and rows already sit on the card's inset,
  // and a table cell's would inset them again, so the two halves would not line up with the rows above them.
  gui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.f, 0.f));
  if (ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX
                                 | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoSavedSettings)) {
    gui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthStretch);
    gui::TableSetupColumn("##gutter", ImGuiTableColumnFlags_WidthFixed, BlockGap());
    gui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthStretch);
    gui::TableNextRow();
    gui::TableSetColumnIndex(0);
    gui::PushID("##left");
    left();
    gui::PopID();
    gui::TableSetColumnIndex(2);
    gui::PushID("##right");
    right();
    gui::PopID();
    gui::EndTable();
  }
  gui::PopStyleVar();
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
    // The header is the card's own lid: the control tone, one step off the panel behind it -- and off the card
    // it opens when a section nests inside one -- so a section reads as a card even closed, when its body is
    // out of sight. Hover lifts it a step, the press takes the accent. Its corner is the card's, not a
    // control's, since what it opens is a card.
    StyleScope tall;
    tall.Var(ImGuiStyleVar_FramePadding, ImVec2(pad, kPadY))
        .Var(ImGuiStyleVar_FrameRounding, tokens::kRadiusCard)
        .Var(ImGuiStyleVar_FrameBorderSize, 0.f)
        .Color(ImGuiCol_Header, tokens::kBgControl)
        .Color(ImGuiCol_HeaderHovered, tokens::kBgControlHover)
        .Color(ImGuiCol_HeaderActive, tokens::kAccentQuiet)
        .Color(ImGuiCol_Border, tokens::kTransparent);
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
  const bool stacked = row_stacked;
  row_stacked = false;  // a switch consumes the row: the next one is not on this label's line
  if (at_row_end) {
    // On a stacked row the label took the line and the cursor has already moved under it; the switch goes on
    // the label's own line at the row's end, or it reads as a control dropped onto a line of its own.
    const float y = stacked ? row_label_y : ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(row_slot_x - ImGui::GetStyle().ItemSpacing.x * 1.5f - width, y));
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
  // The row's control width, set by RowLabel. A caller that draws a segment row without one gets the
  // remaining width, never a frame that overflows into the next column or a reset slot.
  const float item_width = ImGui::CalcItemWidth();
  const float width = (item_width - kGap * static_cast<float>(count - 1)) / static_cast<float>(count);
  float widest = 0.f;
  for (int i = 0; i < count; ++i) {
    widest = (std::max)(widest, ImGui::CalcTextSize(labels[i]).x);
  }
  if (widest + ImGui::GetStyle().FramePadding.x * 2.f > width) {
    // ImGui draws a combo label to the right of the box, so a caller passing a visible id ("NR stage")
    // leaks that text beside the frame; pass only the part of the id ImGui does not draw.
    const std::string combo_id = (id[0] == '#' && id[1] == '#') ? std::string(id) : std::string("##") + id;
    ImGui::SetNextItemWidth(item_width);  // the same width the segments would have taken
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
  // Hit-test every segment first and draw nothing until they have all been laid out: the surfaces below are
  // then drawn on the rects ImGui measured for those same items, so the track, the live segment's plate and
  // the words cannot drift off one another by a padding or a rounding step. A Button's own frame was the
  // reason the live plate sat lower than its track.
  const float frame_h = ImGui::GetFrameHeight();
  const float rounding = ImGui::GetStyle().FrameRounding;
  ImVec2 seg_min[kMaxSegments];
  ImVec2 seg_max[kMaxSegments];
  bool hot[kMaxSegments] = {};
  for (int i = 0; i < count; ++i) {
    if (i != 0) {
      // The track was measured with kGap between the segments, so the gap drawn between them is kGap: a
      // SameLine() with the style's own spacing drifts the row past the track's right edge by the last segment.
      gui::SameLine(0.f, kGap);
    }
    gui::PushID(i);
    gui::BeginDisabled(!reason(i).empty());
    const bool pressed = ImGui::InvisibleButton("##segment", ImVec2(width, frame_h));
    gui::EndDisabled();
    seg_min[i] = ImGui::GetItemRectMin();
    seg_max[i] = ImGui::GetItemRectMax();
    hot[i] = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    gui::PopID();
    if (pressed) {
      clicked = i;
    }
    if (hot[i]) {
      SegmentTip(reason(i), tip(i));
    }
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 track_lo = seg_min[0];
  const ImVec2 track_hi = seg_max[count - 1];
  // One track behind the segments: the group reads as a single control, and an unselected segment has no
  // surface of its own -- the accent marks the live one. The track takes the control tone, one step off the
  // card it stands on.
  draw->AddRectFilled(track_lo, track_hi, ToU32(tokens::kBgControl), rounding);
  draw->AddRect(track_lo, track_hi, ToU32(tokens::kHairline), rounding, 0, 1.f);
  for (int i = 0; i < count; ++i) {
    const bool greyed = !reason(i).empty();
    if (i == current) {
      // The live option is the accent's wash with its words in the accent.
      draw->AddRectFilled(seg_min[i], seg_max[i], ToU32(tokens::kAccentWash), rounding);
    } else if (hot[i] && !greyed) {
      draw->AddRectFilled(seg_min[i], seg_max[i], ToU32(tokens::kSurfaceHover), rounding);
    }
    const ImVec2 size = ImGui::CalcTextSize(labels[i]);
    const ImVec4 color = greyed ? tokens::Fade(tokens::kTextSecondary, 0.5f)
                                : (i == current ? tokens::kAccent : tokens::kTextPrimary);
    draw->AddText(ImVec2((seg_min[i].x + seg_max[i].x - size.x) * 0.5f, (seg_min[i].y + seg_max[i].y - size.y) * 0.5f),
                  ToU32(color), labels[i]);
  }
  gui::PopID();
  return clicked;
}


// The one action a callout offers -- a safety lock's Clear latch, the status card's Retry: the accent as its
// fill and the window's colour as its words, so it stands out from the rows' quiet controls.
inline bool CardActionButton(const char* label) {
  StyleScope accent;
  accent.AccentButton().Var(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x * 1.5f,
                                                                 ImGui::GetStyle().FramePadding.y));
  return gui::Button(label);
}

}  // namespace uplift::ui::widgets
