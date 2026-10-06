// Inventatory - Hardware Inventory Management System
// User-scoped secret storage abstraction.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace inventatory {

// Outcome of reading a secret. Unavailable means the store itself could not answer (locked or
// missing keyring, no session bus), which must never be mistaken for "no secret is stored".
enum class CredentialReadStatus { Found, NotFound, Unavailable };

struct CredentialLookup {
  CredentialReadStatus status = CredentialReadStatus::NotFound;
  std::optional<std::string> secret;
};

// Whether a secret is stored, for display. A store that could not answer says nothing about the secret, so
// `previous` (the last known answer) is kept instead of reporting "not configured" for a stored secret.
inline bool credentialPresence(const CredentialLookup& lookup, bool previous) {
  switch (lookup.status) {
    case CredentialReadStatus::Found: return lookup.secret.has_value();
    case CredentialReadStatus::NotFound: return false;
    case CredentialReadStatus::Unavailable: break;
  }
  return previous;
}

class CredentialStore {
 public:
  // Distinguishes a missing secret from an unreadable store; read() reports both as nullopt.
  static CredentialLookup lookup(const std::string& key);
  static std::optional<std::string> read(const std::string& key);
  static bool write(const std::string& key, const std::string& secret);
  static bool erase(const std::string& key);

  // Scanner credentials are scoped to the selected inventory workspace.  The
  // scope is a stable, non-secret digest of the normalized workspace path and
  // is part of the Credential Manager target name, so a newly selected
  // workspace cannot inherit the previous workspace's pairing token.
  static std::string workspaceScopedKey(const std::filesystem::path& workspaceDirectory,
                                        const std::string& key);
  static CredentialLookup lookupForWorkspace(const std::filesystem::path& workspaceDirectory,
                                             const std::string& key);
  static std::optional<std::string> readForWorkspace(const std::filesystem::path& workspaceDirectory,
                                                     const std::string& key);
  static bool writeForWorkspace(const std::filesystem::path& workspaceDirectory, const std::string& key,
                                const std::string& secret);
  static bool eraseForWorkspace(const std::filesystem::path& workspaceDirectory, const std::string& key);
};

}  // namespace inventatory
