#pragma once

// The panel's style, in two halves that share one set of design tokens:
//   - `tokens`, `icons` and the font sizes are the values every panel control draws with (ui/widgets.hpp);
//     they are ported from FoveatedDLSS5's src/addons/dlss5/ui/theme.hpp, so the owner's two add-ons share
//     one look, and they are the only place a colour, a radius or a size is written down;
//   - Restore() / RefreshForDpi() put those same tokens into ImGui's global style, which is what ReShade's
//     own UI and every other add-on draw with.
//
// ImGui's style is one global struct, so what is set there is also what ReShade draws with: it goes on at
// the panel's first frame (RefreshForDpi), because AddonInit runs long before ReShade has an ImGui context
// and reaching for the style there crashed the game, and it comes off again in AddonUninit (Restore).
//
// Nothing here touches ReShade's font atlas (an add-on must not load a font of its own) or ReShade's
// config, and nothing calls ImGuiStyle::ScaleAllSizes (IMGUI_API, and this add-on links no ImGui) or
// ImGui::SetWindowFontScale (obsolete, and global): the size fields are scaled by hand, per window DPI.
//
// Include after <imgui.h> and reshade.hpp (which addon/reshade_api.hpp does), as the rest of the overlay
// does: the tokens are ImVec4s and StyleScope calls the function table ReShade hands the add-on. Both are
// pulled in here so any translation unit can include this header on its own.

#include <cstdint>

#include <imgui.h>
#include <reshade.hpp>

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
constexpr ImVec4 kBad = Hex(0xEB5757);           // a destructive action ("Restore all")
constexpr ImVec4 kBadHover = Hex(0xF26B6B);
constexpr ImVec4 kBadActive = Hex(0xD14B4B);
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
constexpr float kSectionGap = 18.f;
constexpr float kCardPadding = 12.f;
constexpr float kTooltipPadX = 8.f;
constexpr float kTooltipPadY = 6.f;

// Row geometry, in units of the font size so it follows the player's ReShade font. The label column is a
// fixed width, never more than kLabelShare of a narrow panel, so every control on the panel starts at the
// same x; a label wider than the column extends into it, its right edge against the control. A panel
// narrower than kStackEm has no room for a label column beside a usable control (ReShade docks it at
// ~450 px), so there each label goes on its own line over a control as wide as the panel. A slider's value
// sits just right of its rail, its right edge on the control column's, instead of under the knob.
constexpr float kLabelEm = 12.f;
constexpr float kLabelShare = 0.42f;
constexpr float kStackEm = 24.f;
constexpr float kTooltipEm = 35.f;

}  // namespace tokens

// ForkAwesome glyphs, as ReShade merges them (external/reshade/res/fonts/forkawesome.h: the subset ReShade
// ships, which is all the atlas holds). Add the ones a control needs; the atlas has no other glyph.
namespace icons {
constexpr const char* kUndo = "\xef\x83\xa2";       // U+F0E2 undo
}  // namespace icons

// Text sizes, relative to the size the player chose for ReShade (its FontSize setting, 13 by default). The
// panel draws 16/13 of that: at ReShade's default 13 px its dense rows were too small to read (2026-10-22
// owner), and scaling here keeps every other size in proportion, so a player who enlarges ReShade's font
// gets a correspondingly larger panel rather than one with mismatched headings.
inline constexpr float kFontScale = 16.f / 13.f;
inline float BaseFontSize() {
  const float base = ImGui::GetStyle().FontSizeBase;
  return (base > 0.f ? base : 13.f) * kFontScale;
}
inline float SmallFontSize() { return BaseFontSize() * (12.f / 13.f); }

inline ImU32 ToU32(const ImVec4& color) { return ImGui::GetColorU32(color); }

// Pushes and pops in one object, so a return from the middle of a panel cannot leave a colour, a variable
// or a font on ReShade's stacks. The named pairs are the three control roles the panel draws: its primary
// action, a quiet icon button, and the destructive confirmation.
class StyleScope {
 public:
  StyleScope() = default;
  StyleScope(const StyleScope&) = delete;
  StyleScope& operator=(const StyleScope&) = delete;
  ~StyleScope() {
    if (vars_ != 0) ImGui::PopStyleVar(vars_);
    if (colors_ != 0) ImGui::PopStyleColor(colors_);
    for (int i = 0; i < fonts_; ++i) ImGui::PopFont();
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
  StyleScope& FontSize(float size) {
    ImGui::PushFont(nullptr, size);
    ++fonts_;
    return *this;
  }
  StyleScope& Secondary() { return Color(ImGuiCol_Text, tokens::kTextSecondary); }
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
  StyleScope& DangerButton() {
    return Color(ImGuiCol_Button, tokens::kBad)
        .Color(ImGuiCol_ButtonHovered, tokens::kBadHover)
        .Color(ImGuiCol_ButtonActive, tokens::kBadActive)
        .Color(ImGuiCol_Text, tokens::kOnAccent);
  }

 private:
  int colors_ = 0;
  int vars_ = 0;
  int fonts_ = 0;
};

// Puts ReShade's own style back, in full. Call from AddonUninit; safe when the panel was never drawn.
void Restore();

// Applies the style over ReShade's own and re-applies the size fields for the current window's DPI
// scale. Call once per frame, first thing in the panel.
void RefreshForDpi();

}  // namespace uplift::ui::style
