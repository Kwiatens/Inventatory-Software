// Inventatory - Hardware Inventory Management System
// Versioned user-level application settings, stored outside inventory data.

#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "core/PhysicalValue.h"

namespace inventatory {

constexpr int kDefaultLowStockThreshold = 5;

enum class AppearanceColorRole : std::uint8_t {
  CanvasBg,
  SurfaceBg,
  RaisedSurfaceBg,
  HoverBg,
  SelectionBg,
  Divider,
  PrimaryText,
  SecondaryText,
  MutedText,
  FocusText,
  Interactive,
  Success,
  Link,
  WarningText,
  DangerText,
  ActiveBg,
  ActiveSoftBg,
  WarningBg,
  DangerBg,
  DangerFlashBg,
  Count,
};

constexpr std::size_t kAppearanceColorCount = static_cast<std::size_t>(AppearanceColorRole::Count);

struct AppearanceSettings {
  std::array<std::uint32_t, kAppearanceColorCount> colors = {
      0x0D1010,  // CanvasBg
      0x141918,  // SurfaceBg
      0x1D2422,  // RaisedSurfaceBg
      0x27312F,  // HoverBg
      0x2C4440,  // SelectionBg
      0x384543,  // Divider
      0xF1EEE5,  // PrimaryText
      0xCAD0CA,  // SecondaryText
      0x8C9690,  // MutedText
      0xB9E7DD,  // FocusText
      0x58B9B0,  // Interactive
      0xA5C9A5,  // Success
      0x8FCBC5,  // Link
      0xD8B56B,  // WarningText
      0xE08C83,  // DangerText
      0x315A55,  // ActiveBg
      0x243A37,  // ActiveSoftBg
      0x3A3327,  // WarningBg
      0x3E2A29,  // DangerBg
      0x70403B,  // DangerFlashBg
  };
};

const char* appearanceColorKey(AppearanceColorRole role);
const char* appearanceColorLabel(AppearanceColorRole role);
std::string appearanceColorHex(std::uint32_t rgb);
bool parseAppearanceColorHex(const std::string& text, std::uint32_t& rgb);

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
  int lowStockThreshold = kDefaultLowStockThreshold;
  AppearanceSettings appearance;
  // Shared wire-label shortcuts published to the paired Scan R1.
  std::vector<std::string> quickLabelPresets;
  std::uint32_t quickLabelRevision = 1;
  // Tolerances for physical value comparison in search.
  PhysicalValueTolerances physicalValueTolerances;
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
