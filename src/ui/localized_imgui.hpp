#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "addon/reshade_api.hpp"
#include "ui/localization.hpp"

namespace uplift::ui::gui {
using namespace ImGui;
inline thread_local Language language = Language::ENGLISH;

class LanguageScope {
 public:
  explicit LanguageScope(Language selected) : previous_(language) { language = selected; }
  ~LanguageScope() { language = previous_; }
 private:
  Language previous_;
};

inline std::string Display(std::string_view text) { return Localize(language, text); }
inline std::string Label(std::string_view text) { return LocalizedLabel(language, text); }
// The text a hand-drawn control paints itself. Label() keeps the "###id" suffix that lets ImGui tell two same-named
// controls apart; AddText knows no such rule and would print it.
inline std::string Visible(std::string_view text) {
  std::string shown = Label(text);
  const size_t stable_id = shown.find("###");
  if (stable_id != std::string::npos) shown.resize(stable_id);
  return shown;
}
inline std::string Formatted(const char* format, va_list args) {
  va_list count;
  va_copy(count, args);
  const int size = std::vsnprintf(nullptr, 0u, format, count);
  va_end(count);
  if (size < 0) return Display(format);
  std::string text(static_cast<size_t>(size) + 1u, '\0');
  std::vsnprintf(text.data(), text.size(), format, args);
  text.resize(static_cast<size_t>(size));
  return Display(text);
}

inline void TextUnformatted(const char* text, const char* end = nullptr) {
  const auto shown = Display(end ? std::string_view(text, end) : std::string_view(text));
  ImGui::TextUnformatted(shown.c_str());
}
inline void TextWrapped(const char* format, ...) {
  va_list args; va_start(args, format); const auto text = Formatted(format, args); va_end(args);
  ImGui::TextWrapped("%s", text.c_str());
}
inline void BulletText(const char* format, ...) {
  va_list args; va_start(args, format); const auto text = Formatted(format, args); va_end(args);
  ImGui::BulletText("%s", text.c_str());
}
inline ImVec2 CalcTextSize(const char* text, const char* end = nullptr, bool hide = false, float wrap = -1.f) {
  const auto shown = Display(end ? std::string_view(text, end) : std::string_view(text));
  return ImGui::CalcTextSize(shown.c_str(), nullptr, hide, wrap);
}
inline bool Button(const char* label, const ImVec2& size = ImVec2(0.f, 0.f)) { return ImGui::Button(Label(label).c_str(), size); }
inline bool InputInt(const char* label, int* value, int step = 1, int fast = 100, ImGuiInputTextFlags flags = 0) { return ImGui::InputInt(Label(label).c_str(), value, step, fast, flags); }
inline bool InputText(const char* label, char* value, size_t size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* data = nullptr) { return ImGui::InputText(Label(label).c_str(), value, size, flags, callback, data); }
}  // namespace uplift::ui::gui
