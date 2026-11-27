// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Appearance settings formatting and parsing.

#include "core/settings/AppSettings.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace inventatory {

using namespace std;

namespace {

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

}  // namespace

const char* appearanceColorKey(AppearanceColorRole role) {
  switch (role) {
    case AppearanceColorRole::CanvasBg: return "canvas_bg";
    case AppearanceColorRole::SurfaceBg: return "surface_bg";
    case AppearanceColorRole::RaisedSurfaceBg: return "raised_surface_bg";
    case AppearanceColorRole::HoverBg: return "hover_bg";
    case AppearanceColorRole::SelectionBg: return "selection_bg";
    case AppearanceColorRole::Divider: return "divider";
    case AppearanceColorRole::PrimaryText: return "primary_text";
    case AppearanceColorRole::SecondaryText: return "secondary_text";
    case AppearanceColorRole::MutedText: return "muted_text";
    case AppearanceColorRole::FocusText: return "focus_text";
    case AppearanceColorRole::Interactive: return "interactive";
    case AppearanceColorRole::Success: return "success";
    case AppearanceColorRole::Link: return "link";
    case AppearanceColorRole::WarningText: return "warning_text";
    case AppearanceColorRole::DangerText: return "danger_text";
    case AppearanceColorRole::ActiveBg: return "active_bg";
    case AppearanceColorRole::ActiveSoftBg: return "active_soft_bg";
    case AppearanceColorRole::WarningBg: return "warning_bg";
    case AppearanceColorRole::DangerBg: return "danger_bg";
    case AppearanceColorRole::DangerFlashBg: return "danger_flash_bg";
    case AppearanceColorRole::Count: break;
  }
  return "unknown";
}

const char* appearanceColorLabel(AppearanceColorRole role) {
  switch (role) {
    case AppearanceColorRole::CanvasBg: return "Canvas background";
    case AppearanceColorRole::SurfaceBg: return "Surface background";
    case AppearanceColorRole::RaisedSurfaceBg: return "Raised surface";
    case AppearanceColorRole::HoverBg: return "Hover background";
    case AppearanceColorRole::SelectionBg: return "Selection background";
    case AppearanceColorRole::Divider: return "Divider";
    case AppearanceColorRole::PrimaryText: return "Primary text";
    case AppearanceColorRole::SecondaryText: return "Secondary text";
    case AppearanceColorRole::MutedText: return "Muted text";
    case AppearanceColorRole::FocusText: return "Focus text";
    case AppearanceColorRole::Interactive: return "Interactive / accent";
    case AppearanceColorRole::Success: return "Success";
    case AppearanceColorRole::Link: return "Link";
    case AppearanceColorRole::WarningText: return "Warning text";
    case AppearanceColorRole::DangerText: return "Danger text";
    case AppearanceColorRole::ActiveBg: return "Active background";
    case AppearanceColorRole::ActiveSoftBg: return "Active soft background";
    case AppearanceColorRole::WarningBg: return "Warning background";
    case AppearanceColorRole::DangerBg: return "Danger background";
    case AppearanceColorRole::DangerFlashBg: return "Danger flash background";
    case AppearanceColorRole::Count: break;
  }
  return "Unknown";
}

string appearanceColorHex(uint32_t rgb) {
  ostringstream output;
  output << '#' << uppercase << hex << setw(6) << setfill('0') << (rgb & 0xFFFFFFu);
  return output.str();
}

bool parseAppearanceColorHex(const string& text, uint32_t& rgb) {
  string value;
  for (const char character : text) {
    if (!isspace(static_cast<unsigned char>(character))) value.push_back(character);
  }
  if (!value.empty() && value.front() == '#') value.erase(value.begin());
  if (value.size() != 6) return false;

  uint32_t parsed = 0;
  for (const char character : value) {
    const int digit = hexDigit(character);
    if (digit < 0) return false;
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
  }
  rgb = parsed;
  return true;
}

const char* appearanceThemeKey(AppearanceTheme theme) {
  switch (theme) {
    case AppearanceTheme::Light: return "light";
    case AppearanceTheme::Dark: return "dark";
    case AppearanceTheme::Custom: return "custom";
  }
  return "light";
}

const char* appearanceThemeLabel(AppearanceTheme theme) {
  switch (theme) {
    case AppearanceTheme::Light: return "Light";
    case AppearanceTheme::Dark: return "Dark";
    case AppearanceTheme::Custom: return "Custom";
  }
  return "Light";
}

bool parseAppearanceTheme(const string& text, AppearanceTheme& theme) {
  for (const auto candidate : {AppearanceTheme::Light, AppearanceTheme::Dark, AppearanceTheme::Custom}) {
    if (text == appearanceThemeKey(candidate)) {
      theme = candidate;
      return true;
    }
  }
  return false;
}

const AppearancePalette& appearanceThemePalette(AppearanceTheme theme) {
  return theme == AppearanceTheme::Dark ? kDarkAppearancePalette : kLightAppearancePalette;
}

void selectAppearanceTheme(AppearanceSettings& appearance, AppearanceTheme theme) {
  appearance.theme = theme;
  if (theme != AppearanceTheme::Custom) appearance.colors = appearanceThemePalette(theme);
}

AppearanceTheme appearanceBaseTheme(const AppearanceSettings& appearance) {
  if (appearance.theme != AppearanceTheme::Custom) return appearance.theme;
  const auto canvas = appearance.colors[static_cast<size_t>(AppearanceColorRole::CanvasBg)];
  const auto luma = 0.2126 * ((canvas >> 16) & 0xFFu) + 0.7152 * ((canvas >> 8) & 0xFFu) + 0.0722 * (canvas & 0xFFu);
  return luma >= 128.0 ? AppearanceTheme::Light : AppearanceTheme::Dark;
}

bool upgradeLegacyAppearanceDefaults(AppearanceSettings& appearance) {
  struct Upgrade {
    AppearanceColorRole role;
    uint32_t legacy;
  };
  // Earlier shipped Slate values. A color still at one of them moves to the last Slate value.
  static const Upgrade kUpgrades[] = {
      {AppearanceColorRole::WarningText, 0xD8B56B}, {AppearanceColorRole::DangerText, 0xE08C83},
      {AppearanceColorRole::WarningBg, 0x3A3327},   {AppearanceColorRole::DangerBg, 0x3E2A29},
      {AppearanceColorRole::DangerFlashBg, 0x70403B},
      {AppearanceColorRole::PrimaryText, 0xF1EEE5}, {AppearanceColorRole::SecondaryText, 0xCAD0CA},
      {AppearanceColorRole::Success, 0xA5C9A5},     {AppearanceColorRole::WarningText, 0xE5A77C},
      {AppearanceColorRole::DangerText, 0xE4707C},  {AppearanceColorRole::WarningBg, 0x3B2F28},
      {AppearanceColorRole::DangerBg, 0x43262B},    {AppearanceColorRole::DangerFlashBg, 0x74404B},
  };
  bool changed = false;
  for (const auto& upgrade : kUpgrades) {
    const auto index = static_cast<size_t>(upgrade.role);
    if (appearance.colors[index] != upgrade.legacy) continue;
    appearance.colors[index] = kSlateAppearancePalette[index];
    changed = true;
  }
  return changed;
}

void migrateAppearanceWithoutTheme(AppearanceSettings& appearance) {
  upgradeLegacyAppearanceDefaults(appearance);
  if (appearance.colors == kSlateAppearancePalette || appearance.colors == kDarkAppearancePalette) {
    selectAppearanceTheme(appearance, AppearanceTheme::Dark);
  } else if (appearance.colors == kLightAppearancePalette) {
    selectAppearanceTheme(appearance, AppearanceTheme::Light);
  } else {
    appearance.theme = AppearanceTheme::Custom;
  }
}

}  // namespace inventatory
