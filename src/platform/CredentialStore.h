// Inventatory - Hardware Inventory Management System
// User-scoped secret storage abstraction.

#pragma once

#include <optional>
#include <string>

namespace inventatory {

class CredentialStore {
 public:
  static std::optional<std::string> read(const std::string& key);
  static bool write(const std::string& key, const std::string& secret);
  static bool erase(const std::string& key);
};

}  // namespace inventatory
