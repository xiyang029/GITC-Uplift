#pragma once

// The panel's style: the design tokens every control draws with (ui/overlay_widgets.hpp), and the scope that
// lends them to ImGui's global style for exactly one panel frame.
//
//   - `tokens` and `icons` are the shared vocabulary for built-in and hand-drawn controls.
//   - PanelStyle copies the live style in, applies those tokens, and copies the live style back on the way out.
//
// ImGui's style is one global struct, and ReShade draws its own windows from it. Nothing here may outlive the
// panel's frame: ReShade re-reads its style from ReShade.ini whenever its settings are touched, and a style
// left applied by the panel was why ReShade's own UI came up in Uplift's colours and metrics.
//
// Nothing here touches ReShade's font atlas (an add-on must not load a font of its own) or ReShade's config,
// and nothing calls ImGuiStyle::ScaleAllSizes (IMGUI_API, and this add-on links no ImGui) or
// ImGui::SetWindowFontScale (obsolete, and global): sizes follow the window's already-scaled body font.
//
// Include after <imgui.h> and reshade.hpp (which ui/localized_imgui.hpp pulls in), as the rest of the panel does.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ui/localized_imgui.hpp"

namespace uplift::ui::style {

// Shared control colours and spacing.
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

// Near-black panel, neutral controls and one accent.
constexpr ImVec4 kBgWindow = Hex(0x090B10, 0.93f);
constexpr ImVec4 kBgControl = Hex(0x1F2024);
constexpr ImVec4 kBgPopup = Hex(0x2A2B31);
constexpr ImVec4 kBgControlHover = Mix(kBgControl, kBgPopup, 0.70f);
constexpr ImVec4 kBgControlActive = Mix(kBgPopup, Hex(0xFFFFFF), 0.10f);
constexpr ImVec4 kTextPrimary = Hex(0xF7F8F8);
constexpr ImVec4 kTextSecondary = Hex(0xB2B7BB);
constexpr ImVec4 kTextTertiary = Hex(0x8A8F98);  // hints, captions, the 11 px footnotes
constexpr ImVec4 kTextFaint = Hex(0x62666D);     // the quietest line in a card
constexpr ImVec4 kBad = Hex(0xF18B89);  // semantic safety warning only
// Controls use a brighter accent; primary action fills are darker for white-text contrast.
constexpr ImVec4 kAccent = Hex(0x828FFF);
constexpr ImVec4 kPrimary = Hex(0x5E6AD2);
constexpr ImVec4 kPrimaryHover = Hex(0x6C78E8);
constexpr ImVec4 kPrimaryActive = Hex(0x4C55B8);
constexpr ImVec4 kOnPrimary = Hex(0xFFFFFF);
constexpr ImVec4 kAccentHalo = Fade(kAccent, 0.32f);
constexpr ImVec4 kAccentStrong = Fade(kAccent, 0.70f);
constexpr ImVec4 kSelected = Mix(kBgControl, kAccent, 0.19f);
constexpr ImVec4 kSelectedHover = Mix(kBgControlHover, kAccent, 0.24f);
constexpr ImVec4 kSelectedActive = Mix(kBgControlActive, kAccent, 0.30f);
constexpr ImVec4 kSurfaceHover = Fade(kTextPrimary, 0.08f);
constexpr ImVec4 kSurfaceActive = Fade(kTextPrimary, 0.16f);
// White is reserved for the navigation. Body rows use tonal fills to keep the text and controls readable.
constexpr ImVec4 kPlateInk = Hex(0x0A0A0B);

constexpr float kBaseFont = 13.f;
constexpr float kHeadingScale = 1.2f;   // card and group headings: the reference panel's bold captions
constexpr float kRowLabelScale = 1.03f;  // row labels, a touch larger than the body so they read as bold

// Row geometry measured in the body font; the width limit is relative to the current column.
constexpr float kLabelEm = 10.5f;
constexpr float kLabelShare = 0.42f;
constexpr float kStackEm = 9.f;
constexpr float kTooltipEm = 35.f;

}  // namespace tokens

namespace icons {
// ReShade v6.8 merges ForkAwesome into the UI font. Check coverage for other host atlases.
inline const char* Glyph(ImWchar primary, const char* icon, ImWchar alternate, const char* symbol, const char* fallback) {
  ImFont* const font = gui::GetFont();
  if (font->IsGlyphInFont(primary)) return icon;
  if (font->IsGlyphInFont(alternate)) return symbol;
  return fallback;
}
inline const char* Undo() { return Glyph(0xF0E2, "\xef\x83\xa2", 0x21BA, "\xe2\x86\xba", "<"); }
inline const char* RadioOn() { return Glyph(0xF192, "\xef\x86\x92", 0x25C9, "\xe2\x97\x89", "*"); }
inline const char* RadioOff() { return Glyph(0xF10C, "\xef\x84\x8c", 0x25CB, "\xe2\x97\x8b", "o"); }
inline const char* Previous() { return Glyph(0xF053, "\xef\x81\x93", 0x2039, "\xe2\x80\xb9", "<"); }
inline const char* Next() { return Glyph(0xF054, "\xef\x81\x94", 0x203A, "\xe2\x80\xba", ">"); }
inline const char* Close() { return Glyph(0xF00D, "\xef\x80\x8d", 0x00D7, "\xc3\x97", "x"); }
}  // namespace icons

// Round the scaled 4 px step once so repeated gaps stay integral at fractional font scales.
inline float Scale() { return gui::GetFontSize() / tokens::kBaseFont; }
inline float Px(float value) { return value * Scale(); }
inline float GridStep(float scale) { return (std::max)(1.f, std::round(4.f * scale)); }
inline float Units(float count) { return GridStep(Scale()) * count; }
inline float RowGap() { return Units(1); }
inline float BlockGap() { return Units(2); }
inline float CardInset() { return Units(3); }
inline float CardGap() { return Units(4); }
inline float SnapDown(float size) { const float step = Units(1); return std::floor(size / step) * step; }
inline float SnapUp(float size) { const float step = Units(1); return std::ceil(size / step) * step; }
// Radii, one step rounder than Linear's 5-7 / 8 bound, towards the reference panel: 6 for a control, 10 for a
// card, and a capsule wherever a control is a pill (a tab, a footer action).
inline float ControlRadius() { return SnapUp(Units(1.5f)); }
inline float CardRadius() { return SnapUp(Units(2.5f)); }
inline float CapsuleRadius() { return 999.f; }

// A settings row is 2 em tall whatever the player's font size: 38 px at ReShade's default 19 px face, which
// is the reference panel's row, and it grows with the Ctrl + wheel instead of clipping.
inline float RowHeight() { return SnapUp(gui::GetFontSize() * 2.f); }

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
  StyleScope& AccentButton() { return Button(tokens::kPrimary, tokens::kPrimaryHover, tokens::kPrimaryActive, tokens::kOnPrimary); }
  // A button with no surface of its own: the reset arrows and other quiet icon actions.
  StyleScope& FlatButton(const ImVec4& text = tokens::kTextSecondary) {
    return Button(tokens::kTransparent, tokens::kSurfaceHover, tokens::kSurfaceActive, text);
  }
 private:
  // A button's four colours: its surface at rest, hovered and pressed, and the words on it.
  StyleScope& Button(const ImVec4& base, const ImVec4& hovered, const ImVec4& active, const ImVec4& text) {
    return Color(ImGuiCol_Button, base)
        .Color(ImGuiCol_ButtonHovered, hovered)
        .Color(ImGuiCol_ButtonActive, active)
        .Color(ImGuiCol_Text, text);
  }
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
    {ImGuiCol_PopupBg, tokens::kBgPopup},
    {ImGuiCol_Border, tokens::kTransparent},
    {ImGuiCol_BorderShadow, tokens::kTransparent},

    // Text fields and native slider fallbacks.
    {ImGuiCol_FrameBg, tokens::kBgControl},
    {ImGuiCol_FrameBgHovered, tokens::kBgControlHover},
    {ImGuiCol_FrameBgActive, tokens::kBgControlActive},

    {ImGuiCol_Button, tokens::kBgControl},
    {ImGuiCol_ButtonHovered, tokens::kBgControlHover},
    {ImGuiCol_ButtonActive, tokens::kBgControlActive},

    // A thin, nearly invisible scrollbar: the panel is the content, the bar only a hint of it.
    {ImGuiCol_ScrollbarBg, tokens::Fade(tokens::kTextPrimary, 0.03f)},
    {ImGuiCol_ScrollbarGrab, tokens::Fade(tokens::kTextPrimary, 0.16f)},
    {ImGuiCol_ScrollbarGrabHovered, tokens::Fade(tokens::kTextPrimary, 0.26f)},
    {ImGuiCol_ScrollbarGrabActive, tokens::Fade(tokens::kTextPrimary, 0.36f)},

    // Native sliders remain available for narrow rows and for text input.
    {ImGuiCol_SliderGrab, tokens::kAccent},
    {ImGuiCol_SliderGrabActive, tokens::kPrimary},
    {ImGuiCol_InputTextCursor, tokens::kAccent},
    {ImGuiCol_NavCursor, tokens::kAccentStrong},
    {ImGuiCol_TextSelectedBg, tokens::kAccentHalo},
    {ImGuiCol_TableRowBg, tokens::kTransparent},
    {ImGuiCol_TableRowBgAlt, tokens::Fade(tokens::kTextPrimary, 0.02f)},

    {ImGuiCol_NavWindowingHighlight, tokens::Fade(tokens::kTextPrimary, 0.70f)},
    {ImGuiCol_NavWindowingDimBg, tokens::Fade(tokens::Hex(0x000000), 0.35f)},
    {ImGuiCol_ModalWindowDimBg, tokens::Fade(tokens::Hex(0x000000), 0.55f)},
};

// Override only the panel's controls; preserve the host font and restore the base after drawing.
inline void ApplyPanelStyle() {
  ImGuiStyle& style = ImGui::GetStyle();
  const float scale = Scale();
  style.WindowBorderSize = 0.f;
  style.ChildBorderSize = 0.f;
  style.FrameBorderSize = 0.f;
  style.PopupBorderSize = 0.f;

  const float step = GridStep(scale);
  style.WindowRounding = CardRadius();
  style.PopupRounding = CardRadius();
  style.ScrollbarRounding = ControlRadius();
  style.WindowPadding = ImVec2(3.f * step, 3.f * step);
  style.FramePadding = ImVec2(2.f * step, step);
  style.FrameRounding = ControlRadius();
  style.GrabRounding = ControlRadius();
  style.ItemSpacing = ImVec2(step, step);
  style.ItemInnerSpacing = ImVec2(step, step);

  style.ScrollbarSize = step;
  style.ScrollbarPadding = 0.f;
  style.WindowMinSize = ImVec2(style.WindowMinSize.x * scale, style.WindowMinSize.y * scale);

  style.DisabledAlpha = 0.55f;
  style.ButtonTextAlign = ImVec2(0.5f, 0.5f);

  style.AntiAliasedLines = true;
  style.AntiAliasedLinesUseTex = true;
  style.AntiAliasedFill = true;
  style.CurveTessellationTol = 1.25f;
  style.CircleTessellationMaxError = 0.30f;

  for (const SlotColor& entry : kPalette) {
    style.Colors[entry.slot] = entry.color;
  }
}

}  // namespace detail

// The panel's style, held for one panel frame. ImGui's style is one global struct, so a frame that returns
// through the middle of a tab still has to put ReShade's copy back: the destructor does it.
//
// Copy-initialised on purpose: ImGuiStyle's default constructor lives in imgui.cpp (the add-on links no ImGui),
// while its copy constructor is implicit, so `ImGuiStyle saved;` would not link.
class PanelStyle {
 public:
  PanelStyle() : saved_(ImGui::GetStyle()) { detail::ApplyPanelStyle(); }
  PanelStyle(const PanelStyle&) = delete;
  PanelStyle& operator=(const PanelStyle&) = delete;
  ~PanelStyle() { ImGui::GetStyle() = saved_; }

 private:
  ImGuiStyle saved_;
};

}  // namespace uplift::ui::style
