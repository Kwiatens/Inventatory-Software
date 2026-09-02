// Inventatory - Inventory export and data-folder backup helpers.

#pragma once

#include "core/Inventory.h"

#include <filesystem>
#include <string>

namespace inventatory {

bool exportInventoryCsv(const InventoryStore& store, const std::filesystem::path& path, std::string& error);
bool backupInventatoryData(const std::filesystem::path& sourceDirectory,
                           const std::filesystem::path& destinationDirectory, std::string& error);

// Creates and validates a self-contained backup bundle. Secrets are not part
// of the bundle; scanner pairing is deliberately re-established on restore.
bool createInventatoryBackup(const std::filesystem::path& dataDirectory,
                             const std::filesystem::path& appSettingsPath,
                             const std::filesystem::path& destinationDirectory,
                             const std::string& applicationVersion, std::string& error);
bool validateInventatoryBackup(const std::filesystem::path& backupDirectory, std::string& error);
bool restoreInventatoryBackup(const std::filesystem::path& backupDirectory,
                             const std::filesystem::path& destinationDirectory,
                             const std::filesystem::path& appSettingsPath, std::string& error);

}  // namespace inventatory
