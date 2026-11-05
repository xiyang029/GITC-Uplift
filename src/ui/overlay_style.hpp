#pragma once

// The panel's style: the design tokens every control draws with (ui/overlay_widgets.hpp), and the one thing
// that writes them into ImGui's global style.
//
//   - `tokens`, `icons` and the font sizes are the values every panel control draws with; they are ported from
//     FoveatedDLSS5's src/addons/dlss5/ui/theme.hpp, so the owner's two add-ons share one look, and they are
//     the only place a colour, a radius or a size is written down.
//   - Restore() / RefreshForDpi() put those same tokens into ImGui's global style, which is what ReShade's own
//     UI and every other add-on draw with.
//
// ImGui's style is one global struct, so what RefreshForDpi sets is also what ReShade draws with: it goes on at
// the panel's first frame (AddonInit runs long before ReShade has an ImGui context and reaching for the style
// there crashed the game), and it comes off again in AddonUninit (Restore).
//
// Nothing here touches ReShade's font atlas (an add-on must not load a font of its own) or ReShade's config,
// and nothing calls ImGuiStyle::ScaleAllSizes (IMGUI_API, and this add-on links no ImGui) or
// ImGui::SetWindowFontScale (obsolete, and global): the size fields are scaled by hand, per window DPI.
//
// Include after <imgui.h> and reshade.hpp (which ui/localized_imgui.hpp pulls in), as the rest of the panel does.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ui/localized_imgui.hpp"

namespace uplift::ui::style {

// The panel's palette, its corner radii and the row geometry. A short ramp: the panel, the cards and wells
// sunk into it, and the controls standing on it. No shadows to spend, so elevation is one small step of
// lightness and a hairline only where a step alone cannot be seen.
namespace tokens {

constexpr ImVec4 Hex(std::uint32_t rgb, float alpha = 1.f) {
  return ImVec4(static_cast<float>((rgb >> 16) & 0xff) / 255.f,
                static_cast<float>((rgb >> 8) & 0xff) / 255.f,
                static_cast<float>(rgb & 0xff) / 255.f, alpha);
}

// Two colours blended, `t` weighting `to`: the pressed and active surfaces are the accent showing through
// a control, so there is one accent and no hand-picked second colour to drift away from it.
constexpr ImVec4 Mix(const ImVec4& from, const ImVec4& to, float t) {
  return ImVec4(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t,
                from.w + (to.w - from.w) * t);
}

constexpr ImVec4 Fade(const ImVec4& color, float alpha) {
  return ImVec4(color.x, color.y, color.z, alpha);
}

constexpr ImVec4 kTransparent = ImVec4(0.f, 0.f, 0.f, 0.f);

constexpr ImVec4 kBgWindow = Hex(0x14161A, 0.96f);
constexpr ImVec4 kBgCard = Hex(0x1B1E24);        // recessed: a status card, the well an input sits in
constexpr ImVec4 kBgControl = Hex(0x24282F);     // a button at rest, a segmented control's track
constexpr ImVec4 kBgControlHover = Hex(0x2C3139);
constexpr ImVec4 kStroke = Hex(0x252930);
constexpr ImVec4 kTextPrimary = Hex(0xE8EAED);
constexpr ImVec4 kTextSecondary = Hex(0x9AA0A8);
constexpr ImVec4 kWarn = Hex(0xF5A623);          // ReShade's unsaved-changes marker
constexpr ImVec4 kBad = Hex(0xEB5757);           // a latched safety lock's words, the one red on the panel
constexpr ImVec4 kAccent = Hex(0x4CA6A0);        // the one saturated colour in the panel
constexpr ImVec4 kAccentHover = Hex(0x5BB8B2);
constexpr ImVec4 kAccentActive = Hex(0x3E8C87);

// The accent tints both layers share: hover wells, press washes, focus rings, selection.
constexpr ImVec4 kAccentWash = Hex(0x4CA6A0, 0.14f);
constexpr ImVec4 kAccentHalo = Hex(0x4CA6A0, 0.28f);
constexpr ImVec4 kAccentStrong = Hex(0x4CA6A0, 0.45f);

// An on switch: the accent held back a step from a primary action's fill, so a panel with several switches
// on does not read as a row of saturated blocks.
constexpr ImVec4 kAccentQuiet = Mix(kAccent, kBgWindow, 0.18f);
constexpr ImVec4 kAccentQuietHover = Mix(kAccentHover, kBgWindow, 0.12f);
constexpr ImVec4 kHairline = Hex(0xFFFFFF, 0.06f);   // an inner top edge on raised surfaces
constexpr ImVec4 kSurfaceHover = Hex(0xFFFFFF, 0.05f);  // a flat row's hover, without a second surface
constexpr ImVec4 kOnAccent = Hex(0x101216);          // words on the accent or on kBad

// Radii: the window and its popups, a card or a control, and a small mark (a grab, a knob).
constexpr float kRadiusCard = 10.f;
constexpr float kRadiusControl = 8.f;
constexpr float kRadiusSmall = 5.f;
constexpr float kRowGap = 8.f;
constexpr float kCardPadding = 12.f;
constexpr float kTooltipPadX = 8.f;
constexpr float kTooltipPadY = 6.f;

// Row geometry, in units of the font size so it follows the player's ReShade font. The label column is a
// fixed width, never more than kLabelShare of a narrow panel, so every control on the panel starts at the
// same x; a label wider than the column extends into it, its right edge against the control. A panel
// narrower than kStackEm has no room for a label column beside a usable control (ReShade docks it at
// ~450 px), so there each label goes on its own line over a control as wide as the panel. A slider's value
// sits just right of its rail, its right edge on the control column's, instead of under the knob.
//
// kStackEm is measured against the width the row actually has, which inside a two-column group is that
// column. At 24 em no row on the panel ever cleared it -- not even a single-column one at the width the panel
// opens at -- so every row stacked, and a row ending in a switch put that switch alone at the right of the
// line under its label. Nine em is the narrowest column that still holds a label and its control: below that
// the label goes over the control, which is the only thing stacking is for.
constexpr float kLabelEm = 8.5f;
constexpr float kLabelShare = 0.38f;
constexpr float kStackEm = 9.f;
constexpr float kTooltipEm = 35.f;

}  // namespace tokens

// ForkAwesome glyphs, as ReShade merges them (external/reshade/res/fonts/forkawesome.h: the subset ReShade
// ships, which is all the atlas holds). Add the ones a control needs; the atlas has no other glyph.
namespace icons {
constexpr const char* kUndo = "\xef\x83\xa2";       // U+F0E2 undo
}  // namespace icons

// The panel's spacing rhythm, resolved from the live font size, so every gap, inset and control follows
// ReShade's font-size setting instead of a fixed pixel grid. Each step is one of the same rhythm the rows use.
inline float Units(float count) { return gui::GetFontSize() * (4.f / 13.f) * count; }
// A size token as the live window's pixels. RefreshForDpi scales the fields it writes into ImGui's global style;
// the ones the panel pushes itself (a card's inset, a tooltip's, a section header's) never go through it, so they
// take the same scale here -- without this a 150% panel drew those at their 100% size.
inline float Px(float value) {
  const float dpi = gui::GetWindowDpiScale();
  return value * ((dpi > 0.01f && dpi < 8.f) ? dpi : 1.f);
}
inline float LabelGap() { return Units(0.75f); }
inline float RowGap() { return Units(1.25f); }
inline float BlockGap() { return Units(3); }
inline float SectionGap() { return Units(4.5f); }

// The panel's two-column reading is one number, and it is the stage diagram's: a section shows its two groups
// side by side only where the diagram above draws its six nodes on one line. Below that the diagram folds onto
// two rows of three, and the groups stack one under the other, because a column that narrow leaves a row
// neither room for its label beside its control nor a diagram above it that explains what the columns are.
// The panel opens at Units(112), so both states are reachable by dragging it.
constexpr float kStageColumnsEm = 52.f;
inline bool FitsStageColumns(float width) { return width >= gui::GetFontSize() * kStageColumnsEm; }

inline ImU32 ToU32(const ImVec4& color) { return ImGui::GetColorU32(color); }

// Pushes and pops in one object, so a return from the middle of a panel cannot leave a colour or a variable
// on ReShade's stacks. The named pairs are the two control roles the panel draws: its primary action, and a
// quiet icon button.
class StyleScope {
 public:
  StyleScope() = default;
  StyleScope(const StyleScope&) = delete;
  StyleScope& operator=(const StyleScope&) = delete;
  ~StyleScope() {
    if (vars_ != 0) ImGui::PopStyleVar(vars_);
    if (colors_ != 0) ImGui::PopStyleColor(colors_);
  }
  StyleScope& Color(ImGuiCol index, const ImVec4& color) {
    ImGui::PushStyleColor(index, color);
    ++colors_;
    return *this;
  }
  StyleScope& Var(ImGuiStyleVar index, float value) {
    ImGui::PushStyleVar(index, value);
    ++vars_;
    return *this;
  }
  StyleScope& Var(ImGuiStyleVar index, const ImVec2& value) {
    ImGui::PushStyleVar(index, value);
    ++vars_;
    return *this;
  }
  // The accent as a control's fill and the window's colour as its words: the panel's primary action.
  StyleScope& AccentButton() {
    return Color(ImGuiCol_Button, tokens::kAccent)
        .Color(ImGuiCol_ButtonHovered, tokens::kAccentHover)
        .Color(ImGuiCol_ButtonActive, tokens::kAccentActive)
        .Color(ImGuiCol_Text, tokens::kOnAccent);
  }
  // A button with no surface of its own: the reset arrows and other quiet icon actions.
  StyleScope& FlatButton() {
    return Color(ImGuiCol_Button, tokens::kTransparent)
        .Color(ImGuiCol_ButtonHovered, tokens::kAccentWash)
        .Color(ImGuiCol_ButtonActive, tokens::kAccentHalo)
        .Color(ImGuiCol_Text, tokens::kTextSecondary);
  }
 private:
  int colors_ = 0;
  int vars_ = 0;
};

// The style half's own implementation, in one place and inline: the add-on links no ImGui source, so there is
// no translation unit to put it in.
namespace detail {

// Every colour the panel puts into ImGui's style, as one table: a single list to read, and no call site
// left to drift from the tokens. The tinted states are composed with tokens::Mix, so the accent is the
// only saturated hue anywhere in the panel.
struct SlotColor {
  ImGuiCol slot;
  ImVec4 color;
};

constexpr SlotColor kPalette[] = {
    {ImGuiCol_Text, tokens::kTextPrimary},
    {ImGuiCol_TextDisabled, tokens::kTextSecondary},
    {ImGuiCol_TextLink, tokens::kAccent},
    {ImGuiCol_WindowBg, tokens::kBgWindow},
    {ImGuiCol_ChildBg, tokens::kBgCard},
    // A popup (a combo's list, a tooltip) opens over a card, so it takes the control tone: at the card's own
    // tone it would sit flush against what it covers and only its hairline would say it is there.
    {ImGuiCol_PopupBg, tokens::kBgControl},
    // A frame's edge is a hairline, not a line: on the panel's dark ramp a full-strength stroke reads as a
    // chrome outline around every combo and slider. Cards keep kHairline of their own.
    {ImGuiCol_Border, tokens::Fade(tokens::kTextPrimary, 0.10f)},
    {ImGuiCol_BorderShadow, tokens::kTransparent},

    // Inputs, drags, combos, checkboxes and plots stand on a card, so they take the control tone: the card
    // is what they sit in, and at the well's own fill a text box or a combo read as a hole in the panel.
    {ImGuiCol_FrameBg, tokens::kBgControl},
    {ImGuiCol_FrameBgHovered, tokens::kBgControlHover},
    // Pressed inputs take one step up, not a tint: the accent belongs to the controls that mean "on".
    {ImGuiCol_FrameBgActive, tokens::kBgControlHover},

    {ImGuiCol_Button, tokens::kBgControl},
    {ImGuiCol_ButtonHovered, tokens::kBgControlHover},
    {ImGuiCol_ButtonActive, tokens::kAccentActive},

    // Rows, tree nodes and section headers: one step up from the panel, the accent only while held.
    {ImGuiCol_Header, tokens::kBgCard},
    {ImGuiCol_HeaderHovered, tokens::kSurfaceHover},
    {ImGuiCol_HeaderActive, tokens::kAccentWash},

    // Title bar and tabs keep the panel's own layer when idle, one step up when active; the selected tab
    // also carries the accent overline.
    {ImGuiCol_TitleBg, tokens::kBgWindow},
    {ImGuiCol_TitleBgActive, tokens::Mix(tokens::kBgWindow, tokens::kBgControlHover, 0.35f)},
    {ImGuiCol_TitleBgCollapsed, tokens::kBgWindow},
    {ImGuiCol_MenuBarBg, tokens::kBgWindow},
    {ImGuiCol_Tab, tokens::kBgWindow},
    {ImGuiCol_TabHovered, tokens::kSurfaceHover},
    {ImGuiCol_TabSelected, tokens::Mix(tokens::kBgWindow, tokens::kBgControlHover, 0.45f)},
    {ImGuiCol_TabSelectedOverline, tokens::kAccent},
    {ImGuiCol_TabDimmed, tokens::kBgCard},
    {ImGuiCol_TabDimmedSelected, tokens::Mix(tokens::kBgWindow, tokens::kBgControlHover, 0.35f)},
    {ImGuiCol_TabDimmedSelectedOverline, tokens::kAccentStrong},

    // A thin, nearly invisible scrollbar: the panel is the content, the bar only a hint of it.
    {ImGuiCol_ScrollbarBg, tokens::Fade(tokens::kTextPrimary, 0.03f)},
    {ImGuiCol_ScrollbarGrab, tokens::Fade(tokens::kTextPrimary, 0.16f)},
    {ImGuiCol_ScrollbarGrabHovered, tokens::Fade(tokens::kTextPrimary, 0.26f)},
    {ImGuiCol_ScrollbarGrabActive, tokens::Fade(tokens::kTextPrimary, 0.36f)},

    {ImGuiCol_CheckMark, tokens::kAccent},
    {ImGuiCol_SliderGrab, tokens::kAccent},
    {ImGuiCol_SliderGrabActive, tokens::kAccentHover},
    {ImGuiCol_InputTextCursor, tokens::kAccent},
    {ImGuiCol_NavCursor, tokens::kAccentHalo},
    {ImGuiCol_TextSelectedBg, tokens::kAccentHalo},
    {ImGuiCol_UnsavedMarker, tokens::kWarn},

    {ImGuiCol_Separator, tokens::kStroke},
    {ImGuiCol_SeparatorHovered, tokens::kAccentStrong},
    {ImGuiCol_SeparatorActive, tokens::kAccent},
    {ImGuiCol_ResizeGrip, tokens::Fade(tokens::kTextPrimary, 0.12f)},
    {ImGuiCol_ResizeGripHovered, tokens::Fade(tokens::kTextPrimary, 0.24f)},
    {ImGuiCol_ResizeGripActive, tokens::Fade(tokens::kAccent, 0.70f)},

    {ImGuiCol_PlotLines, tokens::kAccent},
    {ImGuiCol_PlotLinesHovered, tokens::kAccent},
    {ImGuiCol_PlotHistogram, tokens::kAccent},
    {ImGuiCol_PlotHistogramHovered, tokens::kAccent},

    {ImGuiCol_TableHeaderBg, tokens::kBgCard},
    {ImGuiCol_TableBorderStrong, tokens::kStroke},
    {ImGuiCol_TableBorderLight, tokens::Fade(tokens::kTextPrimary, 0.035f)},
    {ImGuiCol_TableRowBg, tokens::kTransparent},
    {ImGuiCol_TableRowBgAlt, tokens::Fade(tokens::kTextPrimary, 0.02f)},

    {ImGuiCol_TreeLines, tokens::Fade(tokens::kTextSecondary, 0.30f)},
    {ImGuiCol_DragDropTarget, tokens::kAccent},
    {ImGuiCol_DragDropTargetBg, tokens::kAccentWash},
    {ImGuiCol_NavWindowingHighlight, tokens::Fade(tokens::kTextPrimary, 0.70f)},
    {ImGuiCol_NavWindowingDimBg, tokens::Fade(tokens::Hex(0x000000), 0.35f)},
    {ImGuiCol_ModalWindowDimBg, tokens::Fade(tokens::Hex(0x000000), 0.55f)},
};

// Taken on the panel's first frame, the earliest point with a live ImGui context. Copy-initialised on
// purpose: ImGuiStyle's default constructor is IMGUI_API and the add-on links no ImGui, only ReShade's
// function table, so a plain `ImGuiStyle original;` would not link.
inline ImGuiStyle& OriginalStyle() {
  static ImGuiStyle original = ImGui::GetStyle();
  return original;
}

// The DPI scale the size fields were last applied at; 0 means "ReShade's own style is in place".
inline float& AppliedDpi() {
  static float dpi = 0.f;
  return dpi;
}

// Borders where they carry information and nowhere else: none around the panel or a card, since the ramp
// separates those two, and a hairline around a frame and around a popup -- a recessed fill alone is hard to
// make out on a dark panel, and a popup has neither a neighbour nor a shadow to lift it off what is behind
// it. A blue title bar or tab is the same kind of frame, so those stay flat in the panel's own colours too.
inline void ApplyBorders(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowBorderSize = 0.f;
  style.ChildBorderSize = 0.f;
  // No border on a control: the panel separates layers by fill, and a line around every combo, slider and
  // button is the one thing that made the panel read as chrome. Popups keep one so they lift off the panel.
  style.FrameBorderSize = 0.f;
  style.PopupBorderSize = (std::max)(1.f, scale);
  style.TabBorderSize = 0.f;
  style.TabBarBorderSize = 0.f;
  style.SeparatorTextBorderSize = 0.f;
}

inline void ApplyPalette() {
  ImGuiStyle& style = ImGui::GetStyle();
  for (const SlotColor& entry : kPalette) {
    style.Colors[entry.slot] = entry.color;
  }
}

// Applies Uplift's style over ReShade's own, at `scale`. ReShade's style is the base every time, so a
// field ReShade sets that Uplift does not simply survives.
inline void ApplyAt(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  // The player's font size comes from the live style, never from the snapshot: ReShade's Ctrl + mouse wheel
  // writes Style.FontScaleMain and its Settings tab writes FontSizeBase/FontScaleDpi ("Change font size if
  // user presses the control key and moves the mouse wheel", runtime_gui.cpp). Copying the snapshot over
  // those three every frame put them back to the size of the panel's first frame, so the wheel looked dead.
  const float font_size_base = style.FontSizeBase;
  const float font_scale_main = style.FontScaleMain;
  const float font_scale_dpi = style.FontScaleDpi;
  style = OriginalStyle();
  style.FontSizeBase = font_size_base;
  style.FontScaleMain = font_scale_main;
  style.FontScaleDpi = font_scale_dpi;

  ApplyBorders(scale);

  // A window and the popups above it take the card radius plus a step, a card or a scrollbar inside it the
  // card radius, the controls on it the control radius, and a grab the small one. The spacing rhythm is
  // shared with the panel's own widgets (widgets.hpp), so hand-drawn and ImGui-drawn rows sit on one grid.
  style.WindowRounding = (tokens::kRadiusCard + 2.f) * scale;
  style.PopupRounding = (tokens::kRadiusCard + 2.f) * scale;
  style.ChildRounding = tokens::kRadiusCard * scale;
  style.ScrollbarRounding = tokens::kRadiusCard * scale;
  style.FrameRounding = tokens::kRadiusControl * scale;
  style.GrabRounding = tokens::kRadiusSmall * scale;
  style.TabRounding = tokens::kRadiusControl * scale;
  style.DragDropTargetRounding = tokens::kRadiusControl * scale;

  style.WindowPadding = ImVec2(16.f * scale, 14.f * scale);
  style.FramePadding = ImVec2(9.f * scale, 6.f * scale);
  // Rows sit on the DPI scale, the paragraph gaps inside them on the font (style::Units): ReShade's own Ctrl +
  // wheel font size then moves the air between groups and leaves the row grid alone.
  style.ItemSpacing = ImVec2(tokens::kRowGap * scale, tokens::kRowGap * scale);
  style.ItemInnerSpacing = ImVec2(6.f * scale, 5.f * scale);
  style.CellPadding = ImVec2(8.f * scale, 4.f * scale);
  style.SeparatorTextPadding = ImVec2(14.f * scale, 6.f * scale);
  style.IndentSpacing = 18.f * scale;

  style.ScrollbarSize = 8.f * scale;
  style.ScrollbarPadding = 2.f * scale;  // a pill grab inside a thin track, not a full-width block
  style.GrabMinSize = 10.f * scale;
  style.DragDropTargetBorderSize = 2.f * scale;
  style.DragDropTargetPadding = 2.f * scale;
  style.TabBarOverlineSize = 2.f * scale;
  style.TreeLinesSize = 1.f * scale;
  style.TreeLinesRounding = 3.f * scale;
  style.WindowMinSize = ImVec2(style.WindowMinSize.x * scale, style.WindowMinSize.y * scale);

  style.DisabledAlpha = 0.50f;  // faded rather than greyed, so a disabled row stays readable
  style.WindowTitleAlign = ImVec2(0.f, 0.5f);
  style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
  style.SelectableTextAlign = ImVec2(0.f, 0.5f);
  style.SeparatorTextAlign = ImVec2(0.f, 0.5f);
  style.ColorButtonPosition = ImGuiDir_Right;

  style.AntiAliasedLines = true;
  style.AntiAliasedLinesUseTex = true;
  style.AntiAliasedFill = true;
  style.CurveTessellationTol = 1.25f;
  style.CircleTessellationMaxError = 0.30f;

  ApplyPalette();
  AppliedDpi() = scale;
}

inline void Restore() {
  if (AppliedDpi() == 0.f) return;  // nothing was applied: ReShade's style is already in place
  ImGui::GetStyle() = OriginalStyle();
  AppliedDpi() = 0.f;
}

inline void RefreshForDpi() {
  // Every frame, not only when the scale changes: ReShade re-reads its own style out of ReShade.ini
  // (runtime::load_config -> load_custom_style) whenever its settings are touched, which put ReShade's
  // colours back over the panel's. Applying is one struct copy and the palette table.
  //
  // The window already knows its monitor's scale, so this needs no HWND and no WM_DPICHANGED handling of
  // its own: a window dragged to another monitor re-applies here.
  const float dpi = ImGui::GetWindowDpiScale();
  ApplyAt((dpi > 0.01f && dpi < 8.f) ? dpi : 1.f);
}

}  // namespace detail

// Puts ReShade's own style back, in full. Call from AddonUninit; safe when the panel was never drawn.
using detail::Restore;

// Applies the style over ReShade's own and re-applies the size fields for the current window's DPI scale. Call
// once per frame, first thing in the panel.
using detail::RefreshForDpi;

}  // namespace uplift::ui::style
