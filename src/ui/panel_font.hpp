#pragma once

#include <imgui.h>

namespace uplift::ui {

// Load Microsoft YaHei regular and bold faces into the current (Uplift-owned) ImGui context.
bool LoadPanelTypeface();
ImFont* PanelBoldFont();

}  // namespace uplift::ui
