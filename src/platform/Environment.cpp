// Inventatory - Hardware Inventory Management System
// Safe environment-variable access shared by Windows integrations.

#include "platform/Environment.h"

#include <cstdlib>

namespace inventatory {

std::optional<std::string> environmentValue(const char* name) {
  if (name == nullptr || *name == '\0') return std::nullopt;

#ifdef _WIN32
  char* raw = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) return std::nullopt;
  std::string value(raw);
  std::free(raw);
  return value;
#else
  const char* raw = std::getenv(name);
  return raw == nullptr ? std::nullopt : std::optional<std::string>(raw);
#endif
}

}  // namespace inventatory
