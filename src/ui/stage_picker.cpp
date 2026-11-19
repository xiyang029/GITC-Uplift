#include "ui/overlay.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string>

#include "ui/overlay_style.hpp"
#include "ui/overlay_widgets.hpp"
#include "ui/settings_schema.hpp"

namespace uplift::ui {
namespace {

constexpr std::array<const char*, 4> TIPS{
    "Run pass 1 on the game's render-resolution input before DLSS upscales it.",
    "Run pass 1 on the upscaled image before the game's remaining post-processing.",
    "Run Present passes on the finished real frame before Streamline frame generation.",
    "Run Present passes at ReShade's Present placement. With downstream ReShade this includes generated frames."};

void CenterText(ImDrawList& draw, ImVec2 center, ImU32 color, const std::string& label) {
  const ImVec2 size = ImGui::CalcTextSize(label.c_str());
  draw.AddText({center.x - size.x * 0.5f, center.y - size.y * 0.5f}, color, label.c_str());
}

void Arrow(ImDrawList& draw, ImVec2 from, ImVec2 to, ImU32 color, float head, float stroke) {
  draw.AddLine(from, to, color, stroke);
  if (to.x == from.x) {
    draw.AddTriangleFilled(to, {to.x - head, to.y - head * 1.6f}, {to.x + head, to.y - head * 1.6f}, color);
  } else {
    draw.AddTriangleFilled(to, {to.x - head * 1.6f, to.y - head}, {to.x - head * 1.6f, to.y + head}, color);
  }
}

}  // namespace

bool DrawStagePicker(const OverlayView& view, Settings* settings, OverlayState* state) {
  gui::PushID("Source");
  bool changed = false;
  const bool can_reset = !IsDefault(*settings, *FindSetting("Source")) || !IsDefault(*settings, *FindSetting("PreUpscale"));
  const float diagram_width = std::max(1.f, gui::GetContentRegionAvail().x);
  const float right = gui::GetCursorScreenPos().x + diagram_width;
  widgets::GroupHeading("NR stage");
  widgets::ItemTooltip("Choose a stage for pass 1");
  if (can_reset) {
    const char* const default_stage = (view.dlss_explicit ? "Present on Vulkan" : "After DLSS");
    const bool reset = widgets::RightLink(style::icons::kUndo, right);
    widgets::ItemTooltip("Reset to default (%s)", gui::Display(default_stage).c_str());
    if (reset) {
      const auto pick = view.dlss_explicit ? SourcePick::PRESENT : SourcePick::AFTER_DLSS;
      SetSourcePick(settings, pick, view.dlss_explicit);
      state->setup_settle.ClickedBest(SetupRow::STAGE, static_cast<uint32_t>(pick), view.setup);
      changed = true;
    }
  }
  widgets::NextGap(style::BlockGap());
  const float em = gui::GetFontSize();
  const ImVec2 origin = gui::GetCursorScreenPos();
  const float width = diagram_width;
  const bool folded = !style::FitsStageColumns(width);
  // The panel's one column rule: the six nodes sit on one line only while the diagram is FitsStageColumns wide,
  // and the sections under it take their two columns from this same answer rather than from a width of their own,
  // so a folded diagram is never followed by a two-column reading.
  widgets::stage_columns = !folded;
  const int columns = folded ? 3 : 6;
  const float margin = std::min(style::RowGap(), width * 0.03f);
  const float gap = std::min(style::BlockGap(), width * 0.05f);
  const float cell = std::max(1.f, (width - 2.f * margin - gap * (columns - 1)) / columns);
  const float height = 2.f * em + 2.f * style::LabelGap();
  const float pitch = cell + gap;
  const float button_h = 2.f * em + gui::GetStyle().FramePadding.y * 2.f;
  const float hook_y = height * 0.5f + style::BlockGap();
  const float row = height * 0.5f + hook_y + button_h + style::LabelGap() + em + style::SectionGap();
  const float button_w = std::max(1.f, std::min({9.2f * em, pitch - style::LabelGap(), width - 2.f * margin}));
  const float bottom = origin.y + (folded ? row : 0.f) + height * 0.5f + hook_y + button_h + style::LabelGap() + em;
  ImDrawList& draw = *gui::GetWindowDrawList();
  const ImU32 wire = style::ToU32(style::tokens::kStroke);
  const float stroke = std::max(1.f, style::Px(1.f));
  const float head = std::min(style::LabelGap() * 0.55f, gap * 0.25f);
  std::array<ImVec2, 6> centers{};
  for (int i = 0; i < 6; ++i) centers[i] = {origin.x + margin + cell * 0.5f + (i % columns) * pitch,
                                              origin.y + height * 0.5f + (i / columns) * row};

  for (int i = 0; i < 5; ++i) {
    const ImVec2 from{centers[i].x + cell * 0.5f, centers[i].y};
    const ImVec2 to{centers[i + 1].x - cell * 0.5f, centers[i + 1].y};
    if (folded && i == 2) {
      const float edge = origin.x + width - stroke, left = origin.x + stroke;
      const float mid = to.y - height * 0.5f - style::RowGap();
      const ImVec2 turn[]{from, {edge, from.y}, {edge, mid}, {left, mid}, {left, to.y}};
      draw.AddPolyline(turn, 5, wire, ImDrawFlags_None, stroke);
      Arrow(draw, turn[4], to, wire, head, stroke);
    } else {
      Arrow(draw, from, to, wire, head, stroke);
    }
  }

  const std::array<const char*, 6> nodes{"Game\nscene", "Ray\nReconstruction", "DLSS\nupscaling", "Game PP\nand HUD", "Frame\ngeneration", "Present\nfor display"};
  const std::array<const char*, 6> compact{"Game", "RR", "DLSS", "PP + HUD", "FG", "Display"};
  for (int i = 0; i < 6; ++i) {
    const ImVec2 c = centers[i], lo{c.x - cell * 0.5f, c.y - height * 0.5f}, hi{c.x + cell * 0.5f, c.y + height * 0.5f};
    const bool inactive = (i == 1 && !view.setup.before_upscaling_on_rr) || (i == 4 && !view.frame_generation_active);
    const ImU32 surface = style::ToU32(inactive ? style::tokens::Mix(style::tokens::kBgCard, style::tokens::kBgControl, 0.5f)
                                                 : style::tokens::kBgControl);
    if (i == 4) {
      const float inset = std::min(cell * 0.12f, em * 0.75f);
      const ImVec2 hex[]{{lo.x + inset, lo.y}, {hi.x - inset, lo.y}, {hi.x, c.y}, {hi.x - inset, hi.y}, {lo.x + inset, hi.y}, {lo.x, c.y}};
      draw.AddConvexPolyFilled(hex, 6, surface);
    } else {
      draw.AddRectFilled(lo, hi, surface, style::ControlRadius(height));
    }
    std::string label = gui::Display(nodes[i]);
    const float room = cell - (i == 4 ? std::min(cell * 0.24f, em * 1.5f) : style::LabelGap());
    if (ImGui::CalcTextSize(label.c_str()).x > room) label = widgets::Ellipsize(gui::Display(compact[i]).c_str(), room);
    CenterText(draw, c, style::ToU32(inactive ? style::tokens::kTextSecondary : style::tokens::kTextPrimary), label);
    if (gui::IsWindowHovered() && gui::IsMouseHoveringRect(lo, hi)) {
      if (i == 1) widgets::MouseTooltip("Ray Reconstruction denoises and upscales together when used in place of DLSS Super Resolution.");
      else widgets::MouseTooltip("%s", gui::Display(nodes[i]).c_str());
    }
  }

  const auto preferred = SourcePickOf(*settings, view.dlss_explicit);
  const bool before_framegen = view.frame_generation_active && settings->fg_present == Settings::FgPresent::BEFORE;
  const size_t selected = preferred == SourcePick::PRESENT ? (before_framegen ? 2u : 3u) : static_cast<size_t>(preferred);
  constexpr std::array<int, 4> preceding{0, 2, 3, 4};
  for (size_t i = 0; i < NR_STAGE_NAMES.size(); ++i) {
    const int prior = preceding[i];
    const ImVec2 c = centers[prior];
    const bool at_fold = folded && prior == 2;
    const float tap_x = at_fold ? origin.x + width - stroke : c.x + pitch * 0.5f;
    const float inset = margin + button_w * 0.5f;
    const float x = origin.x + std::clamp(tap_x - origin.x, std::min(inset, width * 0.5f), std::max(width - inset, width * 0.5f));
    const float y = c.y + hook_y;
    const float bend_y = y - style::RowGap();
    if (!at_fold) draw.AddLine({tap_x, c.y}, {tap_x, bend_y}, wire, stroke);
    if (tap_x != x) draw.AddLine({tap_x, bend_y}, {x, bend_y}, wire, stroke);
    draw.AddCircleFilled({tap_x, at_fold ? bend_y : c.y}, head, wire);
    Arrow(draw, {x, bend_y}, {x, y}, wire, head, stroke);

    const auto source = static_cast<SourcePick>(std::min(i, size_t{2}));
    std::string_view why = view.setup.stage_options[static_cast<size_t>(source)].why;
    if (source == SourcePick::PRESENT && why == FG_PRESENT_OFF_REASON) why = {};
    if (i == 2 && !view.before_fg_unavailable.empty()) why = view.before_fg_unavailable;
    const auto run = view.stage_runs[i];
    gui::PushID(static_cast<int>(i));
    gui::SetCursorScreenPos({x - button_w * 0.5f, y});
    const std::string full_label = gui::Display(NR_STAGE_NAMES[i]);
    const float room = button_w - 2.f * style::LabelGap();
    std::string label = full_label;
    if (i == 2 && ImGui::CalcTextSize(label.c_str()).x > room) label = gui::Display("Before\nFramegen");
    if (ImGui::CalcTextSize(label.c_str()).x > room) label = widgets::Ellipsize(full_label.c_str(), room);
    bool clicked;
    {
      style::StyleScope button;
      button.Var(ImGuiStyleVar_FrameRounding, style::ControlRadius(button_h))
          .Var(ImGuiStyleVar_FramePadding, ImVec2(style::LabelGap(), gui::GetStyle().FramePadding.y));
      if (selected == i) button.SelectedButton();
      gui::BeginDisabled(view.setup.stopped || !why.empty());
      clicked = ImGui::Button((label + "###stage").c_str(), {button_w, button_h});
      gui::EndDisabled();
    }
    if (gui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || (gui::GetIO().NavVisible && gui::IsItemFocused())) {
      const char* tip = i == 0 && view.setup.before_upscaling_on_rr ? "NR works on Ray Reconstruction's input." : TIPS[i];
      const std::string details = why.empty() ? gui::Display(tip) : gui::Display(why);
      if (gui::IsItemFocused() && gui::GetIO().NavVisible) widgets::MouseTooltip("%s\n%s", full_label.c_str(), details.c_str());
      else widgets::ItemTooltip("%s\n%s", full_label.c_str(), details.c_str());
    }
    gui::PopID();

    if (run.count) {
      const std::string passes = run.count == 1 ? std::format("P{}  x1", run.first + 1) : std::format("P{}-{}  x{}", run.first + 1, run.first + run.count, run.count);
      const float text_top = y + button_h + style::LabelGap();
      CenterText(draw, {x, text_top + em * 0.5f}, style::ToU32(style::tokens::kTextSecondary), widgets::Ellipsize(passes.c_str(), button_w));
    }
    if (!clicked) continue;
    if (i == 3 && view.frame_generation_active && settings->fg_present != Settings::FgPresent::AFTER) {
      state->confirm_present_stage = true;
      state->enable_warning_key = "PresentWithFrameGen";
    } else {
      SetSourcePick(settings, source, view.dlss_explicit);
      if (i == 2) settings->fg_present = Settings::FgPresent::BEFORE;
      state->setup_settle.Clicked(SetupRow::STAGE, static_cast<uint32_t>(source));
      changed = true;
    }
  }
  gui::SetCursorScreenPos(origin);
  gui::Dummy({width, bottom - origin.y});
  gui::PopID();
  return changed;
}

}  // namespace uplift::ui
