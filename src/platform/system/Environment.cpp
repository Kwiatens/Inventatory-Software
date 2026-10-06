// Inventatory - Hardware Inventory Management System
// Safe environment-variable access shared by Windows integrations.

#include "platform/system/Environment.h"

#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace inventatory {

std::optional<std::string> environmentValue(const char* name) {
  if (name == nullptr || *name == '\0') return std::nullopt;

#ifdef _WIN32
  // Names and values are UTF-8 here; the CRT's narrow environment is ANSI and lossy for profile
  // paths with characters outside the active code page.
  const int wideNameLength = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
  if (wideNameLength <= 0) return std::nullopt;
  std::wstring wideName(static_cast<std::size_t>(wideNameLength), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wideName.data(), wideNameLength) <= 0) return std::nullopt;

  std::wstring wideValue(256U, L'\0');
  for (;;) {
    SetLastError(ERROR_SUCCESS);
    const DWORD length =
        GetEnvironmentVariableW(wideName.c_str(), wideValue.data(), static_cast<DWORD>(wideValue.size()));
    if (length == 0) {
      if (GetLastError() == ERROR_SUCCESS) return std::string();
      return std::nullopt;
    }
    if (static_cast<std::size_t>(length) >= wideValue.size()) {
      // The buffer was too small; length is the required size including the terminator.
      wideValue.assign(static_cast<std::size_t>(length), L'\0');
      continue;
    }
    wideValue.resize(static_cast<std::size_t>(length));
    break;
  }

  const int valueLength = WideCharToMultiByte(CP_UTF8, 0, wideValue.data(), static_cast<int>(wideValue.size()),
                                              nullptr, 0, nullptr, nullptr);
  if (valueLength <= 0) return std::nullopt;
  std::string value(static_cast<std::size_t>(valueLength), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wideValue.data(), static_cast<int>(wideValue.size()), value.data(),
                          valueLength, nullptr, nullptr) <= 0) {
    return std::nullopt;
  }
  return value;
#else
  const char* raw = std::getenv(name);
  return raw == nullptr ? std::nullopt : std::optional<std::string>(raw);
#endif
}

#ifndef _WIN32
std::filesystem::path homeDirectory() {
  if (const auto home = environmentValue("HOME"); home.has_value() && !home->empty()) return std::filesystem::path(*home);
  return std::filesystem::current_path();
}

namespace {

std::filesystem::path xdgHome(const char* variable, const char* fallback) {
  if (const auto xdg = environmentValue(variable);
      xdg.has_value() && !xdg->empty() && std::filesystem::path(*xdg).is_absolute()) {
    return std::filesystem::path(*xdg);
  }
  return homeDirectory() / fallback;
}

}  // namespace

std::filesystem::path xdgConfigHome() { return xdgHome("XDG_CONFIG_HOME", ".config"); }
std::filesystem::path xdgDataHome() { return xdgHome("XDG_DATA_HOME", ".local/share"); }

std::string stripDeletedExecutableSuffix(std::string path) {
  static const std::string kSuffix = " (deleted)";
  if (path.size() > kSuffix.size() && path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
    path.resize(path.size() - kSuffix.size());
  }
  return path;
}

std::filesystem::path currentExecutablePath() {
  std::vector<char> buffer(4096U);
  std::string target;
  for (;;) {
    const auto count = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count <= 0) return {};
    if (static_cast<size_t>(count) < buffer.size()) {
      target.assign(buffer.data(), static_cast<size_t>(count));
      break;
    }
    if (buffer.size() >= 1024U * 1024U) return {};
    buffer.resize(buffer.size() * 2U);
  }
  struct stat status{};
  // A file that really is called "... (deleted)" keeps its name.
  if (stat(target.c_str(), &status) == 0) return std::filesystem::path(target);
  const auto stripped = stripDeletedExecutableSuffix(target);
  if (stripped == target || stat(stripped.c_str(), &status) != 0) return {};
  return std::filesystem::path(stripped);
}
#endif

}  // namespace inventatory
