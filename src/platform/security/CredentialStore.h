// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// User-scoped secret storage abstraction.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace inventatory {

// Unavailable means the store could not answer (locked or missing keyring).
// Never read it as NotFound.
enum class CredentialReadStatus { Found, NotFound, Unavailable };

struct CredentialLookup {
  CredentialReadStatus status = CredentialReadStatus::NotFound;
  std::optional<std::string> secret;
};

// For display. If the store could not answer, returns `previous` rather than "not configured".
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

  // Scanner credentials are scoped to the workspace: a non-secret digest of its normalized path is
  // part of the key, so a new workspace cannot inherit the previous pairing token.
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
