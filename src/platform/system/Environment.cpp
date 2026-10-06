// Inventatory - Hardware Inventory Management System
// Safe environment-variable access shared by Windows integrations.

#include "platform/system/Environment.h"

#include <cstdlib>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
std::string stripDeletedExecutableSuffix(std::string path) {
  static const std::string kSuffix = " (deleted)";
  if (path.size() > kSuffix.size() && path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
    path.resize(path.size() - kSuffix.size());
  }
  return path;
}

std::filesystem::path currentExecutablePath() {
  std::vector<char> buffer(4096U);
  std::string target;
  for (;;) {
    const auto count = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count <= 0) return {};
    if (static_cast<size_t>(count) < buffer.size()) {
      target.assign(buffer.data(), static_cast<size_t>(count));
      break;
    }
    if (buffer.size() >= 1024U * 1024U) return {};
    buffer.resize(buffer.size() * 2U);
  }
  struct stat status{};
  // A file that really is called "... (deleted)" keeps its name.
  if (stat(target.c_str(), &status) == 0) return std::filesystem::path(target);
  const auto stripped = stripDeletedExecutableSuffix(target);
  if (stripped == target || stat(stripped.c_str(), &status) != 0) return {};
  return std::filesystem::path(stripped);
}
#endif

}  // namespace inventatory
