// Inventatory - Hardware Inventory Management System
// Data-path bootstrap helpers shared by startup and safe directory switching.

#pragma once

#include <filesystem>
#include <functional>

namespace inventatory {

struct InventatoryDataPaths {
  std::filesystem::path dataDirectory;
  std::filesystem::path inventory;
  std::filesystem::path printer;
  std::filesystem::path activity;
  std::filesystem::path scanConfig;
};

InventatoryDataPaths makeInventatoryDataPaths(const std::filesystem::path& dataDirectory);
bool switchInventatoryDataPathsAfterSaving(InventatoryDataPaths& active,
                                           const std::filesystem::path& nextDataDirectory,
                                           const std::function<bool()>& saveCurrentState);

}  // namespace inventatory
