// Inventatory - Windows per-user startup registration for the background scanner service.

#pragma once

#include <string>

namespace inventatory {

std::wstring buildBackgroundStartupLauncherPath(const std::wstring& executablePath);
std::wstring buildBackgroundStartupCommand(const std::wstring& executablePath);
std::wstring buildDesktopShortcutPath(const std::wstring& desktopDirectory);
bool createDesktopShortcut(std::string& error);
bool setBackgroundStartupEnabled(bool enabled, std::string& error);
#ifndef _WIN32
// Starts the registered per-user background unit; false when it is not registered or systemd
// refuses, in which case the caller may fall back to a detached process.
bool startBackgroundServiceUnit(std::string& error);
#endif

}  // namespace inventatory
