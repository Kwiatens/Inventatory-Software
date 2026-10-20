// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Inventory export and data-folder backup helpers.

#pragma once

#include "core/inventory/Inventory.h"

#include <filesystem>
#include <functional>
#include <string>

namespace inventatory {

struct AppSettings;

bool exportInventoryCsv(const InventoryStore& store, const std::filesystem::path& path, std::string& error);

// Test-only seams. Production callers pass nullptr; integrity checks are never delegated here.
struct InventoryTransferTestHooks {
  std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> copyFile;
  std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> renamePath;
  std::function<bool(const std::filesystem::path&, std::string&)> removeAll;
  std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> replaceFile;
  std::function<bool(const std::filesystem::path&, const AppSettings&, std::string&)> saveSettings;
};

// Secrets are not included in the bundle; scanner pairing is re-established on restore.
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
                             // Set when false is returned after publishing the replacement and
                             // the outcome is unclear. Keep services stopped until reload.
                             bool* replacementWorkspaceActiveOnFailure = nullptr);

// Run at startup before loading the workspace: completes a committed restore or rolls back
// an interrupted one. The journal next to appSettingsPath holds no secrets.
bool recoverInventatoryRestore(const std::filesystem::path& destinationDirectory,
                               const std::filesystem::path& appSettingsPath, std::string& error);

}  // namespace inventatory
