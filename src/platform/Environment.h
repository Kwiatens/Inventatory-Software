// Inventatory - Hardware Inventory Management System
// Safe environment-variable access shared by Windows integrations.

#pragma once

#include <optional>
#include <string>

namespace inventatory {

// Returns a value when the named environment variable is present. On Windows
// this owns a copy, so callers never retain a pointer into the CRT environment.
std::optional<std::string> environmentValue(const char* name);

}  // namespace inventatory
