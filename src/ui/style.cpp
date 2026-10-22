#include "ui/style.hpp"

#include <algorithm>  // [style] std::max in ApplyBorders

#include "addon/reshade_api.hpp"

namespace uplift::ui::style {
namespace {

// Two colours blended, `t` weighting `to`. The two states that are the accent showing through a surface --
// a pressed button, an active row -- are built this way, so there is one accent and no hand-picked second
// blue to drift away from it.
constexpr ImVec4 Mix(const ImVec4& from, const ImVec4& to, float t) {
  return ImVec4(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t,
                from.w + (to.w - from.w) * t);
}

// Every colour below is style.hpp's tokens -- the same values the widget layer draws with -- so the two
// cannot drift apart. The border is a hairline, and drawn only where a step of the ramp cannot be seen.

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
  // [style] a hairline that holds at 1 device px on high-DPI too: round instead of multiply, and never 0
  style.FrameBorderSize = (std::max)(1.f, 1.f * scale);
  style.PopupBorderSize = (std::max)(1.f, 1.f * scale);
  style.TabBorderSize = 0.f;
  style.TabBarBorderSize = 0.f;
  style.SeparatorTextBorderSize = 0.f;
}

// The three tri-state groups step up by the same small amount from rest to hover to active, and sit at
// different levels, so a hover is visible without a border and a slider no longer reads as the button next
// to it. The layers carry the hierarchy the borders no longer do, and the accent is spent on the marks that
// say "this is live": the four interactive ones, the text caret, a link, and the tab that is selected.
void ApplyColors() {
  ImGuiStyle& style = ImGui::GetStyle();
  ImVec4* c = style.Colors;  // a plain data member: no ImGui call, so this links against nothing

  c[ImGuiCol_Text] = tokens::kTextPrimary;
  c[ImGuiCol_TextDisabled] = tokens::kTextSecondary;
  c[ImGuiCol_TextLink] = tokens::kAccent;
  c[ImGuiCol_WindowBg] = tokens::kBgWindow;
  c[ImGuiCol_ChildBg] = tokens::kBgCard;
  c[ImGuiCol_PopupBg] = tokens::kBgCard;
  c[ImGuiCol_Border] = tokens::kStroke;
  c[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);

  // Input, slider, drag, combo, checkbox, colour edit and the plot background: all of them sit in the same
  // well the cards do, which is the edge the frame's hairline then draws.
  c[ImGuiCol_FrameBg] = tokens::kBgCard;
  c[ImGuiCol_FrameBgHovered] = tokens::kBgControlHover;
  // [style] shared accent wash instead of an inline 0.12
  c[ImGuiCol_FrameBgActive] = Mix(tokens::kBgControlHover, tokens::kAccent, tokens::kAccentWash.w);

  // Buttons and radio rows: one level above the panel, and the press shows a little of the accent, so a
  // click answers on the surface itself where the accent is not the surface.
  c[ImGuiCol_Button] = tokens::kBgControl;
  c[ImGuiCol_ButtonHovered] = tokens::kBgControlHover;
  c[ImGuiCol_ButtonActive] = tokens::kAccentActive;

  // Selectable rows, tree nodes, list items -- and, since it is the framed colour, every section header:
  // the header is a card's head, so it holds a step of the ramp of its own instead of the panel's.
  c[ImGuiCol_Header] = tokens::kBgCard;
  c[ImGuiCol_HeaderHovered] = tokens::kBgControlHover;
  c[ImGuiCol_HeaderActive] = Mix(tokens::kBgControlHover, tokens::kAccent, 0.20f);  // [style] 0.18 -> 0.20

  // Title bar and tabs: the panel's own layer when they are not the active one, one step up when they
  // are -- never ReShade's default blue. The selected tab carries an underline as well as its layer, which
  // reads at a glance whatever its width.
  c[ImGuiCol_TitleBg] = tokens::kBgWindow;
  c[ImGuiCol_TitleBgActive] = Mix(tokens::kBgWindow, tokens::kBgControlHover, 0.35f);
  c[ImGuiCol_TitleBgCollapsed] = tokens::kBgWindow;
  c[ImGuiCol_MenuBarBg] = tokens::kBgWindow;
  c[ImGuiCol_Tab] = tokens::kBgWindow;
  c[ImGuiCol_TabHovered] = tokens::kBgControlHover;
  c[ImGuiCol_TabSelected] = Mix(tokens::kBgWindow, tokens::kBgControlHover, 0.45f);
  c[ImGuiCol_TabSelectedOverline] = tokens::kAccent;
  c[ImGuiCol_TabDimmed] = tokens::kBgCard;
  c[ImGuiCol_TabDimmedSelected] = Mix(tokens::kBgWindow, tokens::kBgControlHover, 0.35f);
  // [style] token instead of an inline 0.45
  c[ImGuiCol_TabDimmedSelectedOverline] = tokens::kAccentStrong;

  // Thin and quiet: a near-invisible track, a mid-grey grab.
  c[ImGuiCol_ScrollbarBg] = ImVec4(tokens::kBgWindow.x, tokens::kBgWindow.y, tokens::kBgWindow.z, 0.20f);
  c[ImGuiCol_ScrollbarGrab] = Mix(tokens::kBgControl, tokens::kBgControlHover, 0.35f);
  c[ImGuiCol_ScrollbarGrabHovered] = Mix(tokens::kBgControl, tokens::kBgControlHover, 0.65f);
  c[ImGuiCol_ScrollbarGrabActive] = tokens::kBgControlHover;

  c[ImGuiCol_CheckMark] = tokens::kAccent;
  c[ImGuiCol_SliderGrab] = tokens::kAccent;
  c[ImGuiCol_SliderGrabActive] = tokens::kAccentHover;
  c[ImGuiCol_InputTextCursor] = tokens::kAccent;
  // [style] shared halo token instead of two inline 0.30
  c[ImGuiCol_NavCursor] = tokens::kAccentHalo;
  c[ImGuiCol_TextSelectedBg] = tokens::kAccentHalo;
  c[ImGuiCol_UnsavedMarker] = tokens::kWarn;

  c[ImGuiCol_Separator] = tokens::kStroke;
  c[ImGuiCol_SeparatorHovered] = tokens::kAccentStrong;  // [style] inline 0.40 -> token
  c[ImGuiCol_SeparatorActive] = tokens::kAccent;
  c[ImGuiCol_ResizeGrip] = ImVec4(tokens::kBgControlHover.x, tokens::kBgControlHover.y, tokens::kBgControlHover.z, 0.30f);
  c[ImGuiCol_ResizeGripHovered] = ImVec4(tokens::kBgControlHover.x, tokens::kBgControlHover.y, tokens::kBgControlHover.z, 0.55f);
  c[ImGuiCol_ResizeGripActive] = ImVec4(tokens::kAccent.x, tokens::kAccent.y, tokens::kAccent.z, 0.70f);

  c[ImGuiCol_PlotLines] = tokens::kAccent;
  c[ImGuiCol_PlotLinesHovered] = tokens::kAccent;
  c[ImGuiCol_PlotHistogram] = tokens::kAccent;
  c[ImGuiCol_PlotHistogramHovered] = tokens::kAccent;

  c[ImGuiCol_TableHeaderBg] = tokens::kBgCard;
  c[ImGuiCol_TableBorderStrong] = Mix(tokens::kBgCard, tokens::kBgControlHover, 0.5f);
  c[ImGuiCol_TableBorderLight] = tokens::kStroke;
  c[ImGuiCol_TableRowBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1.f, 1.f, 1.f, 0.025f);

  c[ImGuiCol_TreeLines] = ImVec4(tokens::kBgControlHover.x, tokens::kBgControlHover.y, tokens::kBgControlHover.z, 0.70f);
  c[ImGuiCol_DragDropTarget] = ImVec4(tokens::kAccent.x, tokens::kAccent.y, tokens::kAccent.z, 0.90f);
  c[ImGuiCol_DragDropTargetBg] = tokens::kAccentWash;  // [style] inline 0.15 -> token
  c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.f, 1.f, 1.f, 0.70f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.f, 0.f, 0.f, 0.35f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.55f);
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

  // style.hpp's radius family, carried by how high a thing sits: a panel and the popups above it take the
  // card radius plus a step, a card or a scrollbar inside it the card radius, the controls on it the control
  // radius, and a grab the small one -- and style.hpp's spacing rhythm, so the widgets the panel draws and
  // the ones ImGui draws for it land on the same grid.
  style.WindowRounding = (tokens::kRadiusCard + 2.f) * scale;
  style.PopupRounding = (tokens::kRadiusCard + 2.f) * scale;
  style.ChildRounding = tokens::kRadiusCard * scale;
  style.ScrollbarRounding = tokens::kRadiusCard * scale;
  style.FrameRounding = tokens::kRadiusControl * scale;
  style.GrabRounding = tokens::kRadiusSmall * scale;
  style.TabRounding = tokens::kRadiusControl * scale;
  style.DragDropTargetRounding = tokens::kRadiusControl * scale;

  // [style] a touch more room: the frame padding drives every control's height, and the panel read tight
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

  ApplyColors();
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
  // colours back over the panel's. Applying is one struct copy and some sixty assignments.
  //
  // The window already knows its monitor's scale, so this needs no HWND and no WM_DPICHANGED handling of
  // its own: a window dragged to another monitor re-applies here.
  const float dpi = ImGui::GetWindowDpiScale();
  ApplyAt((dpi > 0.01f && dpi < 8.f) ? dpi : 1.f);
}

}  // namespace uplift::ui::style