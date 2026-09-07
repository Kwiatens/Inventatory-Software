// Inventatory - Inventory export and data-folder backup helpers.

#pragma once

#include "core/Inventory.h"

#include <filesystem>
#include <functional>
#include <string>

namespace inventatory {

struct AppSettings;

bool exportInventoryCsv(const InventoryStore& store, const std::filesystem::path& path, std::string& error);

// Optional operation overrides are test-only seams for deterministic
// failure-path tests. Production callers must pass nullptr; integrity checks
// and SQLite snapshot validation are never delegated to these callbacks.
struct InventoryTransferTestHooks {
  std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> copyFile;
  std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> renamePath;
  std::function<bool(const std::filesystem::path&, std::string&)> removeAll;
  std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> replaceFile;
  std::function<bool(const std::filesystem::path&, const AppSettings&, std::string&)> saveSettings;
};

// Creates and validates a self-contained backup bundle. Secrets are not part
// of the bundle; scanner pairing is deliberately re-established on restore.
bool createInventatoryBackup(const std::filesystem::path& dataDirectory,
                             const std::filesystem::path& appSettingsPath,
                             const std::filesystem::path& destinationDirectory,
                             const std::string& applicationVersion, std::string& error,
                             const InventoryTransferTestHooks* testHooks = nullptr);
bool validateInventatoryBackup(const std::filesystem::path& backupDirectory, std::string& error);
bool restoreInventatoryBackup(const std::filesystem::path& backupDirectory,
                             const std::filesystem::path& destinationDirectory,
                             const std::filesystem::path& appSettingsPath, std::string& error,
                             const InventoryTransferTestHooks* testHooks = nullptr,
                             // Set only when the function returns false after
                             // publishing the replacement workspace and the
                             // rollback/cleanup could not make the outcome
                             // unambiguous. Callers must keep services stopped
                             // until the active workspace has been reloaded.
                             bool* replacementWorkspaceActiveOnFailure = nullptr);

// Called by startup before loading the active workspace. It completes a
// committed restore cleanup or rolls back an interrupted activation using the
// journal written next to appSettingsPath. The journal contains no secrets.
bool recoverInventatoryRestore(const std::filesystem::path& destinationDirectory,
                               const std::filesystem::path& appSettingsPath, std::string& error);

}  // namespace inventatory
