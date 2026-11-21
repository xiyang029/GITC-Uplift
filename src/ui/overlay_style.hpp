#pragma once

// The panel's style: the design tokens every control draws with (ui/overlay_widgets.hpp), and the one thing
// that writes them into ImGui's global style.
//
//   - `tokens` and `icons` are the shared vocabulary for built-in and hand-drawn controls.
//   - Restore() / RefreshForDpi() put those same tokens into ImGui's global style, which is what ReShade's own
//     UI and every other add-on draw with.
//
// ImGui's style is one global struct, so what RefreshForDpi sets is also what ReShade draws with: it goes on at
// the panel's first frame (AddonInit runs long before ReShade has an ImGui context and reaching for the style
// there crashed the game), and it comes off again in AddonUninit (Restore).
//
// Nothing here touches ReShade's font atlas (an add-on must not load a font of its own) or ReShade's config,
// and nothing calls ImGuiStyle::ScaleAllSizes (IMGUI_API, and this add-on links no ImGui) or
// ImGui::SetWindowFontScale (obsolete, and global): sizes follow the window's already-scaled body font.
//
// Include after <imgui.h> and reshade.hpp (which ui/localized_imgui.hpp pulls in), as the rest of the panel does.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ui/localized_imgui.hpp"

namespace uplift::ui::style {

// Four neutral elevations. Hover and press change the local surface; they do not introduce new layers.
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

constexpr ImVec4 kBgWindow = Hex(0x151617, 0.98f);
constexpr ImVec4 kBgCard = Hex(0x222426);
constexpr ImVec4 kBgControl = Hex(0x303336);
constexpr ImVec4 kBgPopup = Hex(0x404448);
constexpr ImVec4 kCardShadow = Hex(0x000000, 0.24f);
constexpr ImVec4 kBgControlHover = Mix(kBgControl, kBgPopup, 0.70f);
constexpr ImVec4 kBgControlActive = Mix(kBgPopup, Hex(0xFFFFFF), 0.10f);
constexpr ImVec4 kTextPrimary = Hex(0xECEEF0);
constexpr ImVec4 kTextSecondary = Hex(0xB2B7BB);
constexpr ImVec4 kBad = Hex(0xF18B89);  // semantic safety warning only
constexpr ImVec4 kAccent = Hex(0x74C8BF);
constexpr ImVec4 kAccentHover = Mix(kAccent, kTextPrimary, 0.22f);
constexpr ImVec4 kOnAccent = Hex(0x101718);
constexpr ImVec4 kAccentActive = Mix(kAccent, kOnAccent, 0.18f);
constexpr ImVec4 kAccentWash = Fade(kAccent, 0.16f);
constexpr ImVec4 kAccentHalo = Fade(kAccent, 0.32f);
constexpr ImVec4 kAccentStrong = Fade(kAccent, 0.70f);
constexpr ImVec4 kSelected = Mix(kBgControl, kAccent, 0.19f);
constexpr ImVec4 kSelectedHover = Mix(kBgControlHover, kAccent, 0.24f);
constexpr ImVec4 kSelectedActive = Mix(kBgControlActive, kAccent, 0.30f);
constexpr ImVec4 kSurfaceHover = Fade(kTextPrimary, 0.08f);
constexpr ImVec4 kSurfaceActive = Fade(kTextPrimary, 0.16f);
constexpr ImVec4 kStroke = Fade(kTextSecondary, 0.40f);  // functional connectors, not outlines

constexpr float kBaseFont = 13.f;
constexpr float kHeadingScale = 1.15f;

// Row geometry measured in the body font; the width limit is relative to the current column.
constexpr float kLabelEm = 10.5f;
constexpr float kLabelShare = 0.42f;
constexpr float kStackEm = 9.f;
constexpr float kTooltipEm = 35.f;

}  // namespace tokens

// ForkAwesome glyphs, as ReShade merges them (external/reshade/res/fonts/forkawesome.h: the subset ReShade
// ships, which is all the atlas holds). Add the ones a control needs; the atlas has no other glyph.
namespace icons {
constexpr const char* kUndo = "\xef\x83\xa2";       // U+F0E2 undo
}  // namespace icons

// Round the scaled 4 px step once so repeated gaps stay integral at fractional font scales.
inline float Scale() { return gui::GetFontSize() / tokens::kBaseFont; }
inline float Px(float value) { return value * Scale(); }
inline float GridStep(float scale) { return (std::max)(1.f, std::round(4.f * scale)); }
inline float Units(float count) { return GridStep(Scale()) * count; }
inline float LabelGap() { return Units(1); }
inline float RowGap() { return Units(1); }
inline float BlockGap() { return Units(2); }
inline float CardInset() { return Units(3); }
inline float CardGap() { return Units(4); }
inline float SnapDown(float size) { const float step = Units(1); return std::floor(size / step) * step; }
inline float SnapUp(float size) { const float step = Units(1); return std::ceil(size / step) * step; }
inline float ControlRadius() { return Units(1); }
inline float CardRadius() { return Units(2); }

// The panel's two-column reading is one number, and it is the stage diagram's: a section shows its two groups
// side by side only where the diagram above draws its six nodes on one line. Below that the diagram folds onto
// two rows of three, and the groups stack one under the other, because a column that narrow leaves a row
// neither room for its label beside its control nor a diagram above it that explains what the columns are.
// The panel opens at Units(112), so both states are reachable by dragging it.
constexpr float kStageColumnsEm = 52.f;
inline bool FitsStageColumns(float width) { return width >= SnapUp(gui::GetFontSize() * kStageColumnsEm); }

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
        .Color(ImGuiCol_ButtonHovered, tokens::kSurfaceHover)
        .Color(ImGuiCol_ButtonActive, tokens::kSurfaceActive)
        .Color(ImGuiCol_Text, tokens::kTextSecondary);
  }
  StyleScope& SelectedButton() {
    return Color(ImGuiCol_Button, tokens::kSelected)
        .Color(ImGuiCol_ButtonHovered, tokens::kSelectedHover)
        .Color(ImGuiCol_ButtonActive, tokens::kSelectedActive)
        .Color(ImGuiCol_Text, tokens::kAccent);
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
    {ImGuiCol_TextLink, tokens::kTextSecondary},
    {ImGuiCol_WindowBg, tokens::kBgWindow},
    {ImGuiCol_ChildBg, tokens::kBgCard},
    {ImGuiCol_PopupBg, tokens::kBgPopup},
    {ImGuiCol_Border, tokens::kTransparent},
    {ImGuiCol_BorderShadow, tokens::kTransparent},

    // Inputs, drags, combos, checkboxes and plots stand on a card, so they take the control tone: the card
    // is what they sit in, and at the well's own fill a text box or a combo read as a hole in the panel.
    {ImGuiCol_FrameBg, tokens::kBgControl},
    {ImGuiCol_FrameBgHovered, tokens::kBgControlHover},
    {ImGuiCol_FrameBgActive, tokens::kBgControlActive},

    {ImGuiCol_Button, tokens::kBgControl},
    {ImGuiCol_ButtonHovered, tokens::kBgControlHover},
    {ImGuiCol_ButtonActive, tokens::kBgControlActive},

    // A selected menu entry uses a translucent plate that also reads on the popup surface.
    {ImGuiCol_Header, tokens::kAccentWash},
    {ImGuiCol_HeaderHovered, tokens::kSurfaceHover},
    {ImGuiCol_HeaderActive, tokens::kSurfaceActive},

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
    {ImGuiCol_NavCursor, tokens::kAccentStrong},
    {ImGuiCol_TextSelectedBg, tokens::kAccentHalo},
    {ImGuiCol_UnsavedMarker, tokens::kTextSecondary},

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

inline bool& Applied() {
  static bool applied = false;
  return applied;
}

inline void ApplyBorders() {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowBorderSize = 0.f;
  style.ChildBorderSize = 0.f;
  style.FrameBorderSize = 0.f;
  style.PopupBorderSize = 0.f;
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

  ApplyBorders();

  const float step = GridStep(scale);
  style.WindowRounding = 3.f * step;
  style.PopupRounding = style.WindowRounding;
  style.ChildRounding = 2.f * step;
  style.ScrollbarRounding = step;
  style.WindowPadding = ImVec2(3.f * step, 3.f * step);
  style.FramePadding = ImVec2(2.f * step, step);
  style.FrameRounding = step;
  style.GrabRounding = style.FrameRounding;
  style.TabRounding = style.FrameRounding;
  style.DragDropTargetRounding = style.FrameRounding;
  style.ItemSpacing = ImVec2(step, step);
  style.ItemInnerSpacing = ImVec2(step, step);
  style.CellPadding = ImVec2(2.f * step, step);
  style.SeparatorTextPadding = ImVec2(3.f * step, step);
  style.IndentSpacing = 4.f * step;

  style.ScrollbarSize = 2.f * step;
  style.ScrollbarPadding = 0.f;
  style.GrabMinSize = 2.f * step;
  style.DragDropTargetBorderSize = 2.f * scale;
  style.DragDropTargetPadding = step;
  style.TabBarOverlineSize = 2.f * scale;
  style.TreeLinesSize = 1.f * scale;
  style.TreeLinesRounding = step;
  style.WindowMinSize = ImVec2(style.WindowMinSize.x * scale, style.WindowMinSize.y * scale);

  style.DisabledAlpha = 0.55f;
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
  Applied() = true;
}

inline void Restore() {
  if (!Applied()) return;
  ImGui::GetStyle() = OriginalStyle();
  Applied() = false;
}

inline void RefreshForDpi() {
  // Every frame, not only when the scale changes: ReShade re-reads its own style out of ReShade.ini
  // (runtime::load_config -> load_custom_style) whenever its settings are touched, which put ReShade's
  // colours back over the panel's. Applying is one struct copy and the palette table.
  //
  // GetFontSize already includes the host's font/DPI changes. Do not multiply by monitor DPI again.
  ApplyAt(Scale());
}

}  // namespace detail

// Puts ReShade's own style back, in full. Call from AddonUninit; safe when the panel was never drawn.
using detail::Restore;

// Applies the style over ReShade's own and re-applies the size fields for the current window's DPI scale. Call
// once per frame, first thing in the panel.
using detail::RefreshForDpi;

}  // namespace uplift::ui::style
