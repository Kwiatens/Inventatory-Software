// Inventatory - Hardware Inventory Management System
// User-scoped secret storage abstraction.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace inventatory {

class CredentialStore {
 public:
  static std::optional<std::string> read(const std::string& key);
  static bool write(const std::string& key, const std::string& secret);
  static bool erase(const std::string& key);

  // Scanner credentials are scoped to the selected inventory workspace.  The
  // scope is a stable, non-secret digest of the normalized workspace path and
  // is part of the Credential Manager target name, so a newly selected
  // workspace cannot inherit the previous workspace's pairing token.
  static std::string workspaceScopedKey(const std::filesystem::path& workspaceDirectory,
                                        const std::string& key);
  static std::optional<std::string> readForWorkspace(const std::filesystem::path& workspaceDirectory,
                                                     const std::string& key);
  static bool writeForWorkspace(const std::filesystem::path& workspaceDirectory, const std::string& key,
                                const std::string& secret);
  static bool eraseForWorkspace(const std::filesystem::path& workspaceDirectory, const std::string& key);
};

}  // namespace inventatory
