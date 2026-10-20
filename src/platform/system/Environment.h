// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Safe environment-variable access shared by Windows integrations.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace inventatory {

// On Windows the name and value are UTF-8; build a path from the value with std::filesystem::u8path.
std::optional<std::string> environmentValue(const char* name);

#ifndef _WIN32
// $HOME, or the working directory when it is unset or empty.
std::filesystem::path homeDirectory();
// $XDG_CONFIG_HOME / $XDG_DATA_HOME when set to an absolute path, otherwise $HOME/.config / $HOME/.local/share.
std::filesystem::path xdgConfigHome();
std::filesystem::path xdgDataHome();

// Removes the " (deleted)" suffix Linux adds to a replaced binary; other strings are unchanged.
std::string stripDeletedExecutableSuffix(std::string path);

// Running executable path without the suffix. Empty when unresolved or missing.
std::filesystem::path currentExecutablePath();
#endif

}  // namespace inventatory
