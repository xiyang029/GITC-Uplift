#include "ui/panel_font.hpp"

#include <Windows.h>
#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace uplift::ui {

bool LoadPanelTypeface() {
  auto& io = ImGui::GetIO();
  wchar_t windows_dir[MAX_PATH]{};
  const UINT length = GetWindowsDirectoryW(windows_dir, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    const auto fonts = std::filesystem::path(windows_dir) / L"Fonts";
    for (const auto* name : {L"msyh.ttc", L"simsun.ttc", L"segoeui.ttf"}) {
      std::ifstream file(fonts / name, std::ios::binary | std::ios::ate);
      const auto size = static_cast<std::streamoff>(file.tellg());
      if (size <= 100 || size > 64 * 1024 * 1024) continue;
      std::vector<char> bytes(static_cast<size_t>(size));
      if (!file.seekg(0).read(bytes.data(), size)) continue;
      ImFontConfig config;
      config.Flags = ImFontFlags_NoLoadError;
      config.FontDataOwnedByAtlas = false;  // ImGui copies the bytes into its own atlas.
      io.FontDefault = io.Fonts->AddFontFromMemoryTTF(bytes.data(), static_cast<int>(size), 0.f, &config);
      if (io.FontDefault) return io.FontDefault->IsGlyphInFont(0x4e2d);
    }
  }
  io.FontDefault = io.Fonts->AddFontDefault();
  return false;
}

}  // namespace uplift::ui
