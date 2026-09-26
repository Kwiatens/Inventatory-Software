// Inventatory - Private Settings page helper contracts.

#pragma once

#include "App.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace inventatory {
namespace settings_page_detail {

struct SettingsCategoryEntry {
  int categoryIndex = 0;
  int indent = 0;
};

inline constexpr std::array<SettingsCategoryEntry, 7> settingsCategoryEntries() {
  return {{{0, 0},  // General / Data
           {1, 0},  // Appearance
           {2, 0},  // Updates
           {3, 0},  // Printer
           {4, 1},  // Quick Labels under Printer
           {5, 0},  // Inventatory Scan
           {6, 0}}}; // DigiKey
}

// Standard Settings panel anatomy. Every category panel is a vertical list of
// sections separated by one blank line:
//
//   Section title                                   optional meta
//    >  Label                      Value
//       Label                      Value
//       [Action]  [Action]
//
// Rows share one three-column marker gutter (" > " when selected), one label
// column, and one value column, so fields, toggles, status rows, and list rows
// line up across every category. Section actions always come last in their
// section and align with the labels.
inline constexpr int kSettingsGutterWidth = 3;

int settingsLabelWidth(int width);
// Editable or read-only value row. `editing` shows the value as primary text.
ftxui::Element settingLine(const std::string& label, const std::string& value, int width, bool selected = false);
// On/Off setting row; the value is colored so state is scannable.
ftxui::Element settingToggleLine(const std::string& label, bool enabled, int width, bool selected = false);
// Read-only state row with a semantic value color (Online, Not configured, ...).
ftxui::Element settingStatusLine(const std::string& label, const std::string& value, ftxui::Color valueColor,
                                 int width);
// Selectable list row: name in the label/value area, status right-aligned.
ftxui::Element settingListLine(const std::string& name, const std::string& status, ftxui::Color statusColor,
                               int width, bool selected);
// Muted, gutter-aligned line for an empty list or a status note.
ftxui::Element settingNoteLine(const std::string& text, ftxui::Color color, int width);
ftxui::Element settingsSectionHeader(const std::string& title, ftxui::Element meta = nullptr);
// Buttons laid out with the standard spacing, aligned under the labels.
ftxui::Element settingsActionRow(ftxui::Elements buttons);
// Appends one section (header + rows) with the standard blank-line separation.
void appendSettingsSection(ftxui::Elements& panel, const std::string& title, ftxui::Elements rows,
                           ftxui::Element meta = nullptr);
ftxui::Element appearanceColorLine(AppearanceColorRole role, int width, bool selected);

inline constexpr const char* kDigiKeySecretName = "digikey-client-secret";
inline constexpr int kAppearancePickerHueSteps = 12;
inline constexpr int kAppearancePickerValueSteps = 6;

inline uint32_t hsvToRgb(double hue, double saturation, double value) {
  hue = std::fmod(hue, 360.0);
  if (hue < 0.0) hue += 360.0;
  const double chroma = value * saturation;
  const double segment = hue / 60.0;
  const double intermediate = chroma * (1.0 - std::abs(std::fmod(segment, 2.0) - 1.0));
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  if (segment < 1.0) {
    red = chroma;
    green = intermediate;
  } else if (segment < 2.0) {
    red = intermediate;
    green = chroma;
  } else if (segment < 3.0) {
    green = chroma;
    blue = intermediate;
  } else if (segment < 4.0) {
    green = intermediate;
    blue = chroma;
  } else if (segment < 5.0) {
    red = intermediate;
    blue = chroma;
  } else {
    red = chroma;
    blue = intermediate;
  }
  const double match = value - chroma;
  const auto channel = [match](double component) {
    return static_cast<uint32_t>(std::round((component + match) * 255.0));
  };
  return (channel(red) << 16) | (channel(green) << 8) | channel(blue);
}

struct HsvCoordinates {
  int hue = 0;
  int value = 0;
};

inline HsvCoordinates pickerCoordinates(uint32_t rgb) {
  const double red = static_cast<double>((rgb >> 16) & 0xFFu) / 255.0;
  const double green = static_cast<double>((rgb >> 8) & 0xFFu) / 255.0;
  const double blue = static_cast<double>(rgb & 0xFFu) / 255.0;
  const double maximum = std::max({red, green, blue});
  const double minimum = std::min({red, green, blue});
  const double delta = maximum - minimum;

  double hue = 0.0;
  if (delta > 0.0001) {
    if (maximum == red) {
      hue = 60.0 * std::fmod((green - blue) / delta, 6.0);
    } else if (maximum == green) {
      hue = 60.0 * ((blue - red) / delta + 2.0);
    } else {
      hue = 60.0 * ((red - green) / delta + 4.0);
    }
    if (hue < 0.0) hue += 360.0;
  }
  const int hueIndex = static_cast<int>(std::round(hue / 360.0 * kAppearancePickerHueSteps)) %
                       kAppearancePickerHueSteps;
  const int valueIndex = static_cast<int>(std::round((maximum * kAppearancePickerValueSteps) - 1.0));
  return {hueIndex, std::clamp(valueIndex, 0, kAppearancePickerValueSteps - 1)};
}

inline double pickerValue(int row) {
  static constexpr double values[kAppearancePickerValueSteps] = {0.30, 0.44, 0.58, 0.72, 0.86, 1.0};
  return values[std::clamp(row, 0, kAppearancePickerValueSteps - 1)];
}

inline uint32_t pickerColor(int hue, int value) {
  const auto normalizedHue = (hue + kAppearancePickerHueSteps) % kAppearancePickerHueSteps;
  return hsvToRgb(static_cast<double>(normalizedHue) * 360.0 / kAppearancePickerHueSteps, 0.82,
                  pickerValue(value));
}

}  // namespace settings_page_detail
}  // namespace inventatory
