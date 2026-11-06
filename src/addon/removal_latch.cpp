#include "addon/removal_latch.hpp"

#include <Windows.h>

#include <cstdlib>
#include <format>
#include <system_error>

#include "addon/environment.hpp"
#include "nr/log.hpp"

namespace uplift::addon {

std::filesystem::path UpliftStateDirectory() {
  wchar_t* state_dir_override = nullptr;
  if (_wdupenv_s(&state_dir_override, nullptr, L"UPLIFT_STATE_DIR") == 0 && state_dir_override != nullptr) {
    const std::filesystem::path directory(state_dir_override);
    std::free(state_dir_override);
    return directory;
  }
  std::free(state_dir_override);
  wchar_t* local_app_data = nullptr;
  std::error_code error;
  const std::filesystem::path root = ((_wdupenv_s(&local_app_data, nullptr, L"LOCALAPPDATA") == 0 && local_app_data != nullptr)
                                          ? std::filesystem::path(local_app_data)
                                          : std::filesystem::temp_directory_path(error))
                                     / L"Uplift";
  std::free(local_app_data);
  return root;
}

// FNV-1a 32-bit over the full exe path's UTF-8 bytes: enough entropy to tell two game installs apart,
// no cryptographic hash needed for a file name.
// Minor (fix round 2): case-folded first (LOCALE_INVARIANT, so this does not depend on the system's
// display language) -- Windows paths are case-insensitive, but the hash was comparing raw bytes, so
// a launch through a differently-cased path or a junction missed the marker an earlier session wrote.
static std::filesystem::path MarkerPath(const std::filesystem::path& state_directory,
                                        const std::filesystem::path& game_exe_path, std::wstring_view suffix) {
  std::wstring folded = game_exe_path.wstring();
  if (!folded.empty()) {
    const int length =
        LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, folded.data(), static_cast<int>(folded.size()), nullptr, 0);
    if (length > 0) {
      std::wstring upper(static_cast<size_t>(length), L'\0');
      LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, folded.data(), static_cast<int>(folded.size()), upper.data(), length);
      folded = std::move(upper);
    }
  }
  const std::string utf8_path = Utf8FromPath(folded);
  uint32_t hash = 2166136261u;
  for (const char byte : utf8_path) {
    hash ^= static_cast<uint8_t>(byte);
    hash *= 16777619u;
  }
  std::wstring file_name = game_exe_path.stem().wstring();
  file_name += L'-';
  for (const char digit : std::format("{:08x}", hash)) {  // ASCII hex digits: safe to widen one by one
    file_name += static_cast<wchar_t>(digit);
  }
  file_name += suffix;
  return state_directory / L"latch" / file_name;
}

std::filesystem::path LatchMarkerPath(const std::filesystem::path& state_directory, const std::filesystem::path& game_exe_path) {
  return MarkerPath(state_directory, game_exe_path, L".latch");
}

std::filesystem::path XmxCrashMarkerPath(const std::filesystem::path& state_directory, const std::filesystem::path& game_exe_path) {
  return MarkerPath(state_directory, game_exe_path, L".xmx-crash");
}

uint32_t ReadXmxCrashStreak(const std::filesystem::path& marker_path) {
  const HANDLE file = CreateFileW(marker_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return 0u;
  char line[64]{};
  DWORD read = 0u;
  const BOOL ok = ReadFile(file, line, sizeof(line) - 1u, &read, nullptr);
  CloseHandle(file);
  if (ok == FALSE) return 0u;
  const std::string text(line, read);
  const size_t space = text.find(' ');
  if (space == std::string::npos) return 0u;
  try {
    return static_cast<uint32_t>(std::stoul(text.substr(space + 1u)));
  } catch (...) {
    return 0u;  // a truncated or hand-edited marker counts as none
  }
}

void WriteXmxCrashMarker(const std::filesystem::path& marker_path, uint32_t streak) {
  std::error_code error;
  std::filesystem::create_directories(marker_path.parent_path(), error);
  const HANDLE file = CreateFileW(marker_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    nr::Logf(nr::LogLevel::ERR, "could not write the XMX crash marker {}: error {:#010x}",
             Utf8FromPath(marker_path), static_cast<uint32_t>(GetLastError()));
    return;
  }
  const std::string line = std::format("{} {}", GetCurrentProcessId(), streak);
  DWORD written = 0u;
  if (WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) == FALSE) {
    nr::Logf(nr::LogLevel::ERR, "could not write the XMX crash marker {}: error {:#010x}",
             Utf8FromPath(marker_path), static_cast<uint32_t>(GetLastError()));
  } else {
    FlushFileBuffers(file);  // the next start has to see it even if this one dies right after
  }
  CloseHandle(file);
}

void DeleteXmxCrashMarker(const std::filesystem::path& marker_path) {
  std::error_code error;
  std::filesystem::remove(marker_path, error);
}

void WriteLatchMarker(const std::filesystem::path& marker_path, std::string_view message) {
  std::error_code error;
  std::filesystem::create_directories(marker_path.parent_path(), error);
  const HANDLE file = CreateFileW(marker_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    nr::Logf(nr::LogLevel::ERR, "could not write the device-removal latch marker {}: error {:#010x}",
             Utf8FromPath(marker_path), static_cast<uint32_t>(GetLastError()));
    return;
  }
  SYSTEMTIME time = {};
  GetLocalTime(&time);
  const std::string line = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} {}\n", time.wYear, time.wMonth, time.wDay,
                                       time.wHour, time.wMinute, time.wSecond, message);
  DWORD written = 0u;
  if (WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) == FALSE) {
    nr::Logf(nr::LogLevel::ERR, "could not write the device-removal latch marker {}: error {:#010x}",
             Utf8FromPath(marker_path), static_cast<uint32_t>(GetLastError()));
  } else {
    FlushFileBuffers(file);  // reaches disk before this call returns, even if the process dies right after
  }
  CloseHandle(file);
}

bool LatchMarkerExists(const std::filesystem::path& marker_path) {
  std::error_code error;
  return std::filesystem::is_regular_file(marker_path, error);
}

void DeleteLatchMarker(const std::filesystem::path& marker_path) {
  std::error_code error;
  std::filesystem::remove(marker_path, error);
}

}  // namespace uplift::addon
