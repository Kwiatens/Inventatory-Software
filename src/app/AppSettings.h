// HIMS - Hardware Inventory Management System
// Versioned user-level application settings, stored outside inventory data.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace hims {

struct AppSettings {
  int schemaVersion = 1;
  std::filesystem::path dataDirectory;
  std::string printerQueue;
  bool autoPrintScannedLabels = true;
  std::uint16_t deviceServicePort = 8080;
  std::string digiKeyClientId;
  std::string digiKeyAccountId;
  std::string digiKeySite = "US";
  std::string digiKeyLanguage = "en";
  std::string digiKeyCurrency = "USD";
};

std::filesystem::path appSettingsDirectory();
std::filesystem::path appSettingsPath();
bool loadAppSettings(const std::filesystem::path& path, AppSettings& settings);
bool saveAppSettings(const std::filesystem::path& path, const AppSettings& settings);

}  // namespace hims
