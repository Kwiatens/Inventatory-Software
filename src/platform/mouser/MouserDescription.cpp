// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/mouser/MouserDescription.h"

#include "core/inventory/Inventory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace inventatory {
using namespace std;

namespace {

constexpr const char* kMicro = "\xC2\xB5";  // µ, as DigiKey writes it

bool categoryHas(const string& category, const char* word) { return containsInsensitive(category, word); }

bool categoryHasAny(const string& category, initializer_list<const char*> words) {
  return any_of(words.begin(), words.end(), [&](const char* word) { return categoryHas(category, word); });
}

string numberText(double value) {
  char buffer[32];
  snprintf(buffer, sizeof buffer, "%.9g", value);
  return buffer;
}

// A value seen more than once must agree; two different values mean the text is about something else too.
class UniqueValue {
 public:
  void add(double value) {
    if (!value_.has_value()) {
      value_ = value;
    } else if (fabs(*value_ - value) > 1e-9 * max(fabs(*value_), 1e-30)) {
      conflict_ = true;
    }
  }
  optional<double> get() const { return conflict_ ? nullopt : value_; }

 private:
  optional<double> value_;
  bool conflict_ = false;
};

optional<double> parseNumber(const string& text) {
  if (text.empty() || text == ".") return nullopt;
  try {
    size_t used = 0;
    const double value = stod(text, &used);
    if (used != text.size() || !isfinite(value) || value < 0) return nullopt;
    return value;
  } catch (const exception&) {
    return nullopt;
  }
}

string withPrefix(double value, const char* unit, bool micro) {
  struct Step {
    double factor;
    const char* prefix;
  };
  const Step steps[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""}, {1e-3, "m"}, {1e-6, micro ? kMicro : "u"}, {1e-9, "n"}, {1e-12, "p"}};
  for (const auto& step : steps) {
    if (value >= step.factor * 0.9999999) return numberText(value / step.factor) + " " + step.prefix + unit;
  }
  return numberText(value * 1e12) + " p" + unit;
}

// The imperial chip codes a description names, one of them or none.
optional<string> chipCode(const string& description) {
  static const regex code(R"((?:^|[^0-9A-Za-z.])(01005|0201|0402|0603|0805|1008|1206|1210|1812|2010|2220|2512)(?![0-9A-Za-z]))");
  set<string> found;
  for (sregex_iterator it(description.begin(), description.end(), code), end; it != end; ++it) found.insert((*it)[1]);
  if (found.size() != 1) return nullopt;
  return *found.begin();
}

optional<string> tolerance(const string& description) {
  static const regex percent(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)\s*%)");
  smatch match;
  if (!regex_search(description, match, percent)) return nullopt;
  const auto value = parseNumber(match[1]);
  if (!value.has_value() || *value <= 0 || *value > 100) return nullopt;
  return "\xC2\xB1" + numberText(*value) + "%";
}

optional<string> ratedVoltage(const string& description) {
  static const regex volts(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)\s*V(?:DC)?(?![0-9A-Za-z]))", regex::icase);
  UniqueValue unique;
  for (sregex_iterator it(description.begin(), description.end(), volts), end; it != end; ++it) {
    const auto value = parseNumber((*it)[1]);
    if (value.has_value() && *value > 0 && *value < 100000) unique.add(*value);
  }
  const auto value = unique.get();
  if (!value.has_value()) return nullopt;
  return numberText(*value) + "V";
}

void addIf(vector<Parameter>& out, const char* name, const optional<string>& value) {
  if (value.has_value() && !value->empty()) out.push_back({name, *value});
}

optional<string> resistance(const string& description) {
  UniqueValue unique;
  const auto scale = [](const string& prefix) {
    if (prefix == "k" || prefix == "K") return 1e3;
    if (prefix == "M") return 1e6;
    if (prefix == "G") return 1e9;
    if (prefix == "m") return 1e-3;
    return 1.0;
  };
  // "10K OHM", "10kOhms", "0.01 Ohm", "5 mOhm", "100 Ω"
  static const regex withUnit(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)\s*([kKMGm]?)\s*(?:[Oo][Hh][Mm][Ss]?|\xCE\xA9)(?![A-Za-z]))");
  // "10K", "2.2M" with no unit word
  static const regex bare(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)([kKM])(?![0-9A-Za-z]))");
  // "4R7", "4K7", "100R"
  static const regex eia(R"((?:^|[^0-9A-Za-z.])(\d+)([RKM])(\d*)(?![0-9A-Za-z]))");
  for (const auto* pattern : {&withUnit, &bare}) {
    for (sregex_iterator it(description.begin(), description.end(), *pattern), end; it != end; ++it) {
      const auto number = parseNumber((*it)[1]);
      if (number.has_value()) unique.add(*number * scale((*it)[2]));
    }
  }
  for (sregex_iterator it(description.begin(), description.end(), eia), end; it != end; ++it) {
    const string whole = (*it)[1];
    const string letter = (*it)[2];
    const string fraction = (*it)[3];
    const auto number = parseNumber(whole + (fraction.empty() ? "" : "." + fraction));
    if (number.has_value()) unique.add(*number * (letter == "R" ? 1.0 : scale(letter)));
  }
  const auto value = unique.get();
  if (!value.has_value() || *value <= 0) return nullopt;
  return withPrefix(*value, "Ohms", false);
}

optional<string> capacitance(const string& description) {
  UniqueValue unique;
  static const regex farads(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)\s*(pF|nF|uF|\xC2\xB5F|\xCE\xBCF|KPF)(?![A-Za-z]))", regex::icase);
  for (sregex_iterator it(description.begin(), description.end(), farads), end; it != end; ++it) {
    const auto number = parseNumber((*it)[1]);
    if (!number.has_value()) continue;
    const auto unit = toLower((*it)[2]);
    // Mouser writes 100 nF as "100KPF": thousands of picofarads.
    const double factor = unit == "pf" ? 1e-12 : unit == "nf" ? 1e-9 : unit == "kpf" ? 1e-9 : 1e-6;
    unique.add(*number * factor);
  }
  const auto value = unique.get();
  if (!value.has_value() || *value <= 0) return nullopt;
  return withPrefix(*value, "F", true);
}

optional<string> inductance(const string& description) {
  UniqueValue unique;
  static const regex henries(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)\s*(nH|uH|\xC2\xB5H|\xCE\xBCH)(?![A-Za-z]))", regex::icase);
  for (sregex_iterator it(description.begin(), description.end(), henries), end; it != end; ++it) {
    const auto number = parseNumber((*it)[1]);
    if (!number.has_value()) continue;
    unique.add(*number * (toLower((*it)[2]) == "nh" ? 1e-9 : 1e-6));
  }
  const auto value = unique.get();
  if (!value.has_value() || *value <= 0) return nullopt;
  return withPrefix(*value, "H", true);
}

optional<string> dielectric(const string& description) {
  static const regex code(R"((?:^|[^0-9A-Za-z])(X7R|X5R|X7S|X6S|X7T|X8R|C0G|COG|NP0|NPO|Y5V|Z5U)(?![0-9A-Za-z]))", regex::icase);
  set<string> found;
  for (sregex_iterator it(description.begin(), description.end(), code), end; it != end; ++it) {
    found.insert(toUpper((*it)[1]));
  }
  if (found.size() != 1) return nullopt;
  return *found.begin();
}

optional<string> frequency(const string& description) {
  UniqueValue unique;
  static const regex hertz(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)\s*(kHz|MHz|GHz)(?![A-Za-z]))", regex::icase);
  for (sregex_iterator it(description.begin(), description.end(), hertz), end; it != end; ++it) {
    const auto number = parseNumber((*it)[1]);
    if (!number.has_value()) continue;
    const auto unit = toLower((*it)[2]);
    unique.add(*number * (unit == "khz" ? 1e3 : unit == "mhz" ? 1e6 : 1e9));
  }
  const auto value = unique.get();
  if (!value.has_value() || *value <= 0) return nullopt;
  return withPrefix(*value, "Hz", false);
}

// A crystal's size code is its length and width in tenths of a millimetre: 3225 is 3.2 mm by 2.5 mm.
optional<string> crystalSize(const string& description) {
  static const set<string> codes = {"1612", "2016", "2520", "3215", "3225", "4025", "5032", "6035", "7050", "8045"};
  static const regex code(R"((?:^|[^0-9A-Za-z.])(\d{4})(?![0-9A-Za-z]))");
  set<string> found;
  for (sregex_iterator it(description.begin(), description.end(), code), end; it != end; ++it) {
    if (codes.count((*it)[1]) != 0) found.insert((*it)[1]);
  }
  if (found.size() != 1) return nullopt;
  const auto& text = *found.begin();
  const auto side = [](const string& tenths) {
    char buffer[16];
    snprintf(buffer, sizeof buffer, "%.2f", stoi(tenths) / 10.0);
    return string(buffer) + "mm";
  };
  return side(text.substr(0, 2)) + " x " + side(text.substr(2, 2));
}

optional<string> ledColour(const string& description) {
  static const regex colour(R"((?:^|[^0-9A-Za-z])(RED|GREEN|BLUE|YELLOW|WHITE|AMBER|ORANGE)(?![0-9A-Za-z]))", regex::icase);
  set<string> found;
  for (sregex_iterator it(description.begin(), description.end(), colour), end; it != end; ++it) {
    found.insert(toLower((*it)[1]));
  }
  if (found.size() != 1) return nullopt;
  auto name = *found.begin();
  name[0] = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
  return name;
}

optional<string> currentRating(const string& description) {
  UniqueValue unique;
  static const regex amps(R"((?:^|[^0-9A-Za-z.])(\d*\.?\d+)\s*(mA|A|Amps?)(?![A-Za-z]))");
  for (sregex_iterator it(description.begin(), description.end(), amps), end; it != end; ++it) {
    const auto number = parseNumber((*it)[1]);
    if (number.has_value()) unique.add(*number * ((*it)[2] == "mA" ? 1e-3 : 1.0));
  }
  const auto value = unique.get();
  if (!value.has_value() || *value <= 0) return nullopt;
  return withPrefix(*value, "A", false);
}

bool isResistor(const string& category) {
  return categoryHas(category, "resistors") &&
         !categoryHasAny(category, {"network", "array", "thermistor", "potentiometer", "trimmer", "varistor", "ntc", "ptc"});
}

bool isLed(const string& category) {
  static const regex led(R"((^|[^A-Za-z])LEDs?([^A-Za-z]|$))", regex::icase);
  return regex_search(category, led) &&
         !categoryHasAny(category, {"driver", "display", "lighting", "module", "bar", "array", "matrix", "controller", "emitter", "infrared", "uv"});
}

}  // namespace

vector<Parameter> mouserDescriptionParameters(const string& category, const string& description) {
  vector<Parameter> out;
  if (trim(description).empty()) return out;
  if (isResistor(category)) {
    addIf(out, "Resistance", resistance(description));
    addIf(out, "Tolerance", tolerance(description));
    addIf(out, "Package / Case", chipCode(description));
  } else if (categoryHas(category, "capacitors") && categoryHasAny(category, {"ceramic", "mlcc"}) &&
             !categoryHas(category, "electrolytic")) {
    addIf(out, "Capacitance", capacitance(description));
    addIf(out, "Voltage - Rated", ratedVoltage(description));
    addIf(out, "Temperature Coefficient", dielectric(description));
    addIf(out, "Tolerance", tolerance(description));
    addIf(out, "Package / Case", chipCode(description));
  } else if (categoryHas(category, "inductors") && !categoryHas(category, "ferrite")) {
    addIf(out, "Inductance", inductance(description));
    addIf(out, "Tolerance", tolerance(description));
    addIf(out, "Package / Case", chipCode(description));
  } else if (isLed(category)) {
    addIf(out, "Color", ledColour(description));
    addIf(out, "Package / Case", chipCode(description));
  } else if (categoryHas(category, "crystals") && !categoryHas(category, "oscillator")) {
    addIf(out, "Frequency", frequency(description));
    addIf(out, "Size / Dimension", crystalSize(description));
    static const regex hc49(R"((?:^|[^0-9A-Za-z])HC-?49(?![0-9]))", regex::icase);
    if (regex_search(description, hc49)) out.push_back({"Package / Case", "HC-49"});
  } else if (categoryHas(category, "fuses") && !categoryHasAny(category, {"holder", "clip", "block", "puller"})) {
    addIf(out, "Current Rating (Amps)", currentRating(description));
    addIf(out, "Package / Case", chipCode(description));
  }
  return out;
}

}  // namespace inventatory
