// Inventatory - Windows per-user startup registration for the background scanner service.

#define WIN32_LEAN_AND_MEAN
#include "platform/system/StartupRegistration.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include <iomanip>
#include <memory>
#include <sstream>
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

std::string hresultError(const char* operation, HRESULT result) {
  std::ostringstream message;
  message << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
          << static_cast<unsigned long>(result) << ")";
  return message.str();
}

template <typename T>
struct ComRelease {
  void operator()(T* value) const noexcept {
    if (value != nullptr) value->Release();
  }
};

struct ComApartment {
  ComApartment() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), shouldUninitialize(SUCCEEDED(result)) {}
  ~ComApartment() {
    if (shouldUninitialize) CoUninitialize();
  }

  bool usable() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }

  HRESULT result;
  bool shouldUninitialize;
};

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

std::wstring buildDesktopShortcutPath(const std::wstring& desktopDirectory) {
  if (desktopDirectory.empty()) return L"Inventatory.lnk";
  const wchar_t last = desktopDirectory.back();
  if (last == L'\\' || last == L'/') return desktopDirectory + L"Inventatory.lnk";
  return desktopDirectory + L"\\Inventatory.lnk";
}

bool createDesktopShortcut(std::string& error) {
  const auto executablePath = currentExecutablePath();
  if (executablePath.empty()) {
    error = "Unable to find the Inventatory executable";
    return false;
  }

  const ComApartment apartment;
  if (!apartment.usable()) {
    error = hresultError("Initializing Windows Shell", apartment.result);
    return false;
  }

  PWSTR desktopDirectoryRaw = nullptr;
  HRESULT result = SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktopDirectoryRaw);
  if (FAILED(result)) {
    error = hresultError("Locating the Desktop folder", result);
    return false;
  }
  const std::wstring desktopDirectory(desktopDirectoryRaw);
  CoTaskMemFree(desktopDirectoryRaw);

  IShellLinkW* rawLink = nullptr;
  result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rawLink));
  if (FAILED(result)) {
    error = hresultError("Creating the desktop shortcut", result);
    return false;
  }
  std::unique_ptr<IShellLinkW, ComRelease<IShellLinkW>> link(rawLink);

  result = link->SetPath(executablePath.c_str());
  if (SUCCEEDED(result)) {
    const auto separator = executablePath.find_last_of(L"\\/");
    const auto workingDirectory = separator == std::wstring::npos ? std::wstring(L".") : executablePath.substr(0, separator);
    result = link->SetWorkingDirectory(workingDirectory.c_str());
  }
  if (FAILED(result)) {
    error = hresultError("Configuring the desktop shortcut", result);
    return false;
  }

  IPersistFile* rawFile = nullptr;
  result = link->QueryInterface(IID_PPV_ARGS(&rawFile));
  if (FAILED(result)) {
    error = hresultError("Saving the desktop shortcut", result);
    return false;
  }
  std::unique_ptr<IPersistFile, ComRelease<IPersistFile>> file(rawFile);
  const auto shortcutPath = buildDesktopShortcutPath(desktopDirectory);
  result = file->Save(shortcutPath.c_str(), TRUE);
  if (FAILED(result)) {
    error = hresultError("Saving the desktop shortcut", result);
    return false;
  }
  return true;
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
