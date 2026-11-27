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

using AppearancePalette = std::array<std::uint32_t, kAppearanceColorCount>;

// Light and Dark are the built-in palettes. Custom keeps the colours the user picked.
enum class AppearanceTheme : std::uint8_t { Light, Dark, Custom };

// Light: a blue-grey page in the website's blues, cyan as the one accent.
inline constexpr AppearancePalette kLightAppearancePalette = {
    0xC5CDD4,  // CanvasBg
    0xC5CDD4,  // SurfaceBg
    0xB6C0C9,  // RaisedSurfaceBg
    0xACB7C1,  // HoverBg
    0x97C6D6,  // SelectionBg
    0xA3AEB8,  // Divider
    0x1A2530,  // PrimaryText
    0x26323E,  // SecondaryText
    0x4A5765,  // MutedText
    0x123A4F,  // FocusText
    0x236E92,  // Interactive
    0x245F48,  // Success
    0x234B64,  // Link
    0x6A4C0E,  // WarningText
    0x8C2C3E,  // DangerText
    0x4FA6C2,  // ActiveBg
    0xB3BEC8,  // ActiveSoftBg
    0xD4CAB2,  // WarningBg
    0xD0BCC0,  // DangerBg
    0xC79EA6,  // DangerFlashBg
};

// Dark: the website's night sections, navy and slate with the same cyan accent.
inline constexpr AppearancePalette kDarkAppearancePalette = {
    0x1F2A35,  // CanvasBg
    0x1F2A35,  // SurfaceBg
    0x2C3946,  // RaisedSurfaceBg
    0x34424F,  // HoverBg
    0x2E5C78,  // SelectionBg
    0x3A4856,  // Divider
    0xE8EEF2,  // PrimaryText
    0xC9D3DC,  // SecondaryText
    0x9FB0BF,  // MutedText
    0xBDEBF7,  // FocusText
    0x5CC8E6,  // Interactive
    0x8FCFAF,  // Success
    0x8CC4E0,  // Link
    0xE3C27A,  // WarningText
    0xEE8FA0,  // DangerText
    0x2E5C78,  // ActiveBg
    0x283541,  // ActiveSoftBg
    0x3A3527,  // WarningBg
    0x432C34,  // DangerBg
    0x6A3A47,  // DangerFlashBg
};

// The dark green palette shipped before the themes. A settings file still holding it becomes Dark.
inline constexpr AppearancePalette kSlateAppearancePalette = {
    0x0D1010, 0x141918, 0x1D2422, 0x27312F, 0x2C4440, 0x384543, 0xE5EDEC, 0xBFCAC9, 0x8C9690, 0xB9E7DD,
    0x58B9B0, 0xA0D0C0, 0x8FCBC5, 0xDBCDA9, 0xCC8BA4, 0x315A55, 0x243A37, 0x2A261C, 0x322027, 0x583744,
};

struct AppearanceSettings {
  AppearanceTheme theme = AppearanceTheme::Light;
  AppearancePalette colors = kLightAppearancePalette;
};

inline bool operator==(const AppearanceSettings& a, const AppearanceSettings& b) {
  return a.theme == b.theme && a.colors == b.colors;
}
inline bool operator!=(const AppearanceSettings& a, const AppearanceSettings& b) { return !(a == b); }

const char* appearanceColorKey(AppearanceColorRole role);
const char* appearanceColorLabel(AppearanceColorRole role);
std::string appearanceColorHex(std::uint32_t rgb);
bool parseAppearanceColorHex(const std::string& text, std::uint32_t& rgb);
const char* appearanceThemeKey(AppearanceTheme theme);
const char* appearanceThemeLabel(AppearanceTheme theme);
bool parseAppearanceTheme(const std::string& text, AppearanceTheme& theme);
// The palette of a built-in theme; Custom has none and returns the Light palette.
const AppearancePalette& appearanceThemePalette(AppearanceTheme theme);
// Switches to a built-in theme and loads its colours. Custom only changes the theme.
void selectAppearanceTheme(AppearanceSettings& appearance, AppearanceTheme theme);
// The built-in theme a custom palette is closest to, by its canvas brightness. Used to reset one colour.
AppearanceTheme appearanceBaseTheme(const AppearanceSettings& appearance);
// Moves colours still at an earlier shipped Slate value to the Slate palette; customized colors stay.
// Returns true if any color changed.
bool upgradeLegacyAppearanceDefaults(AppearanceSettings& appearance);
// For a settings file written before themes: Slate becomes Dark, a built-in palette its theme, anything else Custom.
void migrateAppearanceWithoutTheme(AppearanceSettings& appearance);

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
  // A vendor that is off is skipped for price checks and refreshes; its keys stay stored. Written only when
  // false, so older releases can still read the file.
  bool digiKeyEnabled = true;
  bool tmeEnabled = true;
  bool mouserEnabled = true;
  int lowStockThreshold = kDefaultLowStockThreshold;
  // Written to settings.conf only when not the default, so older releases can still read the file.
  SymbolStandard symbolStandard = SymbolStandard::Eu;
  AppearanceSettings appearance;
  // Shared wire-label shortcuts published to the paired Scan R1.
  std::vector<std::string> quickLabelPresets;
  std::uint32_t quickLabelRevision = 1;
};

// The one currency every vendor is asked to price in. It is stored in the DigiKey and TME currency
// keys, kept equal, so older releases still read the file. Empty when they differ, which happens
// only in files written before the currency was shared.
std::string sharedVendorCurrency(const AppSettings& settings);
// Sets both keys. The caller checks isSupportedVendorCurrency first.
void setSharedVendorCurrency(AppSettings& settings, const std::string& currency);
// The one region every vendor is asked about: the DigiKey site and the TME country, kept equal. Empty when
// they differ, which happens only in files written before the region was shared.
std::string sharedVendorRegion(const AppSettings& settings);
// Sets both. The caller checks isValidVendorRegion first.
void setSharedVendorRegion(AppSettings& settings, const std::string& region);
// A two-letter upper-case country code.
bool isValidVendorRegion(const std::string& region);
// Currencies offered in setup and Settings: ones the vendors answer in.
const std::vector<std::string>& supportedVendorCurrencies();
bool isSupportedVendorCurrency(const std::string& currency);

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
