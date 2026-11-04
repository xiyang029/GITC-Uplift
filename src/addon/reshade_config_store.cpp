#include "addon/reshade_config_store.hpp"

#include <algorithm>

#include "addon/reshade_api.hpp"

namespace uplift::addon {

std::optional<std::string> ReshadeConfigStore::Get(std::string_view key) const {
  const std::string key_text(key);
  size_t size = 0u;
  if (!reshade::get_config_value(nullptr, ui::CONFIG_SECTION, key_text.c_str(), nullptr, &size)) return std::nullopt;
  std::string value(size, '\0');
  if (!reshade::get_config_value(nullptr, ui::CONFIG_SECTION, key_text.c_str(), value.data(), &size)) return std::nullopt;
  value.resize(size);
  while (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  // ReShade splits a value at commas into NUL-separated elements; a path may contain commas.
  std::ranges::replace(value, '\0', ',');
  return value;
}

void ReshadeConfigStore::Set(std::string_view key, std::string_view value) {
  reshade::set_config_value(nullptr, ui::CONFIG_SECTION, std::string(key).c_str(), std::string(value).c_str());
}

namespace {
// Whether the player's ini answers a [STYLE] key; ReShade writes an unset one blank ("Font="), so a blank
// value is still ours to choose.
bool StyleIsSet(const char* name) {
  size_t size = 0u;
  if (!reshade::get_config_value(nullptr, "STYLE", name, nullptr, &size) || size == 0u) return false;
  std::string value(size, '\0');
  if (!reshade::get_config_value(nullptr, "STYLE", name, value.data(), &size)) return false;
  value.resize(size);
  // Spaces, and the NULs ReShade puts between comma-separated elements: nothing left over is unset. The
  // count is spelled out -- a plain " \t\0" argument would stop at the NUL and match no NUL at all.
  return value.find_first_not_of(" \t\0", 0, 3) != std::string::npos;
}
}  // namespace

void ApplyPanelFonts() {
  // ReShade writes plain floats into the ini ("13.000000"), so match that rather than fighting its parser.
  static constexpr char kChineseFont[] = "C:\\Windows\\Fonts\\msyh.ttc";
  static constexpr char kLatinFont[] = "C:\\Windows\\Fonts\\segoeui.ttf";
  static constexpr char kFontSize[] = "16.000000";
  // Fill in only what the player never chose: rows measure themselves in font sizes (ui/overlay_style.hpp), so any
  // size renders.
  if (!StyleIsSet("Font")) reshade::set_config_value(nullptr, "STYLE", "Font", kChineseFont);
  if (!StyleIsSet("LatinFont")) reshade::set_config_value(nullptr, "STYLE", "LatinFont", kLatinFont);
  if (!StyleIsSet("FontSize")) reshade::set_config_value(nullptr, "STYLE", "FontSize", kFontSize);
}

}  // namespace uplift::addon
