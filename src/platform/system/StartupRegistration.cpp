// Inventatory - Windows per-user startup registration for the background scanner service.

#define WIN32_LEAN_AND_MEAN
#include "platform/system/StartupRegistration.h"

#include <windows.h>

#include <string>

namespace inventatory {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"Inventatory Background Service";
constexpr wchar_t kLegacyValueNames[][32] = {L"InventatorySoftware", L"HIMSSoftware"};

std::string systemError(DWORD code) {
  char message[256] = {};
  const DWORD count = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                     message, static_cast<DWORD>(sizeof(message)), nullptr);
  return count == 0 ? "Windows error " + std::to_string(code) : std::string(message, count);
}

std::wstring currentExecutablePath() {
  std::wstring path(MAX_PATH, L'\0');
  DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  while (length == path.size()) {
    path.resize(path.size() * 2);
    length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  }
  if (length == 0) return {};
  path.resize(length);
  return path;
}

}  // namespace

std::wstring buildBackgroundStartupLauncherPath(const std::wstring& executablePath) {
  const auto separator = executablePath.find_last_of(L"\\/");
  const auto directory = separator == std::wstring::npos ? std::wstring() : executablePath.substr(0, separator + 1);
  return directory + L"inventatory-background.exe";
}

namespace {

LONG deleteStartupValue(HKEY key, const wchar_t* valueName) {
  const LONG result = RegDeleteValueW(key, valueName);
  return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
}

}  // namespace

std::wstring buildBackgroundStartupCommand(const std::wstring& executablePath) {
  return L"\"" + executablePath + L"\" --background";
}

bool setBackgroundStartupEnabled(bool enabled, std::string& error) {
  HKEY key = nullptr;
  const LONG openResult = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (openResult != ERROR_SUCCESS) {
    error = "Unable to open Windows startup settings: " + systemError(openResult);
    return false;
  }

  LONG result = ERROR_SUCCESS;
  if (enabled) {
    const auto executablePath = currentExecutablePath();
    if (executablePath.empty()) {
      RegCloseKey(key);
      error = "Unable to find the Inventatory executable for Windows startup";
      return false;
    }
    const auto launcherPath = buildBackgroundStartupLauncherPath(executablePath);
    const DWORD launcherAttributes = GetFileAttributesW(launcherPath.c_str());
    if (launcherAttributes == INVALID_FILE_ATTRIBUTES || (launcherAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      RegCloseKey(key);
      error = "Unable to find the Inventatory background launcher for Windows startup";
      return false;
    }
    const auto command = buildBackgroundStartupCommand(launcherPath);
    result = RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (result == ERROR_SUCCESS) {
      for (const auto* legacyValueName : kLegacyValueNames) {
        result = deleteStartupValue(key, legacyValueName);
        if (result != ERROR_SUCCESS) break;
      }
    }
  } else {
    result = deleteStartupValue(key, kValueName);
    if (result == ERROR_SUCCESS) {
      for (const auto* legacyValueName : kLegacyValueNames) {
        result = deleteStartupValue(key, legacyValueName);
        if (result != ERROR_SUCCESS) break;
      }
    }
  }
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) {
    error = "Unable to update Windows startup: " + systemError(result);
    return false;
  }
  return true;
}

}  // namespace inventatory
