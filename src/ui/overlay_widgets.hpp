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

// Use the host font at a larger size; the body and group labels keep their normal size.
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
// A note wrapped to the panel's width, formatted like the tooltips are: reasons and status lines run past
// one line, and every one of them used to call ImGui::TextWrapped itself.
inline void Wrapped(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string text = gui::Formatted(fmt, args);
  va_end(args);
  gui::TextWrapped("%s", text.c_str());
}

// The frame a control last drew a tooltip in. A row asks for its own bubble once the whole row is drawn
// (overlay.cpp's row end), so without this a segment group inside that row opened a second one beside the
// control's: ImGui opens one window per BeginTooltip call, and both were the same text twice.
inline int tooltip_frame = -1;  // ImGui's frame count is an int, and starts above -1

// True once anything has drawn a tooltip this frame.
inline bool TooltipDrawnThisFrame() { return tooltip_frame == ImGui::GetFrameCount(); }

// The contents of a tooltip: wrapped after kTooltipEm font sizes, as the panel's own tooltips do.
inline void TooltipBody(const std::string& text) {
  tooltip_frame = ImGui::GetFrameCount();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * tokens::kTooltipEm);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
}

// A tooltip on the item just drawn. The message is drawn from what the format came out as; nothing to say, no
// bubble: ImGui draws an empty one for an empty string, which reads as a glitch.
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
  TooltipBody(text);
  ImGui::EndTooltip();
}

// The same bubble, anchored to the pointer rather than to an item: a row asks for its tooltip once the whole
// row is drawn, and the last item by then is the control's own value label, which the pointer is usually not
// on -- BeginItemTooltip would then refuse to show it.
inline void MouseTooltip(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string text = gui::Formatted(fmt, args);
  va_end(args);
  if (text.empty() || !ImGui::BeginTooltip()) {
    return;
  }
  TooltipBody(text);
  ImGui::EndTooltip();
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

// The reset slot of the row RowLabel started, as a screen x, and the x its control starts at.
inline float row_slot_x = 0.f;
// A right-aligned switch can reuse the label line when it fits.
inline bool row_stacked = false;
inline float row_label_left = 0.f;
inline float row_label_right = 0.f;
// Whether the stage diagram is drawing its six nodes on one line. DrawStagePicker sets it every frame, and the
// sections under the diagram take their two columns from it (PairedGroups): one rule for the whole panel, so the
// columns can never appear under a diagram that has folded onto two rows of three.
inline bool stage_columns = false;

// A settings row: the label in the secondary colour in a fixed-width left column, then the control, then
// the reset slot one frame high at the end. The slot is reserved whether or not the row shows its reset
// button. Labels that do not fit the shared column use a full-width line above the control, so long
// translations do not shift the grid. Switches can still share that line if there is room.
// Call right before a control whose own label is hidden ("##id"): the next item's width is the control
// column.
inline void RowLabel(const char* label, const char* tooltip = nullptr) {
  const std::string text = gui::Display(label);
  const std::string tip = (tooltip != nullptr ? gui::Display(tooltip) : std::string());
  const ImGuiStyle& style = ImGui::GetStyle();
  const float avail = ImGui::GetContentRegionAvail().x;
  const float font = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float column = (std::min)(font * tokens::kLabelEm, avail * tokens::kLabelShare);
  const float text_width = ImGui::CalcTextSize(text.c_str()).x;
  const bool stacked = avail < font * tokens::kStackEm || text_width + style.ItemSpacing.x > column;
  row_stacked = stacked;
  row_label_left = at.x;
  row_label_right = at.x;
  row_slot_x = at.x + avail - ImGui::GetFrameHeight();
  // A little more air than the item spacing between a control and the row's end slot, so the undo arrow does
  // not crowd whatever the row ends in. Switch places itself by the same measure.
  const float slot_gap = style.ItemSpacing.x;
  const float control_x = at.x + column;
  {
    ImGui::AlignTextToFramePadding();
    StyleScope label_color;
    label_color.Color(ImGuiCol_Text, tokens::kTextSecondary);
    ImGui::TextWrapped("%s", text.c_str());
    row_label_right = ImGui::GetItemRectMax().x;
    if (!tip.empty()) {
      ItemTooltip("%s", tip.c_str());
    }
  }
  if (stacked) {
    gui::SetNextItemWidth((std::max)(1.f, row_slot_x - slot_gap - at.x));
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
  // The row's end slot, as RowLabel sets it: the caller asks its row's own rect by it.
  row_slot_x = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight();
  const std::string text = gui::Display(label);
  ImGui::AlignTextToFramePadding();
  SecondaryText(text.c_str());
  if (tooltip != nullptr) {
    ItemTooltip("%s", gui::Display(tooltip).c_str());
  }
  gui::SameLine();
}

// The reset button in the row RowLabel started: an undo arrow, drawn only for a setting that differs from
// its default (the caller decides). At rest the arrow is muted and brightens while the pointer is over
// its cell, so a panel with many changed rows does not turn its whole right edge into a column of arrows.
// `id` keeps the buttons apart; the printf-style tooltip names what a click restores. Returns true when
// clicked. ResetButton below is the setting-row spelling of it.
inline bool RowIconButton(const char* id, const char* tooltip_fmt, ...) {
  const float height = ImGui::GetFrameHeight();
  // The slot RowLabel reserved, centred on the control the row just drew: one rule for every row type, the
  // arrow placed from that item's own rect whatever its height. SameLine(0, 0) rather than SameLine(): the
  // cursor is then moved to the slot by hand, and a slot that lands outside the visible region -- a narrow
  // panel with a long label -- must not make ImGui wrap the button onto a line of its own. The caller draws
  // this before any note of the row's: a note is an item of its own, and the arrow would centre on it.
  ImGui::SameLine(0.f, 0.f);
  const ImVec2 last_min = ImGui::GetItemRectMin();
  const ImVec2 last_max = ImGui::GetItemRectMax();
  const float y = last_max.y > last_min.y ? (last_min.y + last_max.y - height) * 0.5f
                                          : ImGui::GetCursorScreenPos().y;
  const float window_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  const ImVec2 slot((std::min)(row_slot_x, window_right - height), y);
  const bool over = ImGui::IsWindowHovered()
                    && ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + height, slot.y + height));
  ImGui::SetCursorScreenPos(slot);
  bool clicked = false;
  {
    StyleScope flat;
    flat.FlatButton()
        .Color(ImGuiCol_Text, over ? tokens::kTextPrimary : tokens::kTextSecondary)
        .Var(ImGuiStyleVar_FramePadding, ImVec2(0.f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushID(id);
    clicked = gui::Button(icons::kUndo, ImVec2(height, 0.f));
    ImGui::PopID();
  }
  if (ImGui::IsItemHovered()) {
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
                        ToU32(tokens::kTextSecondary), rail * 0.5f);
  }
  if (hot) {
    draw->AddCircleFilled(ImVec2(x, y), knob + Px(2.f), ToU32(tokens::kAccentHalo));
  }
  draw->AddCircleFilled(ImVec2(x, y), knob, ToU32(hot ? tokens::kAccent : tokens::kTextPrimary));
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
  const float value_width = (std::max)({measure(min), measure(max), measure(shown)}) + LabelGap();
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
  const float t = (max > min) ? std::clamp((shown - min) / (max - min), 0.f, 1.f) : 0.f;
  SliderRail(lo, hi, t, active || ImGui::IsItemHovered());

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
  const float value_width = (std::max)({measure(min), measure(max), measure(*value)}) + LabelGap();
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
  const float t = (max > min) ? std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.f, 1.f) : 0.f;
  SliderRail(lo, hi, t, active || ImGui::IsItemHovered());

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

// A link at the right end of the current line (screen x `right`). The caller keeps the line's baseline; the
// link itself neither opens a new line nor advances past it. Returns true when clicked.
inline bool RightLink(const char* label, float right) {
  gui::SameLine();
  gui::SetCursorScreenPos(ImVec2(right - ImGui::CalcTextSize(label).x, gui::GetCursorScreenPos().y));
  return ImGui::TextLink(label);
}

// An always-open section's title and its quiet reset command.
inline bool SectionCaption(const char* caption, const char* reset_label = nullptr, bool modified = false) {
  const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  Heading(caption);
  if (!modified || reset_label == nullptr) return false;
  const std::string label = gui::Display(reset_label);
  ImGui::PushID(caption);
  const bool clicked = ImGui::GetItemRectMax().x + RowGap() + ImGui::CalcTextSize(label.c_str()).x <= right
      ? RightLink(label.c_str(), right) : ImGui::TextLink(label.c_str());
  ImGui::PopID();
  return clicked;
}

// ---- Layout ----

// The preceding item has already advanced by ItemSpacing.y. Replace that gap instead of adding a second margin.
// The total air between the previous item and the next one. ImGui has already advanced the cursor by one
// ItemSpacing when it ended that item, so that much is subtracted back out: a call site asks for the distance it
// wants to see, not for a number added to a gap it cannot measure.
inline void NextGap(float gap) { gui::SetCursorPosY(gui::GetCursorPosY() + gap - gui::GetStyle().ItemSpacing.y); }

// Groups are separated by whitespace. Their heading stays at body size, using brighter ink than row labels.
inline void GroupHeading(const char* title, const char* note = nullptr) {
  const float width = gui::GetContentRegionAvail().x;
  const float right = gui::GetCursorScreenPos().x + width;
  gui::TextWrapped("%s", title);
  if (note == nullptr) return;

  const float note_width = gui::CalcTextSize(note).x;
  if (gui::GetItemRectMax().x + gui::GetStyle().ItemSpacing.x <= right - note_width) {
    gui::SameLine();
    gui::SetCursorScreenPos(ImVec2(right - note_width, gui::GetCursorScreenPos().y));
    // A wrap width equal to the measured text width can round down at fractional font scales.
    SecondaryText(note);
  } else if (note_width <= width) {
    SecondaryText(note);
  } else {
    SecondaryWrapped(note);
  }
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

template <typename Contents>
void Panel(const char* id, Contents contents) {
  StyleScope panel;
  panel.Var(ImGuiStyleVar_WindowPadding, ImVec2(BlockGap(), BlockGap()))
      .Color(ImGuiCol_ChildBg, tokens::kBgCard);
  // Cards grow with their contents; their parent owns the scrolling. NoScrollbar would block forwarding.
  gui::BeginChild(id, ImVec2(0.f, 0.f), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                  ImGuiWindowFlags_NoScrollWithMouse);
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

// Two halves of a section side by side while the stage diagram above draws its six nodes on one line, and
// stacked when it folds onto two rows of three: the panel has one width rule for this (stage_columns), so two
// columns never appear under a diagram that has folded. `id` keeps the two halves' ImGui ids apart.
template <typename Left, typename Right>
void PairedGroups(const char* id, Left left, Right right) {
  if (!stage_columns) {
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
  const float kPadY = RowGap();
  const float pad = 2.f * LabelGap();
  const float font = ImGui::GetFontSize();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float right = at.x + ImGui::GetContentRegionAvail().x - pad;
  // ImGui draws a framed header's title after the arrow: one font size plus three paddings in.
  const float title_end = at.x + font + pad * 3.f + ImGui::CalcTextSize(title.c_str()).x;
  bool open = false;
  {
    // A closed section is a card-sized entry, not a primary action.
    StyleScope tall;
    tall.Var(ImGuiStyleVar_FramePadding, ImVec2(pad, kPadY))
        .Var(ImGuiStyleVar_FrameRounding, Px(tokens::kRadiusCard))
        .Color(ImGuiCol_Header, tokens::kBgCard)
        .Color(ImGuiCol_HeaderHovered, tokens::kBgControlHover)
        .Color(ImGuiCol_HeaderActive, tokens::kBgControlActive);
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
    const float x = row_slot_x - RowGap() - width;
    if (stacked && row_label_right + RowGap() <= x) gui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2((std::max)(x, row_label_left), ImGui::GetCursorScreenPos().y));
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
  if (pressed) {
    *value = !*value;
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const ImVec4 track = *value ? (held ? tokens::kSelectedActive : hovered ? tokens::kSelectedHover : tokens::kSelected)
                             : (held ? tokens::kBgControlActive : hovered ? tokens::kBgControlHover : tokens::kBgControl);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ToU32(track), radius);
  draw->AddCircleFilled(ImVec2(*value ? at.x + width - radius : at.x + radius, at.y + radius), radius * 0.68f,
                        ToU32(*value ? tokens::kAccent : tokens::kTextSecondary));
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

// A dropdown for long or numerous choices. `why` and `tips` are optional per-item text.
inline int Combo(const char* id, const char* const* english, int count, int preview,
                 const std::string_view* why = nullptr, const std::string_view* tips = nullptr,
                 const char* missing_preview = nullptr) {
  const std::string combo_id = (id[0] == '#' && id[1] == '#') ? std::string(id) : std::string("##") + id;
  int picked = -1;
  if (gui::BeginCombo(combo_id.c_str(), preview >= 0 && preview < count ? english[preview] : missing_preview)) {
    for (int i = 0; i < count; ++i) {
      const std::string_view reason = why ? why[i] : std::string_view{};
      gui::BeginDisabled(!reason.empty());
      if (gui::Selectable(english[i], i == preview)) {
        picked = i;
      }
      gui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        SegmentTip(reason, tips ? tips[i] : std::string_view{});
      }
    }
    gui::EndCombo();
  }
  return picked;
}

// Equal-width buttons in one neutral track, with a tinted plate for the selected option.
// `current` is -1 when no segment matches the setting; `tips` is per segment and shows on
// hover, an empty entry showing nothing. A segment whose `why` is non-empty is greyed and says why on
// hover. The row is the next item's width, which RowLabel sets. Returns the index clicked this frame, or
// -1. A row too narrow for its longest label (ReShade docks the panel at ~450 px) draws a dropdown of the
// same choices instead of clipped words.
inline int Segmented(const char* id, const char* const* english, int count, int current,
                     const std::string_view* why = nullptr, const std::string_view* tips = nullptr) {
  const float kGap = LabelGap() * 0.5f;
  constexpr int kMaxSegments = 8;
  if (count <= 0) return -1;
  if (count > kMaxSegments) return Combo(id, english, count, current, why, tips, "Custom");
  std::string shown[kMaxSegments];  // the local text owns the pointers labels[] hands out
  const char* labels[kMaxSegments] = {};
  for (int i = 0; i < count; ++i) {
    shown[i] = gui::Display(english[i]);
    labels[i] = shown[i].c_str();
  }
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
    ImGui::SetNextItemWidth(item_width);
    return Combo(id, english, count, current, why, tips, "Custom");
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
  bool held[kMaxSegments] = {};
  for (int i = 0; i < count; ++i) {
    if (i != 0) {
      // The track was measured with kGap between the segments, so the gap drawn between them is kGap: a
      // SameLine() with the style's own spacing drifts the row past the track's right edge by the last segment.
      gui::SameLine(0.f, kGap);
    }
    gui::PushID(i);
    gui::BeginDisabled(!reason(i).empty());
    const bool pressed = ImGui::InvisibleButton("##segment", ImVec2(width, frame_h), ImGuiButtonFlags_EnableNav);
    hot[i] = ImGui::IsItemHovered();
    held[i] = ImGui::IsItemActive();
    gui::EndDisabled();
    seg_min[i] = ImGui::GetItemRectMin();
    seg_max[i] = ImGui::GetItemRectMax();
    gui::PopID();
    if (pressed) {
      clicked = i;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
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
  for (int i = 0; i < count; ++i) {
    const bool greyed = !reason(i).empty();
    const float inset = kGap * 0.5f;
    const ImVec2 lo(seg_min[i].x + inset, seg_min[i].y + inset);
    const ImVec2 hi(seg_max[i].x - inset, seg_max[i].y - inset);
    if (i == current) {
      const auto& fill = greyed ? tokens::kSurfaceHover : held[i] ? tokens::kSelectedActive
                                        : hot[i] ? tokens::kSelectedHover : tokens::kSelected;
      draw->AddRectFilled(lo, hi, ToU32(fill), rounding - inset);
    } else if (!greyed && (held[i] || hot[i])) {
      draw->AddRectFilled(lo, hi, ToU32(held[i] ? tokens::kSurfaceActive : tokens::kSurfaceHover), rounding - inset);
    }
    const ImVec2 size = ImGui::CalcTextSize(labels[i]);
    const ImVec4 color = greyed ? tokens::Fade(tokens::kTextPrimary, ImGui::GetStyle().DisabledAlpha)
                                : (i == current ? tokens::kAccent : tokens::kTextPrimary);
    draw->AddText(ImVec2((seg_min[i].x + seg_max[i].x - size.x) * 0.5f, (seg_min[i].y + seg_max[i].y - size.y) * 0.5f),
                  ToU32(color), labels[i]);
  }
  gui::PopID();
  return clicked;
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
  bool hovered = false;
};
inline KeyCaptureHit KeyCaptureButton(const char* label, bool capturing) {
  const char* text = capturing ? "Press a key (right-click clears)" : label;
  const float frame = ImGui::GetStyle().FramePadding.x * 2.f;
  if (gui::CalcTextSize(text).x + frame > gui::GetContentRegionAvail().x) {  // never half a button: a line of its own
    gui::NewLine();
  }
  const bool pressed = gui::Button(text);
  KeyCaptureHit hit;
  hit.clicked = pressed;
  hit.right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  hit.hovered = ImGui::IsItemHovered();
  return hit;
}

// A dialog that asks before a setting takes effect: the words, then a line of buttons that cancel and confirm.
// `open` is the caller's flag, cleared on either answer and on Escape; `confirm_label` is the button that says
// yes. Returns true on the frame it was confirmed -- the caller applies the change then, never before. The
// caller opens it by name: OpenDialog(id). `width` is the dialog's width in panel units, 0 for the text's own.
inline bool ConfirmDialog(const char* id, const char* body, const char* cancel_label, const char* confirm_label, bool* open,
                          float width = 0.f) {
  if (open == nullptr || !*open) {
    return false;
  }
  // The caller's flag is the request, and it stays set while the dialog is up: opening it only once it is not
  // already open, so a frame that redraws it does not push it back onto the stack.
  if (width > 0.f) {
    const float available = gui::GetIO().DisplaySize.x - 2.f * SectionGap();
    gui::SetNextWindowSize(ImVec2((std::max)(1.f, (std::min)(Units(width), available)), 0.f));
  }
  if (!gui::IsPopupOpen(id)) {
    // Centered in the game's window, not in the panel (2026-11-06): a question about a setting reads as belonging to
    // the game, and a panel that sits at a screen edge put the dialog half off it. The overlay draws the game's own
    // presented image, so the IO's DisplaySize is that window, its origin (0, 0).
    const ImVec2 center(0.5f * gui::GetIO().DisplaySize.x, 0.5f * gui::GetIO().DisplaySize.y);
    gui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    gui::OpenPopup(id);
  }
  StyleScope popup;
  popup.Color(ImGuiCol_PopupBg, tokens::kBgPopup)
      .Color(ImGuiCol_TitleBg, tokens::kBgPopup)
      .Color(ImGuiCol_TitleBgActive, tokens::kBgPopup);
  bool shown = true;
  bool confirmed = false;
  if (!gui::BeginPopupModal(id, &shown, ImGuiWindowFlags_AlwaysAutoResize)) {
    *open = false;  // dismissed from outside (Escape, a click away) or never shown: the question is over
    return false;
  }
  gui::TextWrapped("%s", body);
  NextGap(BlockGap());
  const float cancel_width = gui::CalcTextSize(cancel_label).x + 2.f * gui::GetStyle().FramePadding.x;
  const float accept_width = gui::CalcTextSize(confirm_label).x + 2.f * gui::GetStyle().FramePadding.x;
  const float remaining = gui::GetContentRegionAvail().x - cancel_width - accept_width - RowGap();
  if (remaining >= 0.f) gui::SetCursorPosX(gui::GetCursorPosX() + remaining);
  bool cancel = false;
  {
    StyleScope quiet;
    quiet.FlatButton();
    cancel = gui::Button(cancel_label);
  }
  gui::SetItemDefaultFocus();
  if (remaining >= 0.f) gui::SameLine();
  bool accept = false;
  {
    StyleScope accent;
    accent.AccentButton();
    accept = gui::Button(confirm_label);
  }
  if (cancel || accept || gui::IsKeyPressed(ImGuiKey_Escape)) {
    *open = false;
    gui::CloseCurrentPopup();  // answered: the dialog goes away on this frame, not the next
  }
  confirmed = accept;
  gui::EndPopup();
  if (!shown) {
    *open = false;
  }
  return confirmed;
}

}  // namespace uplift::ui::widgets
