// Inventatory - Hardware Inventory Management System
// Safe environment-variable access shared by Windows integrations.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace inventatory {

// Returns a value when the named environment variable is present. On Windows
// the name and the value are UTF-8 (read through the wide API), so a caller that
// builds a path from the value must use std::filesystem::u8path.
std::optional<std::string> environmentValue(const char* name);

#ifndef _WIN32
// $HOME, or the working directory when it is unset or empty.
std::filesystem::path homeDirectory();
// $XDG_CONFIG_HOME / $XDG_DATA_HOME when set to an absolute path, otherwise $HOME/.config / $HOME/.local/share.
std::filesystem::path xdgConfigHome();
std::filesystem::path xdgDataHome();

// Linux reports the running binary as "<path> (deleted)" after it was replaced on disk (an update,
// a reinstall). This removes that suffix; other strings are returned unchanged.
std::string stripDeletedExecutableSuffix(std::string path);

// The path of the running executable with any " (deleted)" suffix removed. Empty when it cannot be
// resolved or no longer exists, so callers never register or exec a path that is not there.
std::filesystem::path currentExecutablePath();
#endif

}  // namespace inventatory
