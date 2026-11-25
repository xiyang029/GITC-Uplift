#include "ui/panel_font.hpp"

#include <Windows.h>
#include <imgui.h>

#include <filesystem>

namespace uplift::ui {

namespace {
ImFont* g_panel_bold_font = nullptr;
}

bool LoadPanelTypeface() {
  auto& io = ImGui::GetIO();
  wchar_t windows_dir[MAX_PATH]{};
  const UINT length = GetWindowsDirectoryW(windows_dir, MAX_PATH);
  if (length <= 0 || length >= MAX_PATH) return false;
  const auto fonts = std::filesystem::path(windows_dir) / L"Fonts";
  io.FontDefault = io.Fonts->AddFontFromFileTTF((fonts / L"msyh.ttc").string().c_str(), 0.f);
  g_panel_bold_font = io.Fonts->AddFontFromFileTTF((fonts / L"msyhbd.ttc").string().c_str(), 0.f);
  return io.FontDefault != nullptr && g_panel_bold_font != nullptr;
}

ImFont* PanelBoldFont() { return g_panel_bold_font; }

}  // namespace uplift::ui
