// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Item label detail selection helpers.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "core/parts/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace inventatory {

using namespace std;

namespace label_printer_detail {

namespace {

// The SI prefixes a value may carry, each with the power of ten it stands for. The micro sign comes as U+00B5 from one
// vendor and as the Greek mu, U+03BC, from another; both are the same prefix.
struct UnitPrefix {
  const char* text;
  int exponent;
};

const vector<UnitPrefix>& unitPrefixes() {
  static const vector<UnitPrefix> prefixes = {
      {"G", 9}, {"M", 6}, {"k", 3}, {"K", 3}, {"m", -3}, {u8"µ", -6}, {u8"μ", -6}, {"u", -6}, {"n", -9}, {"p", -12}};
  return prefixes;
}

// The prefix text printed for a power of ten the normaliser may choose. Only the powers in this list are ever used.
const char* prefixText(int exponent) {
  switch (exponent) {
    case 9: return "G";
    case 6: return "M";
    case 3: return "k";
    case -3: return "m";
    case -6: return u8"µ";
    case -9: return "n";
    case -12: return "p";
    default: return "";
  }
}

// One figure with its unit: "0.6V", "600 mV" or "1.5µH". The unit is optional only so that the low end of a range can
// be "100" in "100 to 600mV".
struct UnitFigure {
  string number;
  int exponent = 0;
  string unit;
  bool hasUnit = false;
};

bool isKnownUnit(const string& unit) {
  static const vector<string> units = {"V", "A", "W", u8"Ω", "Hz", "F", "H", "cd"};
  return find(units.begin(), units.end(), unit) != units.end();
}

// Reads "600 mV" as a figure. Returns false for anything else, so a value this rule does not own is printed as it was
// sent: "Ø5x11mm", "±20%", "2000h" and "-40 to 85°C" all stay as they are.
bool parseUnitFigure(const string& text, UnitFigure& figure) {
  size_t at = 0;
  if (text.empty() || !isdigit(static_cast<unsigned char>(text[0]))) return false;
  while (at < text.size() && isdigit(static_cast<unsigned char>(text[at]))) ++at;
  if (at < text.size() && text[at] == '.') {
    ++at;
    while (at < text.size() && isdigit(static_cast<unsigned char>(text[at]))) ++at;
  }
  figure = {};
  figure.number = text.substr(0, at);
  while (at < text.size() && text[at] == ' ') ++at;
  const auto rest = text.substr(at);
  if (rest.empty()) return true;
  for (const auto& prefix : unitPrefixes()) {
    const auto length = strlen(prefix.text);
    if (rest.size() > length && rest.compare(0, length, prefix.text) == 0 && isKnownUnit(rest.substr(length))) {
      figure.exponent = prefix.exponent;
      figure.unit = rest.substr(length);
      figure.hasUnit = true;
      return true;
    }
  }
  if (!isKnownUnit(rest)) return false;
  figure.unit = rest;
  figure.hasUnit = true;
  return true;
}

// The value of a figure in base units (volts, amps, ohms ...), as a long double so the prefix choice is exact enough.
long double baseValue(const UnitFigure& figure, int exponent) {
  return strtold(figure.number.c_str(), nullptr) * powl(10.0L, static_cast<long double>(exponent));
}

// The power of ten a base value prints with. Most units use the prefix that leaves a mantissa from 1 to 999. Capacitance
// is the exception: electrolytics are written "4700µF" and "1000µF", not "4.7mF" and "1mF", so farads print as F from
// 1 F, mF from 100 mF, µF below that, and nF and pF for the small values.
int exponentFor(const string& unit, long double base) {
  const auto magnitude = fabsl(base);
  if (magnitude == 0.0L) return 0;
  const long double tolerance = 1.0L - 1e-9L;
  if (unit == "F") {
    if (magnitude >= tolerance) return 0;
    if (magnitude >= 0.1L * tolerance) return -3;
    if (magnitude >= 1e-6L * tolerance) return -6;
  }
  for (const int candidate : {9, 6, 3, 0, -3, -6, -9, -12}) {
    if (magnitude >= powl(10.0L, static_cast<long double>(candidate)) * tolerance) return candidate;
  }
  return -12;
}

// Every vendor's value reaches the label through this rule, so the same quantity prints the same way whichever vendor
// sent it and for a vendor added later. The prefix comes from exponentFor: "0.6V" and "600 mV" both print "600mV",
// "0.25W" and "250 mW" both print "250mW", "16 V" prints "16V" and "0.6kV" prints "600V", and "4700 µF" stays "4700µF".
// A range takes one prefix for both ends, so "3.8 to 32V" stays one unit. The ± before a figure is kept as it was sent.
string normalizedUnitValue(const string& value) {
  const auto text = trim(value);
  const auto first = text.find_first_of("0123456789");
  if (first == string::npos) return value;
  const auto leading = text.substr(0, first);
  if (!leading.empty() && leading != u8"±" && leading != u8"± ") return value;
  const auto body = trim(text.substr(first));
  const auto to = body.find(" to ");
  UnitFigure high;
  if (to == string::npos) {
    if (!parseUnitFigure(body, high) || !high.hasUnit) return value;
    const auto exponent = exponentFor(high.unit, baseValue(high, high.exponent));
    char mantissa[32];
    snprintf(mantissa, sizeof mantissa, "%.6Lg", baseValue(high, high.exponent) / powl(10.0L, exponent));
    return leading + mantissa + prefixText(exponent) + high.unit;
  }
  UnitFigure low;
  if (!parseUnitFigure(trim(body.substr(to + 4)), high) || !high.hasUnit) return value;
  if (!parseUnitFigure(trim(body.substr(0, to)), low)) return value;
  if (low.hasUnit && low.unit != high.unit) return value;
  if (!low.hasUnit && low.exponent != 0) return value;
  // A low end without its own unit takes the high end's prefix: "100 to 600mV" is 0.1 to 0.6 V.
  const auto lowBase = baseValue(low, low.hasUnit ? low.exponent : high.exponent);
  const auto highBase = baseValue(high, high.exponent);
  const auto exponent = exponentFor(high.unit, highBase);
  char lowText[32];
  char highText[32];
  snprintf(lowText, sizeof lowText, "%.6Lg", lowBase / powl(10.0L, exponent));
  snprintf(highText, sizeof highText, "%.6Lg", highBase / powl(10.0L, exponent));
  return leading + lowText + " to " + highText + prefixText(exponent) + high.unit;
}

// The measured value printed as the main text of a passive part, without its
// tolerance. Empty when the part is not a passive or the value is missing.
string measuredMainValue(const InventoryItem& item) {
  const auto compactValue = [](string value) {
    value = normalizedUnitValue(trim(value));
    value.erase(remove(value.begin(), value.end(), ' '), value.end());
    return fieldOrBlank(value, 16);
  };
  if (categoryContains(item, {"capacitor"})) {
    return compactValue(firstParameter(item, {"Capacitance", "Value"}).value_or(string{}));
  }
  if (categoryContains(item, {"resistor"})) {
    return compactValue(normalizeResistanceValue(firstParameter(item, {"Resistance", "Value"}).value_or(string{})));
  }
  if (categoryContains(item, {"inductor", "choke", "coil"})) {
    return compactValue(firstInductanceParameter(item).value_or(string{}));
  }
  return {};
}

void replaceAll(string& value, const string& from, const string& to) {
  for (size_t at = value.find(from); at != string::npos; at = value.find(from, at + to.size())) {
    value.replace(at, from.size(), to);
  }
}

string cutAt(const string& value, const string& marker) {
  const auto at = value.find(marker);
  return at == string::npos ? value : trim(value.substr(0, at));
}

// "Radial, Can" and "Axial" say how the leads leave the body, not what the package is.
bool isLeadStyleName(const string& value) {
  const auto key = normalizeKey(value.substr(0, value.find(',')));
  return key == "radial" || key == "axial";
}

bool isNonstandardPackage(const string& value) {
  const auto key = normalizeKey(value);
  return key.empty() || key.find("nonstandard") != string::npos || key == "custom" || key == "other" ||
         isLeadStyleName(value);
}

// "12.00" -> "12", "0.50" -> "0.5".
string trimTrailingZeros(string number) {
  if (number.find('.') != string::npos) {
    while (!number.empty() && number.back() == '0') number.pop_back();
    if (!number.empty() && number.back() == '.') number.pop_back();
  }
  return number;
}

// The label patterns in this file were std::regex searches. std::regex matchers may use stack that
// grows with the input length, and imported text can be very long, so these are linear scanners.
// They match the same ASCII grammar: \d is [0-9], \s is the C isspace set, and (?![A-Za-z]) is an
// ASCII letter test. Each scanner finds the leftmost match, as regex_search does.
bool isAsciiDigitChar(char ch) { return ch >= '0' && ch <= '9'; }

bool isAsciiLetterChar(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

bool isRegexSpaceChar(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}

size_t skipRegexSpaces(const string& value, size_t pos) {
  while (pos < value.size() && isRegexSpaceChar(value[pos])) ++pos;
  return pos;
}

// True when `literal` occurs at `pos`. An empty literal matches at every position up to the end.
bool literalAt(const string& value, size_t pos, const char* literal) {
  return pos <= value.size() && value.compare(pos, strlen(literal), literal) == 0;
}

bool letterAt(const string& value, size_t pos) { return pos < value.size() && isAsciiLetterChar(value[pos]); }

// End of the decimal \d+(?:\.\d+)? that starts at `pos`, which must hold a digit. The fraction is
// taken whenever it is present: every pattern here needs a space, "m", "x" or ")" after the number,
// so a shorter number could never be followed by a match.
size_t decimalEnd(const string& value, size_t pos) {
  size_t end = pos;
  while (end < value.size() && isAsciiDigitChar(value[end])) ++end;
  if (end + 1 < value.size() && value[end] == '.' && isAsciiDigitChar(value[end + 1])) {
    ++end;
    while (end < value.size() && isAsciiDigitChar(value[end])) ++end;
  }
  return end;
}

// Matches (\d+(?:\.\d+)?)\s*mm\s*[xX]\s*(\d+(?:\.\d+)?)\s*mm at `start`, which must hold a digit.
bool metricSizeAt(const string& value, size_t start, string& first, string& second) {
  const size_t firstEnd = decimalEnd(value, start);
  size_t pos = skipRegexSpaces(value, firstEnd);
  if (!literalAt(value, pos, "mm")) return false;
  pos = skipRegexSpaces(value, pos + 2);
  if (pos >= value.size() || (value[pos] != 'x' && value[pos] != 'X')) return false;
  pos = skipRegexSpaces(value, pos + 1);
  if (pos >= value.size() || !isAsciiDigitChar(value[pos])) return false;
  const size_t secondEnd = decimalEnd(value, pos);
  if (!literalAt(value, skipRegexSpaces(value, secondEnd), "mm")) return false;
  first = value.substr(start, firstEnd - start);
  second = value.substr(pos, secondEnd - pos);
  return true;
}

// The leftmost "N mm x N mm" in `value`, with both numbers as written.
bool findMetricSize(const string& value, string& first, string& second) {
  size_t start = 0;
  while (start < value.size()) {
    if (!isAsciiDigitChar(value[start])) {
      ++start;
      continue;
    }
    if (metricSizeAt(value, start, first, second)) return true;
    // Every later start inside this digit run reaches the same end of the run, so none can match
    // either: skip to the end of the run.
    while (start < value.size() && isAsciiDigitChar(value[start])) ++start;
  }
  return false;
}

// The leftmost "(N mm)" in `value`, with the number as written. Only '(' can start the match.
bool findBracketedMetric(const string& value, string& number) {
  for (size_t open = value.find('('); open != string::npos; open = value.find('(', open + 1)) {
    if (open + 1 >= value.size() || !isAsciiDigitChar(value[open + 1])) continue;
    const size_t end = decimalEnd(value, open + 1);
    if (literalAt(value, skipRegexSpaces(value, end), "mm)")) {
      number = value.substr(open + 1, end - open - 1);
      return true;
    }
  }
  return false;
}

// "0.157" L x 0.157" W (4.00mm x 4.00mm)" -> "4x4mm".
string metricSizeLine(const string& value) {
  string first;
  string second;
  if (!findMetricSize(value, first, second)) {
    return {};
  }
  return trimTrailingZeros(first) + "x" + trimTrailingZeros(second) + "mm";
}

// Cuts a vendor list ("Polyester, Metallized") at its first comma, but keeps a
// thousands separator ("1,024") intact.
string cutAtListComma(const string& value) {
  for (size_t at = value.find(','); at != string::npos; at = value.find(',', at + 1)) {
    const bool thousands = at > 0 && isdigit(static_cast<unsigned char>(value[at - 1])) && at + 3 < value.size() &&
                           isdigit(static_cast<unsigned char>(value[at + 1])) &&
                           isdigit(static_cast<unsigned char>(value[at + 2])) &&
                           isdigit(static_cast<unsigned char>(value[at + 3])) &&
                           (at + 4 >= value.size() || !isdigit(static_cast<unsigned char>(value[at + 4])));
    if (!thousands) return trim(value.substr(0, at));
  }
  return value;
}

// Vendor data uses a lone dash or "N/A" for "no value"; treat it as absent,
// matching the stock detail view.
bool isPlaceholderValue(const string& value) {
  const auto trimmed = trim(value);
  return trimmed.empty() || trimmed == "-" || trimmed == "--" || trimmed == "\xE2\x80\x93" ||
         trimmed == "\xE2\x80\x94" || normalizeKey(trimmed) == "na";
}

// Exact-name lookup for short names that the substring matcher confuses,
// such as "RAM" inside "Program Memory Size".
optional<string> exactParameter(const InventoryItem& item, initializer_list<const char*> names) {
  for (const auto* name : names) {
    const auto key = normalizeKey(name);
    for (const auto& parameter : item.parameters) {
      const auto value = trim(parameter.value);
      if (normalizeKey(parameter.name) == key && !isPlaceholderValue(value)) return value;
    }
  }
  return nullopt;
}

// What a tile value must look like. A value that does not carry the expected
// unit is a mismatched parameter, and a blank tile is better than a wrong one.
enum class Unit { Any, Volt, Amp, Watt, Ohm, Hertz, Celsius, Millimetre };

// The positions where the unit letters may start after an optional SI prefix, in the order a regex
// engine tries them: after the prefix when one is present, then at `pos` itself.
vector<size_t> unitStarts(const string& value, size_t pos, const char* prefixes, bool micro) {
  static const char kMicroSign[] = u8"µ";
  vector<size_t> starts;
  if (pos < value.size() && value[pos] != '\0' && strchr(prefixes, value[pos]) != nullptr) starts.push_back(pos + 1);
  if (micro && literalAt(value, pos, kMicroSign)) starts.push_back(pos + 2);
  starts.push_back(pos);
  return starts;
}

// Whether a unit of `unit` follows the digit that precedes `pos`, with the spaces after the digit
// already skipped. The patterns these replace were \d\s*(prefix)?UNIT(?![A-Za-z]) and similar.
bool unitAfterDigit(const string& value, size_t pos, Unit unit) {
  static const char kSiPrefixes[] = "pnumkKMG";
  switch (unit) {
    case Unit::Any:
      return true;
    case Unit::Volt:
      for (const size_t letter : unitStarts(value, pos, kSiPrefixes, true)) {
        if (!literalAt(value, letter, "V")) continue;
        // (?:DC|AC|dc|ac)? is tried with its suffix first, then without it.
        for (const char* suffix : {"DC", "AC", "dc", "ac", ""}) {
          if (literalAt(value, letter + 1, suffix) && !letterAt(value, letter + 1 + strlen(suffix))) return true;
        }
      }
      return false;
    case Unit::Amp:
      for (const size_t letter : unitStarts(value, pos, kSiPrefixes, true)) {
        if (literalAt(value, letter, "A") && !letterAt(value, letter + 1)) return true;
      }
      return false;
    case Unit::Watt:
      for (const size_t letter : unitStarts(value, pos, kSiPrefixes, true)) {
        if (literalAt(value, letter, "W") && !letterAt(value, letter + 1)) return true;
      }
      return false;
    case Unit::Ohm:
      for (const size_t letter : unitStarts(value, pos, kSiPrefixes, true)) {
        if (literalAt(value, letter, u8"Ω") || literalAt(value, letter, "Ohm")) return true;
      }
      return false;
    case Unit::Hertz:
      for (const size_t letter : unitStarts(value, pos, "kKMG", false)) {
        if (literalAt(value, letter, "Hz")) return true;
      }
      return false;
    case Unit::Celsius:
      if (literalAt(value, pos, u8"°") && literalAt(value, skipRegexSpaces(value, pos + 2), "C")) return true;
      return literalAt(value, pos, "C") && !letterAt(value, pos + 1);
    case Unit::Millimetre:
      return literalAt(value, pos, "mm") && !letterAt(value, pos + 2);
  }
  return false;
}

bool hasUnit(const string& value, Unit unit) {
  if (unit == Unit::Any) return true;
  // Every unit pattern starts with a digit, so the digit is the only possible match start. No unit
  // begins with a space, so the greedy run of spaces after a digit is the only choice that can match.
  for (size_t at = 0; at < value.size(); ++at) {
    if (isAsciiDigitChar(value[at]) && unitAfterDigit(value, skipRegexSpaces(value, at + 1), unit)) return true;
  }
  return false;
}

// "Through Hole" and "Surface Mount" describe how a part is fitted, never an
// electrical property; they reach a tile when a loose name match picks up the
// "Mounting Type" parameter.
bool looksLikeMountingValue(const string& value) {
  const auto key = normalizeKey(value);
  if (key == "smd" || key == "smt" || key == "tht") return true;
  for (const char* token : {"throughhole", "surfacemount", "chassismount", "panelmount", "pcmount", "freehanging"}) {
    if (key.find(token) != string::npos) return true;
  }
  return false;
}

// A tile value is printed whole or not at all: a clipped "12.0..." or
// "Through H..." reads as data.
bool fitsTile(const string& value) {
  return estimateFont0Width(value, kLabelTileMinSize, kLabelTileMinSize) <= kLabelTileWidth;
}

// The number of words in a parameter name: "Voltage - Rated (DC)" has three.
size_t nameWordCount(const string& name) {
  size_t count = 0;
  bool inWord = false;
  for (const unsigned char ch : name) {
    if (isalnum(ch)) {
      if (!inWord) ++count;
      inWord = true;
    } else {
      inWord = false;
    }
  }
  return count;
}

// The text of a parameter name after its first `words` words: "Voltage - Rated (DC)" after two words is " (DC)".
string nameTail(const string& name, size_t words) {
  size_t counted = 0;
  for (size_t index = 0; index < name.size(); ++index) {
    if (!isalnum(static_cast<unsigned char>(name[index]))) continue;
    while (index < name.size() && isalnum(static_cast<unsigned char>(name[index]))) ++index;
    if (++counted == words) return name.substr(index);
  }
  return {};
}

// A parameter name that extends the wanted name is the same quantity under a qualifier: "Voltage - Rated (DC)" is
// "Voltage - Rated", and "Current - Average Rectified (Io) (per Diode)" is "Current - Average Rectified (Io)". Only a
// name whose first extra word names another quantity is not: "Voltage - Supply Span (Min)" is a span, not a supply
// voltage.
bool extendsByQualifier(const string& parameterName, const string& wanted) {
  static const initializer_list<const char*> kOtherQuantities = {
      "span", "junction", "offset", "bias", "leakage", "quiescent", "dropout", "standby", "reverse", "forward",
      "peak", "pulse", "clamping", "breakdown", "rise", "fall", "delay", "transition", "switching", "self",
      "stability", "drift", "noise", "threshold", "saturation", "cutoff"};
  auto tail = trim(nameTail(parameterName, nameWordCount(wanted)));
  // The ")" closing a symbol in the wanted name, and a dash, are not part of the qualifier.
  while (!tail.empty() && (tail.front() == ')' || tail.front() == '-')) tail = trim(tail.substr(1));
  if (tail.empty()) return true;
  string word;
  for (const unsigned char ch : tail) {
    if (!isalnum(ch)) break;
    word.push_back(static_cast<char>(tolower(ch)));
  }
  return find_if(kOtherQuantities.begin(), kOtherQuantities.end(), [&](const char* other) { return word == other; }) ==
         kOtherQuantities.end();
}

// A range from its two figures, with the unit printed once as the vendors print it: "3.8V" and "32V" are "3.8 to 32V".
string rangeValue(const string& low, const string& high) {
  const auto unitStart = [](const string& value) {
    const auto last = value.find_last_of("0123456789");
    return last == string::npos ? value.size() : last + 1;
  };
  auto from = trim(low);
  const auto unit = from.substr(unitStart(from));
  if (!unit.empty() && unit == high.substr(unitStart(high))) from = trim(from.substr(0, from.size() - unit.size()));
  return from + " to " + trim(high);
}

class TileCollector {
 public:
  explicit TileCollector(const InventoryItem& item) : item_(item) {}

  // Each parameter feeds at most one tile. Names match exactly first, then, for
  // long names only, as a prefix ("Voltage - Rated" finds "Voltage - Rated (DC)").
  // Short names such as "Type" never match inside longer ones: they would
  // otherwise pick up "Mounting Type".
  void add(const string& caption, initializer_list<const char*> names, Unit unit = Unit::Any) {
    for (const auto* name : names) {
      if (addRange(caption, name, unit)) return;
    }
    for (const bool prefixPass : {false, true}) {
      for (const auto* name : names) {
        const auto wanted = normalizeKey(name);
        if (prefixPass && wanted.size() < 10) continue;
        for (size_t index = 0; index < item_.parameters.size(); ++index) {
          const auto& parameter = item_.parameters[index];
          const auto key = normalizeKey(parameter.name);
          const bool matches = prefixPass ? key.compare(0, wanted.size(), wanted) == 0 && extendsByQualifier(parameter.name, name)
                                          : key == wanted;
          if (!matches || find(used_.begin(), used_.end(), index) != used_.end()) continue;
          const auto value = trim(parameter.value);
          if (isPlaceholderValue(value) || looksLikePackagingValue(value)) continue;
          const auto cleaned = labelTileValue(value);
          if (!acceptable(cleaned, unit)) continue;
          used_.push_back(index);
          addValue(caption, cleaned, unit);
          return;
        }
      }
    }
  }

  // A record that lists a minimum and a maximum as "<name> (Min)" and "<name> (Max)" gives one range, which the tile shows.
  bool addRange(const string& caption, const char* name, Unit unit) {
    const auto low = unusedParameter(string(name) + " (Min)");
    const auto high = unusedParameter(string(name) + " (Max)");
    if (low == string::npos || high == string::npos) return false;
    const auto range = rangeValue(labelTileValue(item_.parameters[low].value), labelTileValue(item_.parameters[high].value));
    if (!acceptable(range, unit)) return false;
    used_.push_back(low);
    used_.push_back(high);
    addValue(caption, range, unit);
    return true;
  }

  // The first parameter with exactly this name that no tile has used, or npos.
  size_t unusedParameter(const string& name) const {
    const auto wanted = normalizeKey(name);
    for (size_t index = 0; index < item_.parameters.size(); ++index) {
      if (normalizeKey(item_.parameters[index].name) == wanted && find(used_.begin(), used_.end(), index) == used_.end()) {
        return index;
      }
    }
    return string::npos;
  }

  void addValue(const string& caption, const string& value, Unit unit = Unit::Any) {
    const auto cleaned = trim(value);
    if (tiles_.size() >= kMaxTiles || !acceptable(cleaned, unit)) {
      return;
    }
    for (const auto& tile : tiles_) {
      if (tile.caption == caption) return;
    }
    tiles_.push_back({caption, cleaned});
  }

  vector<LabelParameterTile> take() { return move(tiles_); }
  bool empty() const { return tiles_.empty(); }

 private:
  static bool acceptable(const string& value, Unit unit) {
    return !isPlaceholderValue(value) && !looksLikeMountingValue(value) && fitsTile(value) && hasUnit(value, unit);
  }

  static constexpr size_t kMaxTiles = 4;
  const InventoryItem& item_;
  vector<size_t> used_;
  vector<LabelParameterTile> tiles_;
};

// Dielectric and film materials by their common abbreviations.
string dielectricTileValue(string value) {
  for (const auto& [name, abbreviation] : {pair<const char*, const char*>{"Polypropylene", "PP"},
                                           {"Polyester", "PET"},
                                           {"Polyphenylene Sulfide", "PPS"},
                                           {"Polyethylene Naphthalate", "PEN"},
                                           {"Polystyrene", "PS"}}) {
    if (normalizeKey(value) == normalizeKey(name)) return abbreviation;
  }
  return value;
}

}  // namespace

bool isMeasuredValueItem(const InventoryItem& item) {
  return !measuredMainValue(item).empty();
}

string mainLabelValue(const InventoryItem& item) {
  if (auto measured = measuredMainValue(item); !measured.empty()) {
    return measured;
  }

  if (categoryContains(item, {"diode", "rectifier", "schottky", "transient voltage suppressor"}) ||
      itemTextContains(item, {"diode", "rectifier", "schottky", "zener"})) {
    return diodeMainLabelValue(item);
  }

  // Distributor descriptions such as "MOSFET N-CH 30V 5A" explain a part,
  // but are not its printable name. For transistors and ICs, preserve the
  // manufacturer's actual part number when DigiKey supplied one.
  if (categoryContains(item, {"transistor", "mosfet", "fet", "discrete semiconductor"}) ||
      itemTextContains(item, {"mosfet", "trans npn", "trans pnp", "bjt transistor"}) || isIcLikeItem(item)) {
    if (!trim(item.vendorMetadata.manufacturerPartNumber).empty()) return trim(item.vendorMetadata.manufacturerPartNumber);
    if (!trim(item.sku).empty()) return trim(item.sku);
  }

  // A part still named by its supplier's description (TME and DigiKey both store it as the title) prints its part
  // number, which is short and identifies the part. A name the user typed is printed as it is.
  const auto partNumber = trim(item.vendorMetadata.manufacturerPartNumber);
  if (!partNumber.empty() && trim(item.partName) == trim(item.vendorMetadata.title)) {
    return partNumber;
  }

  if (!trim(item.partName).empty()) {
    return item.partName;
  }
  if (!trim(item.sku).empty()) {
    return item.sku;
  }
  return item.category;
}

string mainLabelTolerance(const InventoryItem& item) {
  if (!isMeasuredValueItem(item)) {
    return {};
  }
  const auto tolerance = firstParameter(item, {"Tolerance"});
  if (!tolerance || isPlaceholderValue(*tolerance)) {
    return {};
  }
  auto value = labelTileValue(*tolerance);
  // An asymmetric tolerance ("-20 to 80%", as TME writes it) reads as its two limits: "-20/+80%".
  if (const auto to = value.find(" to "); to != string::npos && !value.empty() && value.front() == '-') {
    value = trim(value.substr(0, to)) + "/+" + trim(value.substr(to + 4));
  }
  value.erase(remove(value.begin(), value.end(), ' '), value.end());
  return fieldOrBlank(value, 8);
}

// The package line is at most 12 characters and is never clipped. A name that fits is printed whole, one that does not
// is cut to its first word ("14-VFDFN Exposed Pad" is "14-VFDFN"), and when no word fits the line is left off.
string wholePackageWord(const string& value) {
  constexpr size_t kPackageLineWidth = 12;
  auto cleaned = trim(value);
  const auto cut = cleaned.find_first_of(",;(/");
  if (cut != string::npos) cleaned = trim(cleaned.substr(0, cut));
  if (cleaned.size() <= kPackageLineWidth) return cleaned;
  const auto space = cleaned.find(' ');
  if (space != string::npos && space <= kPackageLineWidth) return cleaned.substr(0, space);
  return {};
}

// A size that does not fit reads as its footprint in millimetres: "6.6x5.1x1.5mm" is "6.6x5.1mm", its length and width.
string footprintSize(const string& value) {
  const auto text = trim(value);
  if (text.find("mm") == string::npos) return {};
  vector<string> numbers;
  string current;
  for (const char ch : text) {
    if (isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
      current += ch;
    } else if (!current.empty()) {
      numbers.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) numbers.push_back(current);
  if (numbers.size() < 2) return {};
  const auto footprint = numbers[0] + "x" + numbers[1] + "mm";
  return footprint.size() <= 12 ? footprint : string();
}

// The same package is often written two ways: DigiKey's JEDEC "DO-204AL" is the "DO-41" that TME prints, and TME's
// "TSOT26" is the "TSOT-23-6" in DigiKey's record. Both vendors print the one name in this table. A row is added when
// two vendors name one package differently.
string canonicalPackageName(const string& name) {
  static const vector<pair<string, string>> names = {
      {"do204al", "DO-41"}, {"do41", "DO-41"}, {"sot23", "SOT-23"}, {"tsot26", "TSOT-23-6"}};
  const auto key = normalizeKey(name);
  for (const auto& [alias, canonical] : names) {
    if (key == alias) return canonical;
  }
  return name;
}

// A cylinder is printed by its diameter, which both vendors give: TME's "Ø5x11mm" and DigiKey's "0.197" Dia (5.00mm)"
// are "Ø5mm". DigiKey gives no body length, so the length is left out for both.
string cylinderLine(const string& value) {
  const auto text = trim(value);
  const auto symbol = text.find(u8"Ø");
  if (symbol == string::npos && text.find("Dia") == string::npos) return {};
  string diameter;
  if (!findBracketedMetric(text, diameter)) {
    if (symbol == string::npos) return {};
    const auto start = symbol + strlen(u8"Ø");
    size_t end = start;
    while (end < text.size() && (isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) ++end;
    diameter = text.substr(start, end - start);
  }
  diameter = trimTrailingZeros(diameter);
  if (diameter.empty()) return {};
  return u8"Ø" + diameter + "mm";
}

string shortPackageLine(const InventoryItem& item) {
  // The supplier package name is usually the one printed on reels and in
  // footprints (SOT-23, SMA); the case name lists several aliases.
  for (const auto names : {initializer_list<const char*>{"Supplier Device Package", "Device Package"},
                           initializer_list<const char*>{"Package / Case", "Package Case", "Case / Package",
                                                         "Case Package", "Package"}}) {
    auto package = firstParameter(item, names);
    if (package) {
      replaceAll(*package, u8"®", "");
      replaceAll(*package, u8"™", "");
    }
    if (package && !isNonstandardPackage(*package)) {
      const auto fitted = canonicalPackageName(wholePackageWord(*package));
      if (!fitted.empty() && !isNonstandardPackage(fitted)) {
        return fitted;
      }
    }
  }
  if (const auto size = firstParameter(item, {"Size / Dimension", "Dimensions"})) {
    if (auto cylinder = cylinderLine(*size); !cylinder.empty()) {
      return cylinder;
    }
    if (auto metric = wholePackageWord(metricSizeLine(*size)); !metric.empty()) {
      return metric;
    }
    if (auto whole = wholePackageWord(*size); !whole.empty()) {
      return whole;
    }
    return footprintSize(*size);
  }
  return {};
}

string manufacturerLine(const InventoryItem& item) {
  return fieldOrBlank(trim(item.manufacturer), 64);
}

vector<string> manufacturerWordCandidates(const string& name) {
  static const initializer_list<const char*> kCorporateWords = {
      "inc", "incorporated", "corp", "corporation", "co", "ltd", "limited", "llc", "gmbh", "ag", "sa", "plc",
      "technologies", "technology", "electronics", "electronic", "semiconductor", "semiconductors", "semicon", "components",
      "industry", "industries", "international", "group", "systems", "devices", "microelectronics", "company",
      "instruments", "usa", "america", "americas", "europe", "asia", "us", "uk"};
  vector<string> candidates;
  const auto full = fieldOrBlank(trim(name), 32);
  if (full.empty()) return candidates;
  candidates.push_back(full);
  vector<string> words;
  istringstream stream(full);
  for (string word; stream >> word;) words.push_back(word);
  const auto isCorporateWord = [&](string word) {
    word = normalizeKey(word);
    return any_of(kCorporateWords.begin(), kCorporateWords.end(), [&](const char* known) { return word == known; });
  };
  // "Infineon Technologies" -> "Infineon": drop trailing corporate words one at
  // a time, always keeping the leading name.
  while (words.size() > 1 && isCorporateWord(words.back())) {
    words.pop_back();
    auto& last = words.back();
    while (!last.empty() && (last.back() == ',' || last.back() == '.')) last.pop_back();
    candidates.push_back(join(words, ' '));
  }
  // "Shenzhen Slkormicro" -> "Slkormicro": a leading city is not the brand.
  static const initializer_list<const char*> kLocations = {"shenzhen", "guangdong", "shanghai", "taiwan", "china",
                                                           "hong", "kong", "dongguan"};
  while (words.size() > 1 && any_of(kLocations.begin(), kLocations.end(),
                                    [&](const char* place) { return normalizeKey(words.front()) == place; })) {
    words.erase(words.begin());
    candidates.push_back(join(words, ' '));
  }
  // Last resort before cutting characters: the leading word alone.
  const auto firstWord = words.front();
  if (words.size() > 1 && firstWord.size() >= 3 && firstWord != candidates.back()) candidates.push_back(firstWord);
  return candidates;
}

// The word cuts, then the short brand names ("ST" for STMicroelectronics). The short names come last, after every
// word cut has been tried.
vector<string> manufacturerCandidates(const string& name) {
  auto candidates = manufacturerWordCandidates(name);
  const auto full = fieldOrBlank(trim(name), 32);
  for (const auto& [long_name, short_name] : {pair<const char*, const char*>{"stmicroelectronics", "ST"},
                                              {"texasinstruments", "TI"},
                                              {"analogdevices", "ADI"}}) {
    if (normalizeKey(full).rfind(long_name, 0) == 0) candidates.push_back(short_name);
  }
  return candidates;
}

string normalizedShieldingLine(const optional<string>& value) {
  if (!value) {
    return {};
  }
  const auto cleaned = trim(*value);
  if (cleaned.empty()) {
    return {};
  }

  const auto normalized = normalizeKey(cleaned);
  if (normalized == "yes" || normalized == "true" || normalized == "shielded") {
    return "Shielded";
  }
  if (normalized == "no" || normalized == "false" || normalized == "unshielded") {
    return "Unshielded";
  }
  if (normalized.find("semi") != string::npos) {
    return "Semi";
  }
  return fitSingleLineLabel(cleaned, 16);
}

string labelTileValue(const string& value) {
  auto text = trim(value);
  replaceAll(text, u8"®", "");
  replaceAll(text, u8"™", "");
  // Vendors often give inches with millimetres in brackets; keep the metric value.
  string bracketed;
  if (text.find('"') != string::npos && findBracketedMetric(text, bracketed)) {
    text = trimTrailingZeros(bracketed) + "mm";
  }
  text = cutAt(text, " @ ");
  text = cutAtListComma(text);
  text = cutAt(text, " (");
  text = cutAt(text, "(");
  replaceAll(text, "Ohms", u8"Ω");
  replaceAll(text, "Ohm", u8"Ω");
  // "~" is the ZPL control prefix, so ranges print as "from to to" with the
  // shared unit once: "-40°C ~ 85°C" -> "-40 to 85°C". A hyphen would read as
  // a minus sign next to negative limits ("-40-85°C").
  if (const auto tilde = text.find('~'); tilde != string::npos) {
    auto low = trim(text.substr(0, tilde));
    const auto high = trim(text.substr(tilde + 1));
    const auto unitStart = [](const string& value) {
      const auto last = value.find_last_of("0123456789");
      return last == string::npos ? value.size() : last + 1;
    };
    const auto lowUnit = low.substr(unitStart(low));
    if (!lowUnit.empty() && lowUnit == high.substr(unitStart(high))) {
      low = trim(low.substr(0, low.size() - lowUnit.size()));
    }
    text = low + " to " + high;
  }
  for (const auto* qualifier : {" Max", " Typ", " Min"}) {
    if (endsWith(text, qualifier)) text = trim(text.substr(0, text.size() - strlen(qualifier)));
  }
  // "20V DC" and "5VDC" are the voltage the unit already gives. The qualifier only makes a supply range too wide for
  // its tile, and the two vendors write it with and without a space. "AC/DC" keeps its qualifier.
  for (const auto* qualifier : {"DC", "AC"}) {
    if (!endsWith(text, qualifier)) continue;
    const auto rest = trim(text.substr(0, text.size() - strlen(qualifier)));
    if (!rest.empty() && (rest.back() == 'V' || rest.back() == 'A')) text = rest;
  }
  if (startsWithInsensitive(text, "ARM ") && text.size() > 4) {
    text = trim(text.substr(4));
  }
  return sanitiseZplFragment(normalizedUnitValue(text));
}

void splitMeasuredValue(const string& value, string& number, string& unit) {
  number = trim(value);
  unit.clear();
  for (const auto& suffix : {string(u8"Ω"), string("F"), string("H")}) {
    if (number.size() > suffix.size() && endsWith(number, suffix)) {
      unit = suffix;
      number = trim(number.substr(0, number.size() - suffix.size()));
      return;
    }
  }
}

void splitRackLocation(const string& location, string& rackCode, string& rackCell) {
  const auto cleaned = trim(location);
  const auto dash = cleaned.rfind('-');
  if (dash == string::npos || dash == 0 || dash + 1 >= cleaned.size()) {
    rackCode = cleaned;
    rackCell.clear();
    return;
  }
  rackCode = trim(cleaned.substr(0, dash));
  rackCell = toUpper(trim(cleaned.substr(dash + 1)));
}

namespace {

// What a field's label says it measures: the order that fields of a part without specific tiles are shown in, and the unit
// its value must carry. The order follows meaning, so one part gives the same tiles from either vendor's record, whatever
// order that record lists its parameters in. A rank of -1 marks a field that describes the part rather than how it
// behaves (a lens, a size, a packaging or a status): such a field never reaches a tile.
struct FieldConcept {
  int rank = 13;
  // A value without the unit is another figure under this label: a tolerance ("±20ppm") under "Frequency" is left off.
  Unit unit = Unit::Any;
  // A colour is printed with a capital first letter, as DigiKey writes it: "green" is "Green".
  bool colour = false;
};

FieldConcept fieldConcept(const string& label) {
  const auto key = normalizeKey(label);
  const auto has = [&](initializer_list<const char*> parts) {
    return any_of(parts.begin(), parts.end(), [&](const char* part) { return key.find(part) != string::npos; });
  };
  if (has({"lens", "feature", "dimension", "size", "height", "length", "width", "thickness", "package", "packag",
           "mounting", "series", "rohs", "lead", "status", "quantity", "price", "roll", "reel", "datasheet", "gender",
           "specification", "application", "ingress", "matingcycle", "lifetime", "front", "shape", "style", "approval",
           "grade", "certification"})) {
    return {-1, Unit::Any};
  }
  if (has({"color", "colour"})) return {1, Unit::Any, true};
  if ((has({"forward"}) && has({"voltage"})) || key == "vf") return {2, Unit::Volt};
  if (has({"luminous", "luminosity", "millicandela", "candela", "intensity"})) return {3, Unit::Any};
  if (has({"viewing", "angle"})) return {4, Unit::Any};
  if (has({"current", "ampere", "amps"})) return {5, Unit::Amp};
  if (has({"voltage"})) return {6, Unit::Volt};
  if (has({"power", "watt"})) return {7, Unit::Watt};
  if (has({"impedance"})) return {8, Unit::Ohm};
  if (has({"resistance"})) return {9, Unit::Ohm};
  if (has({"capacitance", "inductance"})) return {9, Unit::Any};
  if (has({"frequency", "clock"})) return {10, Unit::Hertz};
  if (has({"speed", "wavelength", "resonan"})) return {10, Unit::Any};
  if (has({"temperature", "tempco"})) return {11, Unit::Celsius};
  if (has({"type", "function", "technology", "composition", "dielectric", "material", "channel", "polarity",
           "configuration", "circuit", "output", "termination", "mode"})) {
    return {12, Unit::Any};
  }
  return {13, Unit::Any};
}

// A caption is printed whole or not at all: one the layout would have to cut short is left off the label.
bool captionIsWhole(const string& caption) {
  const auto fitted = fitFont0Text(toUpper(caption), kLabelTileWidth, {12, 11, 10});
  return !fitted.text.empty() && !endsWith(fitted.text, "...");
}

}  // namespace

vector<LabelParameterTile> fallbackParameterTiles(const InventoryItem& item, size_t maxTiles) {
  struct Candidate {
    int rank = 0;
    string caption;
    string value;
  };
  vector<Candidate> candidates;
  const auto main = mainLabelValue(item);
  for (const auto& field : electricalFieldsForItem(item)) {
    const auto meaning = fieldConcept(field.label);
    if (meaning.rank < 0) continue;
    auto value = labelTileValue(field.value);
    if (isPlaceholderValue(value) || value == main || looksLikeMountingValue(value) || !fitsTile(value) ||
        !hasUnit(value, meaning.unit)) {
      continue;
    }
    if (meaning.colour) value[0] = static_cast<char>(toupper(static_cast<unsigned char>(value[0])));
    auto caption = toUpper(shortParameterLabel(field.label));
    while (!caption.empty() && (caption.back() == ':' || caption.back() == ' ')) caption.pop_back();
    if (caption.empty() || !captionIsWhole(caption)) continue;
    candidates.push_back({meaning.rank, caption, value});
  }
  // By concept, then by caption and value; the order the record lists its fields in never decides the tiles.
  sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
    if (left.rank != right.rank) return left.rank < right.rank;
    if (left.caption != right.caption) return left.caption < right.caption;
    return left.value < right.value;
  });
  vector<LabelParameterTile> tiles;
  for (const auto& candidate : candidates) {
    if (tiles.size() >= maxTiles) break;
    const bool captionUsed = any_of(tiles.begin(), tiles.end(),
                                    [&](const LabelParameterTile& tile) { return tile.caption == candidate.caption; });
    if (!captionUsed) tiles.push_back({candidate.caption, candidate.value});
  }
  return tiles;
}

namespace {

// Name and category only: a parameter such as "Synchronous Rectifier=Yes" on a
// buck converter must not make it a diode.
bool nameMentions(const InventoryItem& item, initializer_list<const char*> needles) {
  const auto text = item.partName + " " + item.category;
  return any_of(needles.begin(), needles.end(), [&](const char* needle) { return containsInsensitive(text, needle); });
}

// The rules run in order and the first one that yields a tile decides. A rule that matches the part by its text but
// finds no tile of its own does not end the search: the rules after it still apply, and then the fallback fields. The
// MOSFET rule is the exception, since a MOSFET's tiles must not be replaced by a bipolar transistor's.
vector<LabelParameterTile> specificParameterTiles(const InventoryItem& item) {
  TileCollector tiles(item);

  if (categoryContains(item, {"capacitor"})) {
    tiles.add("VOLTAGE", {"Voltage - Rated", "Rated Voltage", "Operating Voltage", "Voltage"}, Unit::Volt);
    // Ceramics state X7R/C0G as the temperature coefficient and film types
    // name their material; electrolytics have no dielectric tile at all.
    tiles.add("DIELECTRIC", {"Temperature Coefficient"});
    if (const auto material = exactParameter(item, {"Dielectric Material"})) {
      tiles.addValue("DIELECTRIC", dielectricTileValue(labelTileValue(*material)));
    }
    tiles.add("DIELECTRIC", {"Dielectric", "Dielectric Type"});
    tiles.add("TEMP", {"Operating Temperature"}, Unit::Celsius);
    tiles.add("ESR", {"ESR (Equivalent Series Resistance)", "ESR"}, Unit::Ohm);
    tiles.add("HEIGHT", {"Height - Seated (Max)", "Thickness (Max)"}, Unit::Millimetre);
    if (!tiles.empty()) return tiles.take();
  }

  if (categoryContains(item, {"resistor"})) {
    tiles.add("POWER", {"Power (Watts)", "Power Dissipation", "Power Rating", "Power - Max", "Power", "Watts"}, Unit::Watt);
    tiles.add("TEMPCO", {"Temperature Coefficient", "Tempco"});
    tiles.add("TYPE", {"Composition"});
    tiles.add("MAX V", {"Voltage - Max", "Max Working Voltage", "Voltage Rating"}, Unit::Volt);
    if (!tiles.empty()) return tiles.take();
  }

  if (categoryContains(item, {"inductor", "choke", "coil"})) {
    tiles.add("RATED", {"Current Rating (Amps)", "Current Rating", "Current"}, Unit::Amp);
    tiles.add("ISAT", {"Current - Saturation (Isat)", "Saturation Current", "Current - Saturation"}, Unit::Amp);
    tiles.add("DCR", {"DC Resistance (DCR)", "DC Resistance", "DCR"}, Unit::Ohm);
    tiles.addValue("SHIELD", normalizedShieldingLine(firstParameter(item, {"Shielding"})));
    tiles.add("SRF", {"Frequency - Self Resonant"}, Unit::Hertz);
    if (!tiles.empty()) return tiles.take();
  }

  // A part is a microcontroller by its family or by its name; TME files its microcontrollers under the family of ICs.
  if (categoryContains(item, {"mcu", "microcontroller"}) || nameMentions(item, {"mcu", "microcontroller"})) {
    tiles.add("CORE", {"Core Processor", "Core"});
    tiles.add("CLOCK", {"Clock Speed", "Clock Frequency", "Speed"}, Unit::Hertz);
    const auto flash = exactParameter(item, {"Program Memory Size", "Flash", "Flash Size"});
    const auto ram = exactParameter(item, {"RAM Size", "RAM"});
    if (flash && ram) {
      tiles.addValue("FLASH / RAM", cutAt(labelTileValue(*flash), " x ") + "/" + cutAt(labelTileValue(*ram), " x "));
    } else if (flash) {
      tiles.addValue("FLASH", cutAt(labelTileValue(*flash), " x "));
    }
    tiles.add("SUPPLY", {"Voltage - Supply (Vcc/Vdd)", "Voltage - Supply", "Operating Voltage", "Supply Voltage"}, Unit::Volt);
    if (!tiles.empty()) return tiles.take();
  }

  // DigiKey files MOSFETs, BJTs and diodes alike under "Discrete Semiconductor
  // Products", so the kind comes from the parameters a part carries.
  const bool isFet = categoryContains(item, {"mosfet", "fet"}) ||
                     exactParameter(item, {"FET Type", "Drain to Source Voltage (Vdss)"}).has_value();
  const bool isBjt = categoryContains(item, {"transistor"}) ||
                     exactParameter(item, {"Transistor Type", "Voltage - Collector Emitter Breakdown (Max)"}).has_value();
  const bool isTvs = categoryContains(item, {"tvs", "transient voltage suppressor"}) ||
                     nameMentions(item, {"tvs", "transient voltage"}) ||
                     exactParameter(item, {"Voltage - Reverse Standoff (Typ)", "Voltage - Clamping (Max) @ Ipp"}).has_value();
  const bool isDiode = !isTvs && (categoryContains(item, {"diode", "rectifier", "schottky"}) ||
                                  nameMentions(item, {"diode", "rectifier", "schottky", "zener"}) ||
                                  exactParameter(item, {"Voltage - DC Reverse (Vr) (Max)", "Voltage - Zener (Nom) (Vz)"})
                                      .has_value());
  const bool isFuse = !isTvs && (categoryContains(item, {"fuse"}) || nameMentions(item, {"fuse"}) ||
                                 exactParameter(item, {"Fuse Type"}).has_value());
  if (isFet || isBjt || (!isDiode && !isTvs && !isFuse && categoryContains(item, {"discrete semiconductor"}))) {
    if (isFet) {
      tiles.add("VDS", {"Drain to Source Voltage (Vdss)", "Drain-Source Voltage", "Vdss", "Vds"}, Unit::Volt);
      tiles.add("ID", {"Current - Continuous Drain (Id) @ 25°C", "Continuous Drain Current"}, Unit::Amp);
      tiles.add("RDS(ON)", {"Rds On (Max) @ Id, Vgs", "Rds On", "RDS(ON)"}, Unit::Ohm);
      tiles.add("VGS(TH)", {"Vgs(th) (Max) @ Id", "Gate Threshold Voltage", "Vgs(th)"}, Unit::Volt);
      tiles.add("QG", {"Gate Charge (Qg) (Max) @ Vgs", "Gate Charge"});
      return tiles.take();
    }

    tiles.add("VCE", {"Voltage - Collector Emitter Breakdown (Max)", "Collector-Emitter Voltage",
                      "Collector Emitter Voltage", "Vceo", "Vce"}, Unit::Volt);
    tiles.add("IC", {"Current - Collector (Ic) (Max)", "Collector Current", "Continuous Collector Current"}, Unit::Amp);
    tiles.add("GAIN", {"DC Current Gain (hFE) (Min) @ Ic, Vce", "DC Current Gain", "hFE", "Current gain"});
    tiles.add("POWER", {"Power - Max", "Power Dissipation"}, Unit::Watt);
    if (!tiles.empty()) return tiles.take();
  }

  if (isFuse) {
    tiles.add("RATED", {"Current Rating (Amps)", "Current Rating"}, Unit::Amp);
    tiles.add("VDC", {"Voltage Rating - DC", "Voltage - DC"}, Unit::Volt);
    tiles.add("VAC", {"Voltage Rating - AC", "Voltage - AC"}, Unit::Volt);
    tiles.add("SPEED", {"Response Time"});
    // TME names a fuse's limits in plain words and has no response time. These fill the free tiles of such a fuse.
    tiles.add("VMAX", {"Max. voltage", "Maximum voltage"}, Unit::Volt);
    tiles.add("IMAX", {"Maximum current"}, Unit::Amp);
    tiles.add("POWER", {"Power"}, Unit::Watt);
    if (!tiles.empty()) return tiles.take();
  }

  if (isTvs || categoryContains(item, {"circuit protection"}) ||
      itemTextContains(item, {"tvs", "transient voltage suppressor", "surge protection", "esd protection"})) {
    if (hasParameter(item, {"Voltage - Reverse Standoff (Typ)", "Reverse Standoff"})) {
      tiles.add("VST", {"Voltage - Reverse Standoff (Typ)", "Reverse Standoff"}, Unit::Volt);
    } else {
      tiles.add("VBR", {"Voltage - Breakdown (Min)", "Breakdown"}, Unit::Volt);
    }
    tiles.add("VC", {"Voltage - Clamping (Max) @ Ipp", "Clamping"}, Unit::Volt);
    tiles.add("IPP", {"Current - Peak Pulse (10/1000µs)", "Current - Peak Pulse (10/1000Âµs)", "Peak Pulse Current",
                      "Current Rating", "Current"}, Unit::Amp);
    tiles.add("PPP", {"Power - Peak Pulse", "Peak Pulse Power"}, Unit::Watt);
    if (!tiles.empty()) return tiles.take();
  }

  if (isDiode) {
    tiles.add("VZ", {"Voltage - Zener (Nom) (Vz)", "Zener Voltage"}, Unit::Volt);
    tiles.add("VR", {"Voltage - DC Reverse (Vr) (Max)", "Reverse Voltage", "Peak Reverse Voltage", "Vr"}, Unit::Volt);
    tiles.add("IO", {"Current - Average Rectified (Io)", "Forward Current", "Current"}, Unit::Amp);
    tiles.add("VF", {"Voltage - Forward (Vf) (Max) @ If", "Forward Voltage", "Vf"}, Unit::Volt);
    // Technology says which diode it is (Schottky or standard); it outranks the surge rating when four tiles fit.
    tiles.add("TECH", {"Technology"});
    tiles.add("IFSM", {"Current - Non Rep. Surge 50, 60Hz (Ifsm)", "Ifsm"}, Unit::Amp);
    if (!tiles.empty()) return tiles.take();
  }

  if (categoryContains(item, {"connector"}) || itemTextContains(item, {"connector"})) {
    tiles.add("PINS", {"Number of Positions", "Pins", "Pin Count"});
    tiles.add("TYPE", {"Connector Type"});
    tiles.add("ROWS", {"Number of Rows", "Rows"});
    tiles.add("PITCH", {"Pitch - Mating", "Pitch"});
    if (!tiles.empty()) return tiles.take();
  }

  // A regulator's topology is its own parameter ("buck", "boost-buck", "LDO"), and so is the word its description gives.
  const auto topology = exactParameter(item, {"Topology"});
  const bool regulatorTopology =
      topology && (containsInsensitive(*topology, "buck") || containsInsensitive(*topology, "boost") ||
                   containsInsensitive(*topology, "ldo") || containsInsensitive(*topology, "linear"));
  if (categoryContains(item, {"regulator", "voltage regulator", "power management"}) || regulatorTopology ||
      itemTextContains(item, {"regulator", "ldo", "buck", "boost", "dc/dc", "pmic"})) {
    tiles.add("VOUT", {"Voltage - Output (Min/Fixed)", "Voltage - Output", "Output Voltage", "Vout"}, Unit::Volt);
    tiles.add("VIN", {"Voltage - Input (Max)", "Voltage - Input", "Vin"}, Unit::Volt);
    tiles.add("IOUT", {"Current - Output", "Output Current", "Iout"}, Unit::Amp);
    tiles.add("TYPE", {"Output Type", "Type"});
    if (!tiles.empty()) return tiles.take();
  }

  if (categoryContains(item, {"crystal", "oscillator", "resonator"}) ||
      itemTextContains(item, {"crystal", "oscillator", "resonator"})) {
    tiles.add("FREQ", {"Frequency"}, Unit::Hertz);
    tiles.add("LOAD C", {"Load Capacitance"});
    tiles.add("ESR", {"ESR (Equivalent Series Resistance)", "Equivalent Series Resistance", "ESR"}, Unit::Ohm);
    tiles.add("TOL", {"Frequency Tolerance", "Frequency Stability"});
    tiles.add("TEMP", {"Operating Temperature"}, Unit::Celsius);
    if (!tiles.empty()) return tiles.take();
  }

  if (categoryContains(item, {"sensor", "temperature sensor", "pressure sensor"}) ||
      itemTextContains(item, {"sensor", "imu"})) {
    tiles.add("TYPE", {"Sensor Type", "Type"});
    tiles.add("OUTPUT", {"Output Type", "Output"});
    tiles.add("SUPPLY", {"Voltage - Supply"}, Unit::Volt);
    tiles.add("RES", {"Resolution"});
    if (!tiles.empty()) return tiles.take();
  }

  if (categoryContains(item, {"switch"}) || nameMentions(item, {"switch"})) {
    tiles.add("CIRCUIT", {"Circuit"});
    tiles.add("FUNCTION", {"Switch Function"});
    tiles.add("RATING", {"Contact Rating @ Voltage", "Contact Rating"});
    tiles.add("ACTUATOR", {"Actuator Type"});
    if (!tiles.empty()) return tiles.take();
  }

  // Timers, oscillators and other ICs without a dedicated list: supply, speed,
  // current and temperature are what a drawer label needs.
  if (isIcLikeItem(item)) {
    // DigiKey gives an amplifier's supply as a span, "Voltage - Supply Span (Min)" and "(Max)", which reads as one range.
    tiles.add("SUPPLY", {"Voltage - Supply (Vcc/Vdd)", "Voltage - Supply", "Voltage - Supply Span", "Supply Voltage",
                         "Operating Voltage"}, Unit::Volt);
    tiles.add("FREQ", {"Frequency", "Clock Frequency"}, Unit::Hertz);
    tiles.add("CURRENT", {"Current - Supply", "Current - Output"}, Unit::Amp);
    tiles.add("TEMP", {"Operating Temperature"}, Unit::Celsius);
    if (!tiles.empty()) return tiles.take();
  }

  return {};
}

}  // namespace

vector<LabelParameterTile> parameterTilesForItem(const InventoryItem& item) {
  auto tiles = specificParameterTiles(item);
  return tiles.empty() ? fallbackParameterTiles(item, 4) : tiles;
}

}  // namespace label_printer_detail
}  // namespace inventatory
