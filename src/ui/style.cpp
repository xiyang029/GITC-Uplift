#include "ui/style.hpp"

#include <algorithm>

#include "addon/reshade_api.hpp"

namespace uplift::ui::style {
namespace {

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
    {ImGuiCol_PopupBg, tokens::kBgCard},
    {ImGuiCol_Border, tokens::kStroke},
    {ImGuiCol_BorderShadow, tokens::kTransparent},

    // Inputs, sliders, drags, combos, checkboxes and plots all sit in the card well; only the hover and the
    // press move them, so the panel stays quiet until touched.
    {ImGuiCol_FrameBg, tokens::kBgCard},
    {ImGuiCol_FrameBgHovered, tokens::kBgControlHover},
    {ImGuiCol_FrameBgActive, tokens::Mix(tokens::kBgCard, tokens::kAccent, 0.14f)},

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
ImGuiStyle& OriginalStyle() {
  static ImGuiStyle original = ImGui::GetStyle();
  return original;
}

// The DPI scale the size fields were last applied at; 0 means "ReShade's own style is in place".
float& AppliedDpi() {
  static float dpi = 0.f;
  return dpi;
}

// Borders where they carry information and nowhere else: none around the panel or a card, since the ramp
// separates those two, and a hairline around a frame and around a popup -- a recessed fill alone is hard to
// make out on a dark panel, and a popup has neither a neighbour nor a shadow to lift it off what is behind
// it. A blue title bar or tab is the same kind of frame, so those stay flat in the panel's own colours too.
void ApplyBorders(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowBorderSize = 0.f;
  style.ChildBorderSize = 0.f;
  // A hairline that holds at 1 device px on high-DPI too: round instead of multiply, and never 0.
  style.FrameBorderSize = (std::max)(1.f, scale);
  style.PopupBorderSize = (std::max)(1.f, scale);
  style.TabBorderSize = 0.f;
  style.TabBarBorderSize = 0.f;
  style.SeparatorTextBorderSize = 0.f;
}

void ApplyPalette() {
  ImGuiStyle& style = ImGui::GetStyle();
  for (const SlotColor& entry : kPalette) {
    style.Colors[entry.slot] = entry.color;
  }
}

// Applies Uplift's style over ReShade's own, at `scale`. ReShade's style is the base every time, so a
// field ReShade sets that Uplift does not simply survives.
void ApplyAt(float scale) {
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

}  // namespace

void Restore() {
  if (AppliedDpi() == 0.f) return;  // nothing was applied: ReShade's style is already in place
  ImGui::GetStyle() = OriginalStyle();
  AppliedDpi() = 0.f;
}

void RefreshForDpi() {
  // Every frame, not only when the scale changes: ReShade re-reads its own style out of ReShade.ini
  // (runtime::load_config -> load_custom_style) whenever its settings are touched, which put ReShade's
  // colours back over the panel's. Applying is one struct copy and the palette table.
  //
  // The window already knows its monitor's scale, so this needs no HWND and no WM_DPICHANGED handling of
  // its own: a window dragged to another monitor re-applies here.
  const float dpi = ImGui::GetWindowDpiScale();
  ApplyAt((dpi > 0.01f && dpi < 8.f) ? dpi : 1.f);
}

}  // namespace uplift::ui::style
