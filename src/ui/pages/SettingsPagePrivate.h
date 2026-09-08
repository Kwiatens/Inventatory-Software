// Inventatory - Private Settings page helper contracts.

#pragma once

#include "App.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace inventatory {
namespace settings_page_detail {

int settingsLabelWidth(int width);
ftxui::Element settingLine(const std::string& label, const std::string& value, int width, bool selected = false);
ftxui::Element versionLine(const std::string& label, const std::string& installedVersion,
                           const std::string& availableVersion, int width);
ftxui::Element printerQueueLine(const std::string& name, const std::string& status, int width, bool selected);
ftxui::Element buttonRow(ftxui::Element button);
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
