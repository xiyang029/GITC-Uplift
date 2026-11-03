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

// Fills in ReShade's [STYLE] Font, LatinFont and FontSize where the player's ini is silent, at the 16 px
// this add-on ships with. Call right after reshade::register_addon: ReShade reads [STYLE] when it builds its
// font atlas, after every add-on loaded, which is the one moment a write lands.
void ApplyPanelFonts();

}  // namespace uplift::addon
