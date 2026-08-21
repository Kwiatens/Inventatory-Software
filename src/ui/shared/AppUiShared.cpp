// Inventatory - Hardware Inventory Management System
// Shared terminal UI formatting and inventory detail helpers.

#include "ui/shared/AppUiShared.h"

#include "core/PartDescriptor.h"

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

ftxui::Color uiCanvasBg() {
  return ftxui::Color::RGB(13, 16, 16);
}

ftxui::Color uiSurfaceBg() {
  return ftxui::Color::RGB(20, 25, 24);
}

ftxui::Color uiRaisedSurfaceBg() {
  return ftxui::Color::RGB(29, 36, 34);
}

ftxui::Color uiHoverBg() {
  return ftxui::Color::RGB(39, 49, 47);
}

ftxui::Color uiSelectionBg() {
  return ftxui::Color::RGB(44, 68, 64);
}

ftxui::Color uiDividerColor() {
  return ftxui::Color::RGB(56, 69, 67);
}

ftxui::Element uiDivider() {
  return ftxui::separator() | ftxui::color(uiDividerColor());
}

ftxui::Color uiPrimaryText() {
  return ftxui::Color::RGB(241, 238, 229);
}

ftxui::Color uiSecondaryText() {
  return ftxui::Color::RGB(202, 208, 202);
}

ftxui::Color uiMutedText() {
  return ftxui::Color::RGB(140, 150, 144);
}

ftxui::Color uiInteractiveColor() {
  return ftxui::Color::RGB(88, 185, 176);
}

ftxui::Color uiFocusColor() {
  return ftxui::Color::RGB(185, 231, 221);
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
  return ftxui::Color::RGB(165, 201, 165);
}

ftxui::Color uiLinkColor() {
  return ftxui::Color::RGB(143, 203, 197);
}

ftxui::Color uiLabelColor() {
  return uiSecondaryText();
}

ftxui::Color uiWarnColor() {
  return ftxui::Color::RGB(216, 181, 107);
}

ftxui::Color uiDangerColor() {
  return ftxui::Color::RGB(224, 140, 131);
}

ftxui::Color uiActiveBg() {
  return ftxui::Color::RGB(49, 90, 85);
}

ftxui::Color uiActiveSoftBg() {
  return ftxui::Color::RGB(36, 58, 55);
}

ftxui::Color uiWarningBg() {
  return ftxui::Color::RGB(58, 51, 39);
}

ftxui::Color uiDangerBg() {
  return ftxui::Color::RGB(62, 42, 41);
}

ftxui::Color uiDangerFlashBg() {
  return ftxui::Color::RGB(112, 64, 59);
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
  content.push_back(fullLine(title, titleColor.value_or(uiSecondaryText()), uiSurfaceBg()) | ftxui::bold);
  content.push_back(uiDivider());
  for (auto& row : body) content.push_back(move(row));
  return ftxui::vbox(move(content)) | ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element footerField(const string& title, const string& body, ftxui::Color titleColor, ftxui::Color bodyColor,
                           ftxui::Color background, bool flashing) {
  const auto fill = flashing ? uiWarningBg() : background;
  return ftxui::hbox({
             styledText(" " + title + ": ", titleColor, fill) | ftxui::bold,
             styledText(body, bodyColor, fill),
             ftxui::filler(),
         }) |
         ftxui::bgcolor(fill);
}

ftxui::Element uiPrimaryButton(const string& label, bool enabled) {
  // Filled petrol-cyan on canvas-dark text. The inversion is what separates a
  // primary control from the surrounding label/value lines at a glance.
  if (!enabled) return styledText("  " + label + "  ", uiMutedText(), uiRaisedSurfaceBg());
  return styledText("  " + label + "  ", uiCanvasBg(), uiInteractiveColor()) | ftxui::bold;
}

ftxui::Element uiSecondaryButton(const string& label, optional<ftxui::Color> fg, bool enabled) {
  return styledText(" " + label + " ", enabled ? fg.value_or(uiInteractiveColor()) : uiMutedText(),
                    uiRaisedSurfaceBg());
}

ftxui::Element statusCueChip(const string& label, bool active, bool flashing, ftxui::Color fg,
                             ftxui::Color activeBg, ftxui::Color flashingBg, ftxui::Color inactiveBg) {
  const auto fill = flashing ? flashingBg : (active ? activeBg : inactiveBg);
  return styledText(" " + label + " ", fg, fill) | ftxui::bold;
}

ftxui::Element statusDot(const string& label, ftxui::Color color, const string& value) {
  ftxui::Elements parts;
  parts.push_back(ftxui::text("\xE2\x97\x8F ") | ftxui::color(color));  // filled circle
  parts.push_back(ftxui::text(label) | ftxui::color(uiMutedColor()));
  if (!value.empty()) {
    parts.push_back(ftxui::text(" " + value) | ftxui::color(uiMutedColor()));
  }
  return ftxui::hbox(move(parts));
}

bool uiBoxContains(const ftxui::Box& box, int x, int y) {
  return x >= box.x_min && x <= box.x_max && y >= box.y_min && y <= box.y_max;
}

ftxui::Element quantityBadge(int quantity, bool selected) {
  const auto fg = quantity <= 0 ? uiDangerColor() : (quantity <= 5 ? uiWarnColor() : uiSuccessColor());
  const auto bg = selected ? uiRowSelectedBg()
                           : (quantity <= 0 ? uiDangerBg()
                                            : (quantity <= 5 ? uiWarningBg()
                                                             : uiRaisedSurfaceBg()));
  return ftxui::text(" " + to_string(quantity) + " ") | ftxui::bold | ftxui::color(fg) | ftxui::bgcolor(bg);
}

long long uiAnimationTicks() {
  static const auto start = chrono::steady_clock::now();
  return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count();
}

bool uiBlinkOn(int periodMs) {
  const auto period = max(1, periodMs);
  return (uiAnimationTicks() / period) % 2 == 0;
}

string uiAnimatedEllipsis(int intervalMs) {
  const auto interval = max(1, intervalMs);
  const auto phase = static_cast<int>((uiAnimationTicks() / interval) % 4);
  return string(static_cast<size_t>(phase), '.');
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

string normalizeKey(string value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return normalized;
}

bool categoryContains(const InventoryItem& item, initializer_list<const char*> needles) {
  const auto category = normalizeKey(displayCategory(item.category));
  for (const auto* needle : needles) {
    const auto normalizedNeedle = normalizeKey(needle);
    if (category.find(normalizedNeedle) != string::npos || normalizedNeedle.find(category) != string::npos) {
      return true;
    }
  }
  return false;
}

bool parameterLabelMatches(const string& lhs, const string& rhs) {
  const auto normalizedLhs = normalizeKey(lhs);
  const auto normalizedRhs = normalizeKey(rhs);
  return normalizedLhs == normalizedRhs || normalizedLhs.find(normalizedRhs) != string::npos ||
         normalizedRhs.find(normalizedLhs) != string::npos;
}

bool looksLikePackagingValue(const string& value) {
  const auto normalized = normalizeKey(value);
  if (normalized.empty()) {
    return false;
  }

  static const initializer_list<const char*> kPackagingTokens = {
      "tapeandreel", "cuttape", "digireel", "tube", "tray", "bulk", "bag", "strip", "ammo", "box", "loose",
      "reel"};
  return any_of(kPackagingTokens.begin(), kPackagingTokens.end(), [&](const char* token) {
    return normalized == token || normalized.find(token) != string::npos;
  });
}

bool looksLikeFrequencyValue(const string& value) {
  return normalizeKey(value).find("hz") != string::npos;
}

bool looksLikeInductanceValue(const string& value) {
  const auto normalized = normalizeKey(value);
  if (normalized.empty() || looksLikeFrequencyValue(value)) {
    return false;
  }
  if (normalized.find("uh") != string::npos || normalized.find("nh") != string::npos ||
      normalized.find("ph") != string::npos || normalized.find("henry") != string::npos) {
    return true;
  }
  return normalized.find_first_of("0123456789") != string::npos && normalized.back() == 'h';
}

string canonicalInductanceUnit(string unit) {
  transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  if (unit == "uh") {
    return "uH";
  }
  if (unit == "nh") {
    return "nH";
  }
  if (unit == "mh") {
    return "mH";
  }
  if (unit == "ph") {
    return "pH";
  }
  return "H";
}

optional<string> extractInductanceFromText(const string& text) {
  regex valuePattern(R"(\b(\d+(?:\.\d+)?|\d+[rR]\d+)\s*([munp]?h)\b)", regex_constants::icase);
  smatch match;
  if (regex_search(text, match, valuePattern) && match.size() > 2) {
    auto number = match[1].str();
    replace(number.begin(), number.end(), 'R', '.');
    replace(number.begin(), number.end(), 'r', '.');
    return number + canonicalInductanceUnit(match[2].str());
  }
  return nullopt;
}

const Parameter* findParameter(const vector<Parameter>& parameters, initializer_list<const char*> names) {
  for (const auto* name : names) {
    for (const auto& parameter : parameters) {
      if (parameterLabelMatches(parameter.name, name)) {
        return &parameter;
      }
    }
  }
  return nullptr;
}

optional<string> parameterValueMatching(const InventoryItem& item, initializer_list<const char*> names,
                                        bool (*predicate)(const string&)) {
  for (const auto* name : names) {
    for (const auto& parameter : item.parameters) {
      if (!parameterLabelMatches(parameter.name, name)) {
        continue;
      }
      const auto value = trim(parameter.value);
      if (!value.empty() && !looksLikePackagingValue(value) && predicate(value)) {
        return value;
      }
    }
  }
  return nullopt;
}

optional<string> inferredInductanceValue(const InventoryItem& item) {
  if (const auto value = parameterValueMatching(item, {"Inductance", "Value"}, looksLikeInductanceValue)) {
    return value;
  }
  return extractInductanceFromText(item.notes + " " + item.partName + " " + item.sku);
}

optional<string> parameterValue(const InventoryItem& item, initializer_list<const char*> names) {
  if (const auto* parameter = findParameter(item.parameters, names); parameter != nullptr) {
    const auto value = trim(parameter->value);
    if (!value.empty() && !looksLikePackagingValue(value)) {
      return value;
    }
  }
  return nullopt;
}

string prettyLabel(const string& label) {
  const auto trimmed = trim(label);
  if (!trimmed.empty()) {
    return trimmed;
  }
  return "Value";
}

vector<DetailField> electricalFieldsForItem(const InventoryItem& item) {
  // Vendor data uses a lone dash or "N/A" to mean "no value". Those carry no
  // more information than an absent parameter, so they are dropped rather than
  // given a row in the detail panel.
  const auto isPlaceholder = [](const string& value) {
    const auto trimmed = trim(value);
    // normalizeKey drops punctuation, so "N/A" and "n.a." both reduce to "na".
    return trimmed.empty() || trimmed == "-" || trimmed == "\xE2\x80\x93" || trimmed == "\xE2\x80\x94" ||
           normalizeKey(trimmed) == "na";
  };
  vector<DetailField> fields;
  const auto addField = [&](const string& label, optional<string> value) {
    if (value && !isPlaceholder(*value)) {
      fields.push_back({label + ": ", *value, uiLabelColor(), uiTitleColor()});
    }
  };
  const auto addValueAndPackage = [&](const string& label, optional<string> value,
                                      optional<string> package) {
    addField(label, move(value));
    addField("Package", move(package));
  };
  const auto hasParameter = [&](initializer_list<const char*> names) {
    return findParameter(item.parameters, names) != nullptr;
  };

  const auto package = parameterValue(item, {"Package / Case", "Package Case", "Case / Package", "Case Package",
                                             "Supplier Device Package", "Device Package", "Package"});

  if (categoryContains(item, {"capacitor"})) {
    addValueAndPackage("Capacitance", parameterValue(item, {"Capacitance", "Value"}), package);
    addField("Operating Voltage", parameterValue(item, {"Operating Voltage", "Voltage", "Voltage - Rated", "Rated Voltage"}));
    addField("Tolerance", parameterValue(item, {"Tolerance"}));
    addField("Type", parameterValue(item, {"Type", "Dielectric", "Dielectric Type"}));
    addField("ESR", parameterValue(item, {"ESR", "ESR (Equivalent Series Resistance)"}));
    addField("Lifetime @ Temp.", parameterValue(item, {"Lifetime @ Temp.", "Lifetime"}));
    addField("Polarization", parameterValue(item, {"Polarization"}));
    addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
    addField("Applications", parameterValue(item, {"Applications"}));
    addField("Size / Dimension", parameterValue(item, {"Size / Dimension"}));
    return fields;
  }

  if (categoryContains(item, {"resistor"})) {
    addValueAndPackage("Resistance", parameterValue(item, {"Resistance", "Value"}), package);
    addField("Power Dissipation",
             parameterValue(item, {"Power Dissipation", "Power (Watts)", "Power Rating", "Power", "Power - Max", "Watts"}));
    addField("Tolerance", parameterValue(item, {"Tolerance"}));
    addField("Composition", parameterValue(item, {"Composition"}));
    addField("Temperature Coefficient", parameterValue(item, {"Temperature Coefficient", "Tempco"}));
    addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
    addField("Size / Dimension", parameterValue(item, {"Size / Dimension"}));
    addField("Height", parameterValue(item, {"Height - Seated (Max)", "Height"}));
    addField("Number of Terminations", parameterValue(item, {"Number of Terminations"}));
    return fields;
  }

  if (categoryContains(item, {"indicator", "led"})) {
    addValueAndPackage("Color", parameterValue(item, {"Color"}), package);
    addField("Forward Voltage", parameterValue(item, {"Forward Voltage", "Vf"}));
    addField("Wavelength", parameterValue(item, {"Wavelength"}));
    addField("Current", parameterValue(item, {"Current", "If", "Forward Current"}));
    addField("Type", parameterValue(item, {"Type"}));
    return fields;
  }

  if (categoryContains(item, {"integrated circuit", "integrated circuits"})) {
    if (hasParameter({"Memory Type", "Memory Format", "Memory Size"}) ||
        hasParameter({"Program Memory Size", "Program Memory Type"})) {
      addValueAndPackage("Memory", parameterValue(item, {"Memory Size", "Program Memory Size", "Memory"}), package);
      addField("Memory Type", parameterValue(item, {"Memory Type", "Memory Format"}));
      addField("Memory Interface", parameterValue(item, {"Memory Interface", "Interface"}));
      addField("Technology", parameterValue(item, {"Technology"}));
      addField("Clock", parameterValue(item, {"Clock Frequency", "Speed"}));
      addField("Operating Voltage", parameterValue(item, {"Voltage - Supply", "Voltage - Supply (Min/Max)", "Voltage - Supply (Min)", "Voltage - Supply (Max)"}));
      addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
      addField("Mounting Type", parameterValue(item, {"Mounting Type"}));
      addField("DigiKey Programmable", parameterValue(item, {"DigiKey Programmable"}));
      return fields;
    }

    if (hasParameter({"Voltage - Input", "Voltage - Output", "Current - Output", "Output Type", "Voltage - Output (Min/Fixed)"})) {
      addValueAndPackage("Output Voltage",
                         parameterValue(item, {"Voltage - Output", "Voltage - Output (Min/Fixed)", "Voltage - Output (Max)", "Output Voltage"}),
                         package);
      addField("Input Voltage", parameterValue(item, {"Voltage - Input", "Voltage - Input (Min/Max)", "Vin"}));
      addField("Current", parameterValue(item, {"Current - Output", "Output Current", "Current"}));
      addField("Type", parameterValue(item, {"Type", "Output Type"}));
      addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
      addField("Mounting Type", parameterValue(item, {"Mounting Type"}));
      return fields;
    }

    addValueAndPackage("Function", parameterValue(item, {"Function", "Type", "Memory Type"}), package);
    addField("Technology", parameterValue(item, {"Technology"}));
    addField("Interface", parameterValue(item, {"Interface", "Memory Interface"}));
    addField("Operating Voltage", parameterValue(item, {"Voltage - Supply", "Voltage - Supply (Min/Max)"}));
    addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
    addField("Mounting Type", parameterValue(item, {"Mounting Type"}));
    addField("DigiKey Programmable", parameterValue(item, {"DigiKey Programmable"}));
    return fields;
  }

  if (categoryContains(item, {"inductor", "choke", "coil"})) {
    addValueAndPackage("Inductance", inferredInductanceValue(item), package);
    addField("Current Rating", parameterValue(item, {"Current Rating", "Current Rating (Amps)", "Current"}));
    addField("DC Resistance", parameterValue(item, {"DC Resistance", "DC Resistance (DCR)", "DCR"}));
    addField("Saturation Current", parameterValue(item, {"Saturation Current", "Current - Saturation (Isat)"}));
    addField("Q @ Freq", parameterValue(item, {"Q @ Freq"}));
    addField("Frequency", parameterValue(item, {"Frequency - Self Resonant", "Frequency"}));
    addField("Shielding", parameterValue(item, {"Shielding"}));
    addField("Material", parameterValue(item, {"Material - Core"}));
    return fields;
  }

  if (categoryContains(item, {"diode", "rectifier", "schottky"})) {
    addValueAndPackage("Forward Voltage", parameterValue(item, {"Forward Voltage", "Voltage - Forward (Vf) (Max) @ If", "Vf"}), package);
    addField("Reverse Voltage", parameterValue(item, {"Reverse Voltage", "Voltage - DC Reverse (Vr) (Max)", "Peak Reverse Voltage", "Vr"}));
    addField("Current", parameterValue(item, {"Current", "Current - Average Rectified (Io)", "If", "Forward Current"}));
    addField("Technology", parameterValue(item, {"Technology"}));
    addField("Speed", parameterValue(item, {"Speed"}));
    addField("Reverse Leakage", parameterValue(item, {"Current - Reverse Leakage @ Vr"}));
    addField("Operating Temperature", parameterValue(item, {"Operating Temperature - Junction", "Operating Temperature"}));
    return fields;
  }

  if (categoryContains(item, {"transistor", "mosfet", "fet", "discrete semiconductor"})) {
    addValueAndPackage("Drain-Source Voltage",
                       parameterValue(item, {"Drain-Source Voltage", "Drain to Source Voltage (Vdss)", "Vds", "Vdss"}), package);
    addField("Current", parameterValue(item, {"Continuous Drain Current", "Current - Continuous Drain (Id) @ 25Â°C", "Current", "Id"}));
    addField("RDS On", parameterValue(item, {"Rds On", "Rds On (Max) @ Id, Vgs", "RDS(ON)"}));
    addField("Gate Charge", parameterValue(item, {"Gate Charge", "Gate Charge (Qg) (Max) @ Vgs"}));
    addField("Drive Voltage", parameterValue(item, {"Drive Voltage", "Drive Voltage (Max Rds On, Min Rds On)"}));
    addField("VGS", parameterValue(item, {"Vgs", "Vgs (Max)", "Vgs(th) (Max) @ Id"}));
    addField("Input Capacitance", parameterValue(item, {"Input Capacitance (Ciss) (Max) @ Vds", "Input Capacitance"}));
    addField("Power - Max", parameterValue(item, {"Power - Max"}));
    addField("Technology", parameterValue(item, {"Technology"}));
    return fields;
  }

  if (categoryContains(item, {"connector"})) {
    addValueAndPackage("Pins", parameterValue(item, {"Pins", "Number of Positions", "Pin Count"}), package);
    addField("Connector Type", parameterValue(item, {"Connector Type"}));
    addField("Contact Type", parameterValue(item, {"Contact Type"}));
    addField("Rows", parameterValue(item, {"Rows", "Number of Rows"}));
    addField("Pitch", parameterValue(item, {"Pitch", "Pitch - Mating"}));
    addField("Mounting Type", parameterValue(item, {"Mounting Type"}));
    addField("Termination", parameterValue(item, {"Termination"}));
    addField("Fastening Type", parameterValue(item, {"Fastening Type"}));
    addField("Shrouding", parameterValue(item, {"Shrouding"}));
    return fields;
  }

  if (categoryContains(item, {"mcu", "microcontroller"})) {
    addValueAndPackage("Flash", parameterValue(item, {"Flash"}), package);
    addField("Core", parameterValue(item, {"Core", "Core Processor"}));
    addField("Operating Voltage", parameterValue(item, {"Operating Voltage", "Voltage - Supply", "Voltage"}));
    addField("RAM", parameterValue(item, {"RAM", "Memory"}));
    addField("Clock", parameterValue(item, {"Clock Speed", "Speed"}));
    addField("I/O", parameterValue(item, {"Number of I/O"}));
    addField("Program Memory", parameterValue(item, {"Program Memory Size", "Program Memory Type"}));
    addField("EEPROM", parameterValue(item, {"EEPROM Size"}));
    addField("Connectivity", parameterValue(item, {"Connectivity"}));
    addField("Peripherals", parameterValue(item, {"Peripherals"}));
    return fields;
  }

  if (categoryContains(item, {"regulator", "voltage regulator", "power management"})) {
    addValueAndPackage("Output Voltage", parameterValue(item, {"Output Voltage", "Voltage - Output", "Vout"}), package);
    addField("Input Voltage", parameterValue(item, {"Voltage - Input", "Vin"}));
    addField("Current", parameterValue(item, {"Output Current", "Current - Output", "Iout"}));
    addField("Type", parameterValue(item, {"Type", "Output Type"}));
    return fields;
  }

  if (categoryContains(item, {"crystal", "oscillator", "resonator"})) {
    addValueAndPackage("Frequency", parameterValue(item, {"Frequency"}), package);
    addField("Load Capacitance", parameterValue(item, {"Load Capacitance"}));
    addField("ESR", parameterValue(item, {"ESR", "Equivalent Series Resistance"}));
    addField("Operating Mode", parameterValue(item, {"Operating Mode"}));
    addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
    addField("Size / Dimension", parameterValue(item, {"Size / Dimension"}));
    return fields;
  }

  if (categoryContains(item, {"sensor", "temperature sensor", "pressure sensor"})) {
    addValueAndPackage("Type", parameterValue(item, {"Type"}), package);
    addField("Sensor Type", parameterValue(item, {"Sensor Type"}));
    addField("Output", parameterValue(item, {"Output", "Output Type"}));
    addField("Voltage - Supply", parameterValue(item, {"Voltage - Supply"}));
    addField("Resolution", parameterValue(item, {"Resolution"}));
    addField("Accuracy", parameterValue(item, {"Accuracy - Highest (Lowest)"}));
    addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
    addField("Features", parameterValue(item, {"Features"}));
    return fields;
  }

  if (categoryContains(item, {"circuit protection", "fuse", "tvs", "transient voltage suppressor"})) {
    addValueAndPackage("Current Rating", parameterValue(item, {"Current Rating (Amps)", "Current Rating", "Current"}), package);
    addField("Voltage AC", parameterValue(item, {"Voltage Rating - AC"}));
    addField("Voltage DC", parameterValue(item, {"Voltage Rating - DC"}));
    addField("Reverse Standoff", parameterValue(item, {"Voltage - Reverse Standoff (Typ)"}));
    addField("Breakdown", parameterValue(item, {"Voltage - Breakdown (Min)"}));
    addField("Clamping", parameterValue(item, {"Voltage - Clamping (Max) @ Ipp"}));
    addField("Peak Pulse Current", parameterValue(item, {"Current - Peak Pulse (10/1000Âµs)"}));
    addField("Peak Pulse Power", parameterValue(item, {"Power - Peak Pulse"}));
    addField("Response Time", parameterValue(item, {"Response Time"}));
    addField("Operating Temperature", parameterValue(item, {"Operating Temperature"}));
    return fields;
  }

  optional<string> primaryValue;
  optional<string> primaryLabel;
  for (const auto& parameter : item.parameters) {
    const auto label = prettyLabel(parameter.name);
    if (normalizeKey(label) == "package") {
      continue;
    }
    if (!primaryValue) {
      primaryLabel = label;
      primaryValue = trim(parameter.value);
      continue;
    }
    addField(label, trim(parameter.value));
  }

  if (primaryValue && !primaryValue->empty()) {
    fields.insert(fields.begin(),
                  DetailField{primaryLabel.value_or("Value") + ": ", *primaryValue, uiLabelColor(), uiTitleColor()});
  } else if (package && !package->empty()) {
    fields.insert(fields.begin(), DetailField{"Package: ", *package, uiLabelColor(), uiTitleColor()});
  }

  return fields;
}

vector<DetailField> stockPreviewFields(const InventoryItem& item, string rack) {
  vector<DetailField> fields;
  const bool rackAssigned = !rack.empty();
  fields.push_back({"Inventatory RACK: ", rackAssigned ? rack : "NOT ASSIGNED", uiAccentColor(),
                    rackAssigned ? uiTitleColor() : uiWarnColor()});
  fields.push_back({"Name: ", item.partName, uiInfoColor(), uiTitleColor()});
  fields.push_back({"At a glance: ", partShortDescription(item), uiLabelColor(), uiTitleColor()});
  fields.push_back({"Manufacturer: ", item.manufacturer, uiInfoColor(), uiTitleColor()});
  fields.push_back({"Quantity: ", to_string(item.quantity), uiSuccessColor(), uiTitleColor()});

  const auto electricalFields = electricalFieldsForItem(item);
  fields.insert(fields.end(), electricalFields.begin(), electricalFields.end());
  return fields;
}

vector<DetailField> detailCoreFields(const InventoryItem& item, string rack) {
  const bool rackAssigned = !rack.empty();
  return {
      {"Inventatory RACK: ", rackAssigned ? rack : "NOT ASSIGNED", uiAccentColor(),
       rackAssigned ? uiTitleColor() : uiWarnColor()},
      {"Name: ", item.partName, uiInfoColor(), uiTitleColor()},
      {"At a glance: ", partShortDescription(item), uiLabelColor(), uiTitleColor()},
      {"Manufacturer: ", item.manufacturer, uiInfoColor(), uiTitleColor()},
      {"Category: ", displayCategory(item.category), uiLabelColor(), uiTitleColor()},
      {"Quantity: ", to_string(item.quantity), uiSuccessColor(), uiTitleColor()},
      {"Threshold: ", to_string(item.reorderThreshold), uiWarnColor(), uiTitleColor()},
      {"Location: ", item.location, uiMutedColor(), uiTitleColor()},
  };
}

ftxui::Element detailFieldLine(const DetailField& field, int width) {
  const int valueWidth = max(0, width - static_cast<int>(field.label.size()) - 1);
  return ftxui::hbox({
             styledText(field.label, field.labelColor),
             styledText(ellipsize(field.value, static_cast<size_t>(valueWidth)), field.valueColor),
             ftxui::filler(),
         }) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

}  // namespace inventatory
