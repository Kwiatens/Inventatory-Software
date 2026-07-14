// HIMS - Windows per-user startup registration for the background scanner service.

#pragma once

#include <string>

namespace hims {

std::wstring buildBackgroundStartupCommand(const std::wstring& executablePath);
bool setBackgroundStartupEnabled(bool enabled, std::string& error);

}  // namespace hims
