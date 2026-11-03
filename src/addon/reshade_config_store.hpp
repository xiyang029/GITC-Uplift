#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "ui/settings.hpp"

namespace uplift::addon {

// ui::ConfigStore over ReShade.ini [Uplift], through ReShade's global config API.
class ReshadeConfigStore final : public ui::ConfigStore {
 public:
  [[nodiscard]] std::optional<std::string> Get(std::string_view key) const override;
  void Set(std::string_view key, std::string_view value) override;
};

// Writes ReShade's own [STYLE] font settings: a Chinese face for the interface, a Latin one ReShade merges
// over it for the ASCII glyphs, and the 16 px the panel's rows are measured in (ui/style.hpp). Call right after reshade::register_addon succeeded: ReShade reads [STYLE] when it builds
// the overlay's font atlas, which is after every add-on has loaded, so this is the one moment it lands.
void ApplyPanelFonts();

}  // namespace uplift::addon
