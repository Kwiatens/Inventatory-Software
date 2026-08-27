// Inventatory - Inventory export and data-folder backup helpers.

#pragma once

#include "core/Inventory.h"

#include <filesystem>
#include <string>

namespace inventatory {

bool exportInventoryCsv(const InventoryStore& store, const std::filesystem::path& path, std::string& error);
bool backupInventatoryData(const std::filesystem::path& sourceDirectory,
                           const std::filesystem::path& destinationDirectory, std::string& error);

}  // namespace inventatory
