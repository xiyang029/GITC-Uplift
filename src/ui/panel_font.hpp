#pragma once

namespace uplift::ui {

// Load the face into the current (Uplift-owned) ImGui context. Reports whether
// a Chinese-capable face was found so the preview can pick a suitable language.
bool LoadPanelTypeface();

}  // namespace uplift::ui
