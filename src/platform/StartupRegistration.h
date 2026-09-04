// Inventatory - Windows per-user startup registration for the background scanner service.

#pragma once

#include <string>

namespace inventatory {

std::wstring buildBackgroundStartupLauncherPath(const std::wstring& executablePath);
std::wstring buildBackgroundStartupCommand(const std::wstring& executablePath);
bool setBackgroundStartupEnabled(bool enabled, std::string& error);

}  // namespace inventatory
