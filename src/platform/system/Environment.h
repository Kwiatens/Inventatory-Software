// Inventatory - Hardware Inventory Management System
// Safe environment-variable access shared by Windows integrations.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace inventatory {

// Returns a value when the named environment variable is present. On Windows
// this owns a copy, so callers never retain a pointer into the CRT environment.
std::optional<std::string> environmentValue(const char* name);

#ifndef _WIN32
// Linux reports the running binary as "<path> (deleted)" after it was replaced on disk (an update,
// a reinstall). This removes that suffix; other strings are returned unchanged.
std::string stripDeletedExecutableSuffix(std::string path);

// The path of the running executable with any " (deleted)" suffix removed. Empty when it cannot be
// resolved or no longer exists, so callers never register or exec a path that is not there.
std::filesystem::path currentExecutablePath();
#endif

}  // namespace inventatory
