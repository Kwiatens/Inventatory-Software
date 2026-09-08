// Inventatory - Headless Windows launcher for the background scanner service.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

namespace {

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
std::wstring backgroundApplicationPath(const std::wstring& launcherPath) {
  const auto separator = launcherPath.find_last_of(L"\\/");
  const auto directory = separator == std::wstring::npos ? std::wstring() : launcherPath.substr(0, separator + 1);
  return directory + L"inventatory.exe";
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const auto launcherPath = currentExecutablePath();
  if (launcherPath.empty()) return 1;

  const auto applicationPath = backgroundApplicationPath(launcherPath);
  std::wstring commandLine = L"\"" + applicationPath + L"\" --background";
  std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back(L'\0');

  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};
  if (!CreateProcessW(applicationPath.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startupInfo, &processInfo)) {
    return static_cast<int>(GetLastError());
  }

  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);
  return 0;
}
