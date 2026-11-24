#pragma once

#include <map>

#include "ui/panel_context.hpp"

namespace reshade::api { struct effect_runtime; }

namespace uplift::addon {

// The panel owns its ImGui context. ReShade receives only its clipped triangles
// and a texture view for each draw command, not its fonts or widget state.
class PanelRenderer {
 public:
  bool Draw(reshade::api::effect_runtime* runtime, const ui::OverlayView& view, ui::Settings* settings,
            ui::OverlayState* state);
  void Suspend(reshade::api::effect_runtime* runtime);
  void Release(reshade::api::effect_runtime* runtime);

 private:
  struct Panel {
    ui::PanelContext context;
    std::map<uint64_t, uint64_t> textures;  // resource view -> resource
  };
  std::map<reshade::api::effect_runtime*, Panel> panels_;
};

}  // namespace uplift::addon
