// Inventatory - Hardware Inventory Management System
// Shared terminal UI formatting and inventory detail helpers.

#include "ui/shared/AppUiShared.h"

#include "core/parts/PartDescriptor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>

namespace inventatory {

using namespace std;

namespace {

AppearanceSettings g_activeAppearance;

ftxui::Color colorFromRgb(uint32_t rgb) {
  return ftxui::Color::RGB(static_cast<uint8_t>((rgb >> 16) & 0xFFu),
                           static_cast<uint8_t>((rgb >> 8) & 0xFFu),
                           static_cast<uint8_t>(rgb & 0xFFu));
}

}  // namespace

void applyUiAppearance(const AppearanceSettings& appearance) {
  g_activeAppearance = appearance;
}

const AppearanceSettings& activeUiAppearance() {
  return g_activeAppearance;
}

ftxui::Color uiAppearanceColor(AppearanceColorRole role) {
  const auto index = static_cast<size_t>(role);
  if (index >= kAppearanceColorCount) return colorFromRgb(0);
  return colorFromRgb(g_activeAppearance.colors[index]);
}

ftxui::Color uiCanvasBg() {
  return uiAppearanceColor(AppearanceColorRole::CanvasBg);
}

ftxui::Color uiSurfaceBg() {
  return uiAppearanceColor(AppearanceColorRole::SurfaceBg);
}

ftxui::Color uiRaisedSurfaceBg() {
  return uiAppearanceColor(AppearanceColorRole::RaisedSurfaceBg);
}

ftxui::Color uiHoverBg() {
  return uiAppearanceColor(AppearanceColorRole::HoverBg);
}

ftxui::Color uiSelectionBg() {
  return uiAppearanceColor(AppearanceColorRole::SelectionBg);
}

ftxui::Color uiDividerColor() {
  return uiAppearanceColor(AppearanceColorRole::Divider);
}

ftxui::Element uiDivider() {
  return ftxui::separator() | ftxui::color(uiDividerColor());
}

ftxui::Color uiPrimaryText() {
  return uiAppearanceColor(AppearanceColorRole::PrimaryText);
}

ftxui::Color uiSecondaryText() {
  return uiAppearanceColor(AppearanceColorRole::SecondaryText);
}

ftxui::Color uiMutedText() {
  return uiAppearanceColor(AppearanceColorRole::MutedText);
}

ftxui::Color uiInteractiveColor() {
  return uiAppearanceColor(AppearanceColorRole::Interactive);
}

ftxui::Color uiFocusColor() {
  return uiAppearanceColor(AppearanceColorRole::FocusText);
}

ftxui::Color uiTitleColor() {
  return uiPrimaryText();
}

ftxui::Color uiAccentColor() {
  return uiInteractiveColor();
}

ftxui::Color uiInfoColor() {
  return uiPrimaryText();
}

ftxui::Color uiSuccessColor() {
  return uiAppearanceColor(AppearanceColorRole::Success);
}

ftxui::Color uiLinkColor() {
  return uiAppearanceColor(AppearanceColorRole::Link);
}

ftxui::Color uiLabelColor() {
  return uiSecondaryText();
}

ftxui::Color uiWarnColor() {
  return uiAppearanceColor(AppearanceColorRole::WarningText);
}

ftxui::Color uiDangerColor() {
  return uiAppearanceColor(AppearanceColorRole::DangerText);
}

ftxui::Color uiActiveBg() {
  return uiAppearanceColor(AppearanceColorRole::ActiveBg);
}

ftxui::Color uiActiveSoftBg() {
  return uiAppearanceColor(AppearanceColorRole::ActiveSoftBg);
}

ftxui::Color uiWarningBg() {
  return uiAppearanceColor(AppearanceColorRole::WarningBg);
}

ftxui::Color uiDangerBg() {
  return uiAppearanceColor(AppearanceColorRole::DangerBg);
}

ftxui::Color uiDangerFlashBg() {
  return uiAppearanceColor(AppearanceColorRole::DangerFlashBg);
}

ftxui::Color uiMutedColor() {
  return uiMutedText();
}

ftxui::Color uiDimColor() {
  return uiDividerColor();
}

ftxui::Color uiPanelLeftBg() {
  return uiSurfaceBg();
}

ftxui::Color uiPanelRightBg() {
  return uiSurfaceBg();
}

ftxui::Color uiRowDarkBg() {
  return uiSurfaceBg();
}

ftxui::Color uiRowLightBg() {
  return uiSurfaceBg();
}

ftxui::Color uiRowSelectedBg() {
  return uiSelectionBg();
}

ftxui::Element styledText(const string& text, optional<ftxui::Color> fg,
                          optional<ftxui::Color> bg) {
  auto element = ftxui::text(text);
  if (fg) {
    element = element | ftxui::color(*fg);
  }
  if (bg) {
    element = element | ftxui::bgcolor(*bg);
  }
  return element;
}

ftxui::Element uiHeaderText(const string& text, optional<ftxui::Color> fg,
                            optional<ftxui::Color> bg) {
  return styledText(text, fg, bg) | ftxui::bold;
}

ftxui::Element uiBodyText(const string& text, optional<ftxui::Color> fg,
                          optional<ftxui::Color> bg) {
  return styledText(text, fg, bg);
}

ftxui::Element uiSectionHeader(const string& text, optional<ftxui::Color> fg,
                               optional<ftxui::Color> bg) {
  const auto fill = bg.value_or(uiSurfaceBg());
  return ftxui::hbox({uiHeaderText(text, fg, fill), ftxui::filler()}) |
         ftxui::bgcolor(fill);
}

ftxui::Element fullLine(const string& text, optional<ftxui::Color> fg,
                        optional<ftxui::Color> bg) {
  auto element = ftxui::hbox({ftxui::text(text), ftxui::filler()});
  if (fg) {
    element = element | ftxui::color(*fg);
  }
  if (bg) {
    element = element | ftxui::bgcolor(*bg);
  }
  return element;
}

ftxui::Element bulletLine(const string& label, const string& value, ftxui::Color labelColor,
                          ftxui::Color valueColor) {
  return ftxui::hbox({
      styledText(label, labelColor),
      styledText(value, valueColor),
      ftxui::filler(),
  });
}

ftxui::Element panel(const string& title, ftxui::Elements body, optional<ftxui::Color> titleColor,
                     optional<ftxui::Color> borderColor) {
  (void)borderColor;
  ftxui::Elements content;
  content.push_back(uiSectionHeader(title, titleColor.value_or(uiSecondaryText()), uiSurfaceBg()));
  content.push_back(uiDivider());
  for (auto& row : body) content.push_back(move(row));
  return ftxui::vbox(move(content)) | ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element footerField(const string& title, const string& body, ftxui::Color titleColor, ftxui::Color bodyColor,
                           ftxui::Color background, bool flashing) {
  const auto fill = flashing ? uiWarningBg() : background;
  return ftxui::hbox({
             uiHeaderText(" " + title + ": ", titleColor, fill),
             uiBodyText(body, bodyColor, fill),
             ftxui::filler(),
         }) |
         ftxui::bgcolor(fill);
}

ftxui::Element uiPrimaryButton(const string& label, bool enabled) {
  // Filled petrol-cyan on canvas-dark text. The inversion is what separates a
  // primary control from the surrounding label/value lines at a glance.
  if (!enabled) return styledText("  " + label + "  ", uiMutedText(), uiRaisedSurfaceBg());
  return uiHeaderText("  " + label + "  ", uiCanvasBg(), uiInteractiveColor());
}

ftxui::Element uiSecondaryButton(const string& label, optional<ftxui::Color> fg, bool enabled) {
  return styledText(" " + label + " ", enabled ? fg.value_or(uiInteractiveColor()) : uiMutedText(),
                    uiRaisedSurfaceBg());
}

ftxui::Element statusTextBox(const string& label, bool active) {
  auto box = styledText(" " + label + " ", active ? uiFocusColor() : uiMutedColor(),
                        active ? uiActiveSoftBg() : uiSurfaceBg());
  if (active) box = uiHeaderText(" " + label + " ", uiFocusColor(), uiActiveSoftBg());
  return box;
}

bool uiBoxContains(const ftxui::Box& box, int x, int y) {
  return x >= box.x_min && x <= box.x_max && y >= box.y_min && y <= box.y_max;
}

ftxui::Element quantityBadge(int quantity, int lowStockThreshold, bool selected) {
  InventoryItem item;
  item.quantity = quantity;
  const auto fg = quantity <= 0 ? uiDangerColor()
                                : (isLowStock(item, lowStockThreshold) ? uiWarnColor() : uiSuccessColor());
  const auto bg = selected ? uiRowSelectedBg()
                           : (quantity <= 0 ? uiDangerBg()
                                            : (isLowStock(item, lowStockThreshold) ? uiWarningBg()
                                                             : uiRaisedSurfaceBg()));
  auto badge = uiBodyText(" " + to_string(quantity) + " ", fg, bg);
  if (selected) badge = badge | ftxui::bold;
  return badge;
}

long long uiAnimationTicks() {
  static const auto start = chrono::steady_clock::now();
  return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
}

bool uiBlinkOn(int periodMs) {
  const auto period = max(1, periodMs);
  return (uiAnimationTicks() / period) % 2 == 0;
}

string uiLoadingSpinner(int intervalMs) {
  const auto interval = max(1, intervalMs);
  static constexpr char kFrames[] = "/-\\|";
  const auto phase = static_cast<size_t>((uiAnimationTicks() / interval) % (sizeof(kFrames) - 1));
  return string(1, kFrames[phase]);
}

namespace {

// Repeats a full block so bars render identically in every terminal font.
string blockRun(int cells) {
  string run;
  for (int index = 0; index < max(0, cells); ++index) {
    run += "\xE2\x96\x88";  // U+2588 FULL BLOCK
  }
  return run;
}

string shadeRun(int cells) {
  string run;
  for (int index = 0; index < max(0, cells); ++index) {
    run += "\xE2\x96\x91";  // U+2591 LIGHT SHADE
  }
  return run;
}

}  // namespace

ftxui::Element uiProgressBar(double fraction, int width, ftxui::Color fill) {
  return uiSplitProgressBar(fraction, 0.0, width, fill, fill);
}

ftxui::Element uiSplitProgressBar(double first, double second, int width, ftxui::Color firstFill,
                                  ftxui::Color secondFill) {
  const int total = max(1, width);
  const double clampedFirst = clamp(first, 0.0, 1.0);
  const double clampedSecond = clamp(second, 0.0, 1.0 - clampedFirst);

  int firstCells = static_cast<int>(clampedFirst * total + 0.5);
  int secondCells = static_cast<int>(clampedSecond * total + 0.5);
  // Never let rounding claim a cell that the value did not earn, and never let
  // a non-zero fraction round away to an empty bar.
  if (firstCells + secondCells > total) {
    secondCells = total - firstCells;
  }
  if (firstCells == 0 && clampedFirst > 0.0) {
    firstCells = 1;
  }
  if (secondCells == 0 && clampedSecond > 0.0 && firstCells < total) {
    secondCells = 1;
  }
  firstCells = clamp(firstCells, 0, total);
  secondCells = clamp(secondCells, 0, total - firstCells);

  return ftxui::hbox({
      ftxui::text(blockRun(firstCells)) | ftxui::color(firstFill),
      ftxui::text(blockRun(secondCells)) | ftxui::color(secondFill),
      ftxui::text(shadeRun(total - firstCells - secondCells)) | ftxui::color(uiDividerColor()),
  });
}

string displayCategory(const string& category) {
  auto value = trim(category);
  const auto slash = value.find(" / ");
  if (slash != string::npos) {
    value = trim(value.substr(0, slash));
  }

  const auto openParen = value.find(" (");
  if (openParen != string::npos) {
    value = trim(value.substr(0, openParen));
  }

  return value;
}

string ellipsize(const string& value, size_t maxLength) {
  if (maxLength == 0 || value.size() <= maxLength) {
    return value;
  }
  if (maxLength <= 3) {
    return value.substr(0, maxLength);
  }
  return value.substr(0, maxLength - 3) + "...";
}

vector<string> wrapText(const string& text, int width) {
  vector<string> lines;
  if (width <= 0) {
    return lines;
  }

  istringstream words(text);
  string word;
  string line;

  while (words >> word) {
    if (static_cast<int>(line.size() + word.size() + 1) > width && !line.empty()) {
      lines.push_back(line);
      line.clear();
    }

    if (!line.empty()) {
      line.push_back(' ');
    }
    line += word;
  }

  if (!line.empty()) {
    lines.push_back(line);
  }

  if (lines.empty()) {
    lines.push_back({});
  }

  return lines;
}

string joinTags(const vector<string>& tags) {
  return join(tags, ',');
}

string renderTags(const vector<string>& tags) {
  if (tags.empty()) {
    return "-";
  }
  return ellipsize(joinTags(tags), 32);
}

string renderParameters(const vector<Parameter>& parameters) {
  if (parameters.empty()) {
    return "-";
  }

  ostringstream out;
  for (size_t index = 0; index < parameters.size(); ++index) {
    if (index > 0) {
      out << "; ";
    }
    out << parameters[index].name << '=' << parameters[index].value;
  }
  return out.str();
}

string renderUrl(const string& url) {
  if (url.empty()) {
    return "-";
  }
  return ellipsize(url.rfind("//", 0) == 0 ? "https:" + url : url, 32);
}

}  // namespace inventatory
