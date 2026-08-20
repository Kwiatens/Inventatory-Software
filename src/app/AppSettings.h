// Inventatory - Hardware Inventory Management System
// Versioned user-level application settings, stored outside inventory data.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

struct AppSettings {
  int schemaVersion = 1;
  int completedOnboardingVersion = 0;
  std::filesystem::path dataDirectory;
  std::string printerQueue;
  bool autoPrintScannedLabels = true;
  bool backgroundServiceEnabled = false;
  bool backgroundConsentAsked = false;
  bool updateChecksEnabled = true;
  std::int64_t lastUpdateCheckUnixSeconds = 0;
  std::string latestAvailableVersion;
  std::string latestReleaseUrl;
  std::uint16_t deviceServicePort = 8080;
  std::string digiKeyClientId;
  std::string digiKeyAccountId;
  std::string digiKeySite = "US";
  std::string digiKeyLanguage = "en";
  std::string digiKeyCurrency = "USD";
  // Shared wire-label shortcuts published to the paired Scan R1.
  std::vector<std::string> quickLabelPresets;
  std::uint32_t quickLabelRevision = 1;
};

std::filesystem::path appSettingsDirectory();
std::filesystem::path appSettingsPath();
bool loadAppSettings(const std::filesystem::path& path, AppSettings& settings);
bool saveAppSettings(const std::filesystem::path& path, const AppSettings& settings);

// Quick label presets travel with the Inventatory data folder (so they follow
// the user's data when it is backed up or moved) rather than living in the
// machine-local settings file.
std::filesystem::path quickLabelsPath(const std::filesystem::path& dataDirectory);
bool loadQuickLabels(const std::filesystem::path& path, std::vector<std::string>& presets, std::uint32_t& revision);
bool saveQuickLabels(const std::filesystem::path& path, const std::vector<std::string>& presets,
                     std::uint32_t revision);

}  // namespace inventatory
