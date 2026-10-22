// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "label_printer/symbols/RackSymbols.h"

#include <cstdint>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

constexpr int kDefaultLowStockThreshold = 5;

// Byte limits shared by the Settings editor and the validator, so accepted edits always save.
inline constexpr std::size_t kMaxDigiKeyFieldBytes = 512U;
inline constexpr std::size_t kMaxDigiKeyLocaleFieldBytes = 32U;
// TME byte limits, shared with the editor like the DigiKey limits.
inline constexpr std::size_t kMaxTmeFieldBytes = 512U;
inline constexpr std::size_t kMaxTmeLocaleFieldBytes = 32U;

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
      0xE5EDEC,  // PrimaryText
      0xBFCAC9,  // SecondaryText
      0x8C9690,  // MutedText
      0xB9E7DD,  // FocusText
      0x58B9B0,  // Interactive
      0xA0D0C0,  // Success
      0x8FCBC5,  // Link
      0xDBCDA9,  // WarningText
      0xCC8BA4,  // DangerText
      0x315A55,  // ActiveBg
      0x243A37,  // ActiveSoftBg
      0x2A261C,  // WarningBg
      0x322027,  // DangerBg
      0x583744,  // DangerFlashBg
  };
};

const char* appearanceColorKey(AppearanceColorRole role);
const char* appearanceColorLabel(AppearanceColorRole role);
std::string appearanceColorHex(std::uint32_t rgb);
bool parseAppearanceColorHex(const std::string& text, std::uint32_t& rgb);
// Moves unchanged legacy default colors to the current ones; customized colors stay.
// Returns true if any color changed.
bool upgradeLegacyAppearanceDefaults(AppearanceSettings& appearance);

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
  // Storefront for TME price data. The token and secret live in the credential store, not here.
  std::string tmeCountry = "PL";
  std::string tmeLanguage = "EN";
  std::string tmeCurrency = "EUR";
  int lowStockThreshold = kDefaultLowStockThreshold;
  // Written to settings.conf only when not the default, so older releases can still read the file.
  SymbolStandard symbolStandard = SymbolStandard::Eu;
  AppearanceSettings appearance;
  // Shared wire-label shortcuts published to the paired Scan R1.
  std::vector<std::string> quickLabelPresets;
  std::uint32_t quickLabelRevision = 1;
};

std::filesystem::path appSettingsDirectory();
std::filesystem::path appSettingsPath();
bool loadAppSettings(const std::filesystem::path& path, AppSettings& settings);
bool saveAppSettings(const std::filesystem::path& path, const AppSettings& settings);

// Quick label presets live with the data folder, so backups and moves carry them.
std::filesystem::path quickLabelsPath(const std::filesystem::path& dataDirectory);
bool loadQuickLabels(const std::filesystem::path& path, std::vector<std::string>& presets, std::uint32_t& revision);
bool saveQuickLabels(const std::filesystem::path& path, const std::vector<std::string>& presets,
                     std::uint32_t revision);

}  // namespace inventatory
