// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/bom/BomVendorMatch.h"

#include "BomMatchPrivate.h"
#include "core/inventory/VendorProvider.h"
#include "core/parts/DecimalParse.h"
#include "core/text/Voice.h"
#include "core/vendor/VendorPurchase.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

namespace inventatory {

using namespace std;
using namespace bom_match_detail;

namespace {

constexpr const char* kMicro = "\xC2\xB5";  // µ, as DigiKey writes it

// ---------- text helpers ----------

// Numbers in search text and in matches are written the same in every locale. printf and to_string follow the
// locale, so a comma-decimal locale would turn 0.33 into "0,33" and the vendor text would not match.
string formatNumber(double value) {
  ostringstream text;
  text.imbue(locale::classic());
  text << setprecision(6) << value;  // the digits of "%.6g"
  return text.str();
}

string fixedDigits(double value, int digits) {
  ostringstream text;
  text.imbue(locale::classic());
  text << fixed << setprecision(digits) << value;
  return text.str();
}

// A number the pattern has already matched. The classic locale reads "2.50" as 2.5 in every locale.
double decimalOf(const string& text) {
  double value = 0;
  parseClassicDecimal(text, value);
  return value;
}

// Upper-case letters and digits only: "MCP2515-I/SO" -> "MCP2515ISO".
string normPart(const string& value) {
  string out;
  for (const unsigned char ch : value) {
    if (isalnum(ch) != 0) out.push_back(static_cast<char>(toupper(ch)));
  }
  return out;
}

bool containsInsensitive(const string& text, const string& needle) {
  return !needle.empty() && toLower(text).find(toLower(needle)) != string::npos;
}

bool startsWith(const string& text, const string& prefix) { return text.rfind(prefix, 0) == 0; }

bool closeTo(optional<double> a, double b, double tolerance) {
  if (!a.has_value()) return false;
  return fabs(*a - b) <= tolerance * max({fabs(*a), fabs(b), 1e-15});
}

// ---------- standard series ----------

constexpr double kE96[] = {1.00, 1.02, 1.05, 1.07, 1.10, 1.13, 1.15, 1.18, 1.21, 1.24, 1.27, 1.30, 1.33, 1.37, 1.40,
                           1.43, 1.47, 1.50, 1.54, 1.58, 1.62, 1.65, 1.69, 1.74, 1.78, 1.82, 1.87, 1.91, 1.96, 2.00,
                           2.05, 2.10, 2.15, 2.21, 2.26, 2.32, 2.37, 2.43, 2.49, 2.55, 2.61, 2.67, 2.74, 2.80, 2.87,
                           2.94, 3.01, 3.09, 3.16, 3.24, 3.32, 3.40, 3.48, 3.57, 3.65, 3.74, 3.83, 3.92, 4.02, 4.12,
                           4.22, 4.32, 4.42, 4.53, 4.64, 4.75, 4.87, 4.99, 5.11, 5.23, 5.36, 5.49, 5.62, 5.76, 5.90,
                           6.04, 6.19, 6.34, 6.49, 6.65, 6.81, 6.98, 7.15, 7.32, 7.50, 7.68, 7.87, 8.06, 8.25, 8.45,
                           8.66, 8.87, 9.09, 9.31, 9.53, 9.76};
constexpr double kE24[] = {1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                           3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1};

vector<double> seriesFor(BomPartKind kind) {
  vector<double> series(begin(kE24), end(kE24));
  if (kind == BomPartKind::Resistor) series.insert(series.end(), begin(kE96), end(kE96));
  return series;
}

// ---------- footprint reading ----------

struct IcPackage {
  string family;
  int pins = 0;
};

// The IC package a KiCad footprint names: "SOIC-8_3.9x4.9mm" -> SOIC 8, "D_SOD-123" -> SOD-123.
IcPackage icPackageOf(const string& footprint) {
  string fp = startsWith(footprint, "D_") ? footprint.substr(2) : footprint;
  smatch m;
  static const regex fixed(R"(^(TSOT-23-6|SOT-23-[56]|SOT-23|SOT-223|SOT-89|SOD-123F?|SOD-323|SOD-523|SMA|SMB|SMC|TO-263|TO-252|TO-220|TO-92)(?![A-Za-z0-9]))");
  if (regex_search(fp, m, fixed)) {
    string family = m[1];
    if (family == "SOD-123F") family = "SOD-123";
    return {family, 0};
  }
  static const regex pinned(R"(^(HTSSOP|TSSOP|SSOP|SOIC|SOP|TQFP|LQFP|QFN|VQFN|WQFN|VSSOP|MSOP|VSON|WSON|DFN|SON)-(\d+))");
  if (regex_search(fp, m, pinned)) return {m[1], stoi(m[2])};
  // IPC names: SOP65P640X120-29N is a 28-pin TSSOP with a thermal pad counted as pin 29.
  static const regex ipc(R"(^SOP65P\d+X\d+-(\d+)N)");
  if (regex_search(fp, m, ipc)) {
    const int pins = stoi(m[1]);
    return {"TSSOP", pins % 2 == 1 ? pins - 1 : pins};
  }
  return {};
}

struct PackageAlias {
  const char* family;
  vector<const char*> words;
};

const vector<PackageAlias>& fixedPackageAliases() {
  static const vector<PackageAlias> aliases = {
      {"SOT-23-5", {"SOT-23-5", "SC-74A", "SOT-753"}},
      {"SOT-23-6", {"SOT-23-6", "SC-74", "SOT-26"}},
      {"TSOT-23-6", {"TSOT-23-6", "SOT-23-6", "TSOT-6", "SOT-563"}},
      {"SOT-223", {"SOT-223", "TO-261"}},
      {"SOT-89", {"SOT-89", "TO-243"}},
      {"SOD-123", {"SOD-123"}},
      {"SOD-323", {"SOD-323", "SC-76", "SC-90"}},
      {"SOD-523", {"SOD-523", "SC-79"}},
      {"SMA", {"DO-214AC", "SMA"}},
      {"SMB", {"DO-214AA", "SMB"}},
      {"SMC", {"DO-214AB", "SMC"}},
      {"TO-263", {"TO-263", "D2PAK", "D\xC2\xB2PAK"}},
      {"TO-252", {"TO-252", "DPAK"}},
      {"TO-220", {"TO-220"}},
      {"TO-92", {"TO-92", "TO-226"}},
  };
  return aliases;
}

vector<string> pinFamilyWords(const string& family) {
  if (family == "HTSSOP") return {"HTSSOP", "TSSOP", "PowerTSSOP"};
  if (family == "TSSOP") return {"TSSOP", "HTSSOP", "PowerTSSOP"};
  if (family == "SOIC") return {"SOIC", "SO"};
  if (family == "SOP") return {"SOP", "SOIC"};
  if (family == "TQFP") return {"TQFP", "QFP"};
  if (family == "LQFP") return {"LQFP", "QFP"};
  if (family == "VSSOP") return {"VSSOP", "MSOP", "TSSOP"};
  if (family == "MSOP") return {"MSOP", "VSSOP"};
  if (family == "VSON" || family == "WSON" || family == "SON") return {"VSON", "WSON", "SON", "DFN"};
  if (family == "QFN" || family == "VQFN" || family == "WQFN") return {"QFN", "VQFN", "WQFN", "VFQFN"};
  return {family};
}

// True or false when the candidate's package can be judged against the footprint, nullopt when it cannot.
optional<bool> icPackageFits(const BomPartSpec& spec, const VendorCandidate& candidate) {
  if (spec.icFamily.empty()) return nullopt;
  const auto text = toUpper(vendorParameter(candidate, "Package / Case") + " | " +
                            vendorParameter(candidate, "Supplier Device Package"));
  if (trim(text) == "|") return nullopt;
  if (spec.icFamily == "SOT-23") {
    // Plain SOT-23 is the three-pin body: SOT-23-5 and -6 are other footprints.
    static const regex threePin(R"(SOT-23(-3)?(?![-0-9])|TO-236|SC-59)");
    static const regex morePins(R"(SOT-23-[4-8])");
    return regex_search(text, threePin) && !regex_search(text, morePins);
  }
  for (const auto& alias : fixedPackageAliases()) {
    if (spec.icFamily != alias.family) continue;
    for (const auto* word : alias.words) {
      if (text.find(toUpper(word)) != string::npos) return true;
    }
    return false;
  }
  if (spec.icPins <= 0) return nullopt;
  for (const auto& word : pinFamilyWords(spec.icFamily)) {
    const auto upper = toUpper(word);
    const regex numberFirst("(^|[^A-Z0-9])" + to_string(spec.icPins) + "-" + upper + "([^A-Z0-9]|$)");
    const regex familyFirst("(^|[^A-Z0-9])" + upper + "-" + to_string(spec.icPins) + "([^0-9]|$)");
    if (regex_search(text, numberFirst) || regex_search(text, familyFirst)) return true;
  }
  return false;
}

string icPackageWords(const BomPartSpec& spec) {
  return spec.icPins > 0 ? to_string(spec.icPins) + "-" + spec.icFamily : spec.icFamily;
}

string metricOf(const string& chip) {
  static const vector<pair<string, string>> codes = {{"01005", "0402"}, {"0201", "0603"}, {"0402", "1005"},
                                                     {"0603", "1608"},  {"0805", "2012"}, {"1206", "3216"},
                                                     {"1210", "3225"},  {"1812", "4532"}, {"2010", "5025"},
                                                     {"2512", "6332"}};
  for (const auto& [imperial, metric] : codes) {
    if (imperial == chip) return metric;
  }
  return {};
}

// ---------- designation reading ----------

const vector<string>& colourWords() {
  static const vector<string> words = {"warm white", "red", "green", "blue", "yellow", "white", "amber",
                                       "orange",     "rgb", "pink",  "purple", "uv", "infrared"};
  return words;
}

optional<double> parseAmps(const string& text) {
  static const regex amps(R"(^\s*(\d+(?:\.\d+)?)\s*(m?)A\b)");
  smatch m;
  if (!regex_search(text, m, amps)) return nullopt;
  return decimalOf(m[1]) * (m[2].length() > 0 ? 1e-3 : 1.0);
}

optional<double> parseValueOfKind(const string& text, ValueKind wanted) {
  ValueKind kind = ValueKind::None;
  const auto value = parseElectricalValue(text, kind);
  if (!value.has_value() || kind != wanted) return nullopt;
  return value;
}

bool parsesAsAnyValue(const string& token) {
  ValueKind kind = ValueKind::None;
  return parseElectricalValue(token, kind).has_value() || parseAmps(token).has_value();
}

bool looksLikeMpn(const string& token) {
  const auto t = trim(token);
  if (t.find(' ') != string::npos) return false;
  static const regex molex(R"(^\d{5,}-\d{3,}$)");
  if (regex_match(t, molex)) return true;
  const auto n = normPart(t);
  if (n.size() < 4) return false;
  if (none_of(n.begin(), n.end(), [](unsigned char ch) { return isalpha(ch) != 0; }) ||
      none_of(n.begin(), n.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
    return false;
  }
  static const regex notPart(
      R"(^(\d+(\.\d+)?x\d+(\.\d+)?(x\d+(\.\d+)?)?mm|P\d+(\.\d+)?mm|[12]x\d+|\d+x\d+|D\d+(\.\d+)?mm|L\d+(\.\d+)?mm|H\d+(\.\d+)?mm|W\d+(\.\d+)?mm|Pitch.*|Drill.*|TabPin\d|Pad.*|EP.*|Mask.*|ThermalVias|\d+Metric|(H?T?SSOP|SOIC|SOP|SOT|TSOT|SOD|TQFP|LQFP|V?QFN|DFN|VSON|WSON|VSSOP|MSOP|LGA|BGA|WLCSP|SC|TO|DO|SMD|THT|DIP|SIP)(-.*)?|(SOP|SOIC|QFP|QFN|SON|SOT|BGA)\d+P\d+X\d+.*)$)",
      regex::icase);
  if (regex_match(t, notPart)) return false;
  static const regex genericWord(R"(^(Conn|PinHeader|PinSocket|Screw|Terminal|USB|LED|MPB|RST|PWR|SW|TP|TestPoint|MountingHole|Fiducial|Crystal|Fuse)(_|-|$))",
                                 regex::icase);
  if (regex_search(t, genericWord)) return false;
  return !parsesAsAnyValue(t);
}

vector<string> footprintTokens(const string& footprint) {
  // KiCad library prefixes in front of a vendor part number.
  static const regex prefix(
      R"(^(XDCR|JST_(PH|XH|SH|GH|ZH|EH|PA|VH)|USB_C_Receptacle_[A-Za-z]+|USB_Micro-B|TerminalBlock_[A-Za-z]+|Crystal|Fuse|Inductor|L|D|C|R)_)");
  string f = regex_replace(footprint, prefix, "");
  static const regex pinTail(R"(_(1x|2x)\d+.*$)");
  f = regex_replace(f, pinTail, "");
  static const regex maker(R"((^|_)(Molex|Wurth|TE|Amphenol|Hirose|Samtec)-)");
  f = regex_replace(f, maker, "$1");
  vector<string> out;
  for (const auto& token : splitOnUnderscore(f)) {
    if (looksLikeMpn(token)) out.push_back(token);
  }
  return out;
}

void addUnique(vector<string>& tokens, const string& token) {
  const auto n = normPart(token);
  if (n.empty()) return;
  for (const auto& existing : tokens) {
    if (normPart(existing) == n) return;
  }
  tokens.push_back(trim(token));
}

// ---------- candidate facts ----------

bool usable(const VendorCandidate& candidate) {
  const auto status = toLower(candidate.status);
  const bool active = status.empty() || status == "active";
  return active && !candidate.discontinued && vendorCandidateCost(candidate, 1).has_value();
}

// The cheapest cost of an order, in micros, or a large number when the candidate has no price.
double costKey(const VendorCandidate& candidate, uint64_t quantity) {
  const auto cost = vendorCandidateCost(candidate, quantity);
  return cost.has_value() ? static_cast<double>(cost->totalMicros) : 1e18;
}

using RankKey = vector<double>;

// Earlier keys win. `extra` sits after stock, so a part out of stock never beats one in stock.
RankKey rankKey(const VendorCandidate& candidate, uint64_t quantity, const RankKey& extra) {
  const auto stock = vendorCandidateStock(candidate);
  RankKey key = {usable(candidate) ? 0.0 : 1.0, stock >= quantity ? 0.0 : 1.0};
  key.insert(key.end(), extra.begin(), extra.end());
  key.push_back(stock >= max<uint64_t>(quantity * 10, 100) ? 0.0 : 1.0);
  key.push_back(costKey(candidate, quantity));
  key.push_back(-static_cast<double>(stock));
  return key;
}

// A part the matcher or the picker may offer for the quantity: not discontinued, and an order for it can be
// bought now. A listed part that is out of stock, or whose order is out of proportion, is never offered.
bool buyableNow(const VendorCandidate& candidate, uint64_t quantity) {
  return !candidate.discontinued && vendorCandidateCost(candidate, quantity).has_value();
}

optional<double> toleranceOf(const VendorCandidate& candidate) {
  static const regex percent(R"((\d+(?:\.\d+)?)%)");
  const auto text = vendorParameter(candidate, "Tolerance");
  smatch m;
  if (!regex_search(text, m, percent)) return nullopt;
  return decimalOf(m[1]);
}

bool dielectricMatches(const string& text, const string& wanted) {
  const auto t = toUpper(text);
  const auto w = toUpper(wanted);
  if (w == "C0G" || w == "NP0" || w == "COG" || w == "NPO") return t.find("C0G") != string::npos || t.find("NP0") != string::npos;
  return t.find(w) != string::npos;
}

RankKey passivePreference(const BomPartSpec& spec, const VendorCandidate& candidate) {
  if (spec.kind == BomPartKind::Resistor) {
    const auto tolerance = toleranceOf(candidate).value_or(99);
    return {tolerance <= 1 ? 0.0 : tolerance <= 5 ? 1.0 : 2.0};
  }
  if (spec.kind == BomPartKind::Capacitor) {
    static const regex stable(R"(X7R|X5R|C0G|NP0|X7S|X6S|X8R)");
    const double dielectric = regex_search(vendorParameter(candidate, "Temperature Coefficient"), stable) ? 0 : 1;
    const auto rated = parseVendorQuantity(vendorParameter(candidate, "Voltage - Rated"), "V").value_or(0);
    return {dielectric + (rated >= spec.voltage.value_or(16) ? 0 : 1)};
  }
  if (spec.kind == BomPartKind::Electrolytic) {
    const auto rated = parseVendorQuantity(vendorParameter(candidate, "Voltage - Rated"), "V").value_or(0);
    return {rated >= spec.voltage.value_or(16) ? 0.0 : 1.0};
  }
  return {};
}

// ---------- verification ----------

bool chipFits(const BomPartSpec& spec, const VendorCandidate& candidate) {
  if (spec.axial) return vendorParameter(candidate, "Package / Case").find("Axial") != string::npos;
  if (spec.chipCode.empty()) return true;
  return packageMatches(spec.chipCode, vendorParameter(candidate, "Package / Case"));
}

// The line's value, package and ratings against the candidate's parameters, for value-and-package kinds.
bool valueFits(const BomPartSpec& spec, double target, const VendorCandidate& candidate) {
  switch (spec.kind) {
    case BomPartKind::Resistor:
      return closeTo(parseVendorQuantity(vendorParameter(candidate, "Resistance"), "Ohm"), target, 1e-6) &&
             chipFits(spec, candidate);
    case BomPartKind::Capacitor: {
      if (!closeTo(parseVendorQuantity(vendorParameter(candidate, "Capacitance"), "F"), target, 1e-6)) return false;
      if (!chipFits(spec, candidate)) return false;
      if (!spec.dielectric.empty() &&
          !dielectricMatches(vendorParameter(candidate, "Temperature Coefficient"), spec.dielectric)) {
        return false;
      }
      if (spec.voltage.has_value() &&
          parseVendorQuantity(vendorParameter(candidate, "Voltage - Rated"), "V").value_or(0) < *spec.voltage) {
        return false;
      }
      return true;
    }
    case BomPartKind::Inductor:
      return closeTo(parseVendorQuantity(vendorParameter(candidate, "Inductance"), "H"), target, 1e-6) &&
             chipFits(spec, candidate);
    case BomPartKind::Electrolytic: {
      if (!closeTo(parseVendorQuantity(vendorParameter(candidate, "Capacitance"), "F"), target, 1e-6)) return false;
      const auto size = vendorParameter(candidate, "Size / Dimension");
      const auto package = vendorParameter(candidate, "Package / Case");
      if (!startsWith(package, "Radial") || package.find("SMD") != string::npos) return false;
      if (spec.diameterMm.has_value()) {
        const auto diameter = "(" + fixedDigits(*spec.diameterMm, 2) + "mm)";
        if (size.find("Dia") == string::npos || size.find(diameter) == string::npos) return false;
      }
      if (spec.voltage.has_value() &&
          parseVendorQuantity(vendorParameter(candidate, "Voltage - Rated"), "V").value_or(0) < *spec.voltage) {
        return false;
      }
      return true;
    }
    case BomPartKind::Crystal: {
      if (!closeTo(parseVendorQuantity(vendorParameter(candidate, "Frequency"), "Hz"), target, 1e-4)) return false;
      if (!containsInsensitive(candidate.category, "crystal") && !startsWith(toUpper(candidate.description), "CRYSTAL")) {
        return false;
      }
      const auto size = vendorParameter(candidate, "Size / Dimension");
      if (!spec.crystalSize.empty() && size.find(spec.crystalSize) == string::npos) return false;
      if (spec.diameterMm.has_value()) {
        const auto diameter = "(" + fixedDigits(*spec.diameterMm, 2) + "mm x ";
        if (size.find(diameter) == string::npos) return false;
      }
      if (spec.hc49) {
        auto package = toUpper(vendorParameter(candidate, "Package / Case") + vendorParameter(candidate, "Supplier Device Package"));
        package.erase(remove(package.begin(), package.end(), '-'), package.end());
        if (package.find("HC49") == string::npos) return false;
      }
      return true;
    }
    case BomPartKind::Fuse:
      return closeTo(parseVendorQuantity(vendorParameter(candidate, "Current Rating (Amps)"), "A"), target, 1e-3) &&
             chipFits(spec, candidate);
    case BomPartKind::Led:
      return containsInsensitive(vendorParameter(candidate, "Color"), spec.colour) && chipFits(spec, candidate) &&
             (containsInsensitive(candidate.category, "LED") || containsInsensitive(candidate.description, "LED"));
    case BomPartKind::PartNumber:
    case BomPartKind::Generic:
      break;
  }
  return false;
}

// How a candidate's part number relates to the one searched for. Lower is closer.
enum class PartRelation {
  Exact = 0,
  OrderingVariant = 1,  // "AP63203WU-7", "LP5907MFX-3.3/NOPB": suffix after a separator or a letter
  ShorterCode = 2,      // the BOM carries a few more letters than the vendor's number
  GluedSuffix = 3,      // a letter glued to a final digit: "WROOM-1" -> "WROOM-1U" may be another part
  Series = 4,           // the token is a series code: any variant fits the footprint
};

optional<PartRelation> partRelation(const string& token, const string& mpn) {
  const auto a = normPart(token);
  const auto b = normPart(mpn);
  if (a.empty() || b.empty()) return nullopt;
  if (a == b) return PartRelation::Exact;
  const bool alphaOnly = all_of(a.begin(), a.end(), [](unsigned char ch) { return isalpha(ch) != 0; });
  if (alphaOnly) return startsWith(b, a) ? optional<PartRelation>(PartRelation::Series) : nullopt;
  if (startsWith(b, a) && b.size() - a.size() <= 8) {
    const auto raw = toUpper(trim(mpn));
    const auto wanted = toUpper(trim(token));
    const auto tail = startsWith(raw, wanted) ? raw.substr(wanted.size()) : string();
    const bool glued = isdigit(static_cast<unsigned char>(wanted.back())) != 0 && !tail.empty() &&
                       isalpha(static_cast<unsigned char>(tail.front())) != 0;
    return glued ? PartRelation::GluedSuffix : PartRelation::OrderingVariant;
  }
  if (startsWith(a, b) && a.size() - b.size() <= 3 && b.size() >= 5) return PartRelation::ShorterCode;
  // Microchip puts its tape-and-reel T inside the number: MCP1700-3302E -> MCP1700T-3302E/TT.
  for (size_t at = 4; at < b.size(); ++at) {
    if (b[at] != 'T') continue;
    const auto without = b.substr(0, at) + b.substr(at + 1);
    if (startsWith(without, a) && b.size() - a.size() <= 9) return PartRelation::OrderingVariant;
  }
  return nullopt;
}

size_t tailLength(const string& token, const string& mpn) {
  const auto a = normPart(token).size();
  const auto b = normPart(mpn).size();
  return b > a ? b - a : 0;
}

// The option a part number adds after the searched one, without packaging and plating codes:
// "ESP32-S3-WROOM-1-N16R8" -> "N16R8", "B3B-PH-K-S" -> "", "SI2301CDS-T1-GE3" -> "".
string optionCode(const string& token, const string& mpn) {
  const auto raw = toUpper(trim(mpn));
  const auto wanted = toUpper(trim(token));
  if (!startsWith(raw, wanted)) return {};
  // Reel, tape, plating and lead-free codes, and single letters such as JST's -S: the same part.
  static const regex packaging(R"(^([A-Z]|T\d*|TR|RL|LF|HF|SN|LFSN|PBF|TRPBF|NOPB|E3|GE3|BE3|HE3|G\d|\d{1,3}|CT|TA|TB)$)");
  vector<string> kept;
  string segment;
  const auto flush = [&]() {
    if (!segment.empty() && !regex_match(segment, packaging)) kept.push_back(segment);
    segment.clear();
  };
  for (const char ch : raw.substr(wanted.size())) {
    if (ch == '-' || ch == '/' || ch == ',' || ch == ' ' || ch == '(' || ch == ')') {
      flush();
    } else {
      segment.push_back(ch);
    }
  }
  flush();
  string code;
  for (const auto& part : kept) code += (code.empty() ? "" : "-") + part;
  return code;
}

// ---------- queries ----------

string resistorEia(double ohm) {
  if (ohm == 0) return "0R";
  if (ohm < 1) {
    return "R" + fixedDigits(ohm, 3).substr(2);  // 0.28 -> R280, as current-sense parts are sold
  }
  if (ohm >= 1e6) return formatNumber(ohm / 1e6) + "M";
  if (ohm >= 1e3) return formatNumber(ohm / 1e3) + "K";
  return formatNumber(ohm) + "R";
}

string resistorWords(double ohm) {
  if (ohm >= 1e6) return formatNumber(ohm / 1e6) + "M";
  if (ohm >= 1e3) return formatNumber(ohm / 1e3) + "K";
  return formatNumber(ohm);
}

string valueLabel(BomPartKind kind, double value) {
  switch (kind) {
    case BomPartKind::Resistor:
      return value >= 1e6 ? formatNumber(value / 1e6) + "M" : value >= 1e3 ? formatNumber(value / 1e3) + "k"
                                                                            : formatNumber(value) + "R";
    case BomPartKind::Capacitor:
    case BomPartKind::Electrolytic:
      return value >= 1e-6 ? formatNumber(value * 1e6) + "uF" : value >= 1e-9 ? formatNumber(value * 1e9) + "nF"
                                                                              : formatNumber(value * 1e12) + "pF";
    case BomPartKind::Inductor:
      return value >= 1e-6 ? formatNumber(value * 1e6) + "uH" : formatNumber(value * 1e9) + "nH";
    default:
      return formatNumber(value);
  }
}

string frequencyWords(double hertz) {
  return hertz >= 1e6 ? formatNumber(hertz / 1e6) + "MHz" : formatNumber(hertz / 1e3) + "kHz";
}

// DigiKey answers best in its own notation: compact EIA for resistors, its parameter text for
// ceramic capacitors, its description style for electrolytics.
vector<string> valueQueries(const BomPartSpec& spec, double value) {
  const auto chip = spec.chipCode;
  switch (spec.kind) {
    case BomPartKind::Resistor:
      return {trim(resistorEia(value) + " " + chip), trim("RES " + resistorWords(value) + " OHM " + chip)};
    case BomPartKind::Capacitor: {
      // DigiKey writes 10 nF as 10000 pF; "0.01 µF" also pulls 0.012 and 0.015 µF.
      const auto text = value >= 1.1e-8 ? formatNumber(value * 1e6) + " " + kMicro + "F" : formatNumber(value * 1e12) + " pF";
      const auto metric = metricOf(chip);
      return {trim(text + " " + chip + (metric.empty() ? "" : " (" + metric + " Metric)")),
              trim("CAP CER " + formatNumber(value * 1e6) + "UF " + chip)};
    }
    case BomPartKind::Inductor: {
      const auto micro = value >= 1e-6;
      const auto amount = formatNumber(micro ? value * 1e6 : value * 1e9);
      return {trim(amount + (micro ? string(" ") + kMicro + "H " : " nH ") + chip),
              trim("IND " + amount + (micro ? "UH " : "NH ") + chip)};
    }
    case BomPartKind::Electrolytic: {
      const auto uf = formatNumber(value * 1e6);
      vector<string> queries;
      if (spec.diameterMm.has_value()) {
        // The usual can height for the diameter; the search needs both to find through-hole cans.
        const double d = *spec.diameterMm;
        const double height = d <= 4 ? 7 : d <= 6.3 ? 11 : d <= 8 ? 11.5 : d <= 10 ? 12.5 : 0;
        if (height > 0) queries.push_back("CAP ALUM " + uf + "UF 20% RADIAL " + formatNumber(d) + "X" + formatNumber(height));
      }
      queries.push_back("CAP ALUM " + uf + "UF 20% RADIAL");
      return queries;
    }
    case BomPartKind::Crystal: {
      const auto words = frequencyWords(value);
      if (!spec.crystalCode.empty()) return {"crystal " + words + " " + spec.crystalCode};
      if (spec.diameterMm.has_value()) {
        return {"CRYSTAL " + fixedDigits(value / 1e3, 4) + "KHZ TH"};
      }
      return {"crystal " + words + (spec.hc49 ? " HC-49" : "")};
    }
    case BomPartKind::Fuse:
      return {trim("fuse " + (value < 1 ? formatNumber(value * 1000) + "mA" : formatNumber(value) + "A") + " " + chip)};
    case BomPartKind::Led:
      return {trim("LED " + spec.colour + " " + chip)};
    case BomPartKind::PartNumber:
    case BomPartKind::Generic:
      break;
  }
  return {};
}

// The family number without package and option letters: "TPS26601PWP" -> "TPS26601", "MCP1700-3302E" -> "MCP1700".
string basePart(const string& token) {
  static const regex head(R"(^([A-Za-z]*\d+[A-Za-z]?\d*))");
  smatch m;
  string base = regex_search(token, m, head) ? string(m[1]) : token;
  if (base.size() > 6) {
    static const regex trailingLetter(R"((\d)[A-Za-z]$)");
    base = regex_replace(base, trailingLetter, "$1");
  }
  return normPart(base).size() >= 5 && normPart(base) != normPart(token) ? base : string();
}

string vendorName() { return "DigiKey"; }

struct Searcher {
  const BomVendorSearch& search;
  int budget;
  BomVendorAnswer& answer;
  bool failed = false;

  // Nullopt when the search failed or the budget is spent; the answer is then Failed or NoMatch.
  optional<vector<VendorCandidate>> run(const string& keywords) {
    if (failed || answer.requests >= budget) return nullopt;
    ++answer.requests;
    auto result = search(keywords);
    if (!result.has_value()) failed = true;
    return result;
  }
};

// Value-and-package kinds: exact value first, then the nearest standard value, flagged.
bool matchByValue(const BomPartSpec& spec, uint64_t quantity, Searcher& searcher, BomVendorAnswer& answer) {
  if (!spec.value.has_value()) return false;
  vector<pair<double, string>> targets = {{*spec.value, ""}};
  const bool seriesKind = spec.kind == BomPartKind::Resistor || spec.kind == BomPartKind::Capacitor ||
                          spec.kind == BomPartKind::Inductor || spec.kind == BomPartKind::Electrolytic;
  if (seriesKind && *spec.value > 0) {
    if (const auto nearest = nearestStandardValue(*spec.value, spec.kind)) {
      const auto change = (*nearest / *spec.value - 1) * 100;
      const string sign = change >= 0 ? "+" : "";
      targets.push_back({*nearest, "nearest standard value " + valueLabel(spec.kind, *nearest) + " (" + sign +
                                       fixedDigits(change, 1) + "%)"});
    }
  }
  for (const auto& [target, note] : targets) {
    for (const auto& keywords : valueQueries(spec, target)) {
      const auto products = searcher.run(keywords);
      if (!products.has_value()) return false;
      vector<const VendorCandidate*> fits;
      for (const auto& product : *products) {
        if (valueFits(spec, target, product) && buyableNow(product, quantity)) fits.push_back(&product);
      }
      if (fits.empty()) continue;
      const auto best = *min_element(fits.begin(), fits.end(), [&](const auto* lhs, const auto* rhs) {
        return rankKey(*lhs, quantity, passivePreference(spec, *lhs)) < rankKey(*rhs, quantity, passivePreference(spec, *rhs));
      });
      answer.outcome = BomLookupOutcome::Found;
      answer.part = *best;
      answer.verified = true;
      answer.note = note;
      answer.check = !note.empty();
      if (spec.kind == BomPartKind::Electrolytic && !spec.voltage.has_value()) {
        answer.check = true;
        const auto rated = vendorParameter(*best, "Voltage - Rated");
        answer.note = (answer.note.empty() ? "" : answer.note + "; ") + "no voltage in BOM, chose " + (rated.empty() ? "?" : rated);
      }
      return true;
    }
  }
  return false;
}

// Part-number lines: search each token alone, accept exact and ordering variants in the footprint's
// package, and fall back to the token with its package, then the family number.
bool matchByPartNumber(const BomLine& line, const BomPartSpec& spec, uint64_t quantity, Searcher& searcher,
                       BomVendorAnswer& answer) {
  vector<string> tokens = spec.partNumbers;
  if (!spec.seriesCode.empty()) tokens.push_back(spec.seriesCode);
  const bool vendorNumber = toLower(line.supplier) == "digikey" && !trim(line.supplierPartNumber).empty();
  string otherPackageNote;
  const auto designationToken = !tokens.empty() && normPart(tokens.front()) == normPart(line.designation);
  for (size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    struct Query {
      string keywords;
      string relateTo;  // the number candidates are compared against
      bool onlyIfOtherPackage = false;
    };
    vector<Query> queries = {{token, token}};
    if (!spec.icFamily.empty()) queries.push_back({token + " " + icPackageWords(spec), token, true});
    if (const auto base = basePart(token); !base.empty()) queries.push_back({base, base, false});
    bool foundPart = false;
    for (const auto& query : queries) {
      if (query.onlyIfOtherPackage && !foundPart) continue;
      const auto products = searcher.run(query.keywords);
      if (!products.has_value()) return false;
      struct Fit {
        const VendorCandidate* candidate;
        PartRelation relation;
      };
      vector<Fit> related;
      for (const auto& product : *products) {
        auto relation = partRelation(query.relateTo, product.manufacturerPartNumber);
        if (vendorNumber && !relation.has_value()) {
          for (const auto& offer : product.offers) {
            if (normPart(offer.sku) == normPart(line.supplierPartNumber)) relation = PartRelation::Exact;
          }
        }
        // The family search only tells whether the part exists in another package.
        if (relation.has_value() && query.relateTo != token) {
          const auto exact = partRelation(token, product.manufacturerPartNumber);
          if (exact.has_value()) relation = exact;
        }
        if (relation.has_value()) related.push_back({&product, *relation});
      }
      if (!related.empty()) foundPart = true;
      vector<Fit> fits;
      for (const auto& fit : related) {
        if (icPackageFits(spec, *fit.candidate) != optional<bool>(false) && buyableNow(*fit.candidate, quantity)) {
          fits.push_back(fit);
        }
      }
      if (fits.empty()) {
        if (!related.empty() && !spec.icFamily.empty()) {
          otherPackageNote = "not sold as " + icPackageWords(spec) + " at " + vendorName() + " (" +
                             related.front().candidate->manufacturerPartNumber + " is another package)";
        }
        continue;
      }
      // A family hit must still carry the BOM's number, not only its family.
      if (query.relateTo != token) {
        fits.erase(remove_if(fits.begin(), fits.end(),
                             [&](const Fit& fit) { return !partRelation(token, fit.candidate->manufacturerPartNumber); }),
                   fits.end());
        if (fits.empty()) continue;
      }
      const auto keyOf = [&](const Fit& fit) {
        const auto packageKnown = icPackageFits(spec, *fit.candidate).has_value() || spec.icFamily.empty();
        return rankKey(*fit.candidate, quantity,
                       {packageKnown ? 0.0 : 1.0, static_cast<double>(fit.relation),
                        static_cast<double>(tailLength(token, fit.candidate->manufacturerPartNumber))});
      };
      const auto best = *min_element(fits.begin(), fits.end(), [&](const Fit& lhs, const Fit& rhs) { return keyOf(lhs) < keyOf(rhs); });
      answer.outcome = BomLookupOutcome::Found;
      answer.part = *best.candidate;
      answer.verified = true;
      vector<string> notes;
      if (best.relation == PartRelation::Series) {
        answer.check = true;
        notes.push_back("series " + token + ": variant chosen");
      } else if (best.relation == PartRelation::GluedSuffix &&
                 !optionCode(token, best.candidate->manufacturerPartNumber).empty() &&
                 (tailLength(token, best.candidate->manufacturerPartNumber) > 3 || !icPackageFits(spec, *best.candidate).value_or(false))) {
        answer.check = true;
        notes.push_back(best.candidate->manufacturerPartNumber + " carries more than " + token);
      }
      if (!spec.icFamily.empty() && !icPackageFits(spec, *best.candidate).has_value()) {
        answer.check = true;
        notes.push_back("package not listed");
      }
      // The maker sells several options of the number (flash size, grade): the user picks. Other
      // makers' suffixes say nothing about this maker's options.
      set<string> options;
      for (const auto& fit : fits) {
        if (fit.relation != PartRelation::OrderingVariant && fit.relation != PartRelation::Exact) continue;
        if (toLower(fit.candidate->manufacturer) != toLower(best.candidate->manufacturer)) continue;
        options.insert(optionCode(token, fit.candidate->manufacturerPartNumber));
      }
      if (options.size() > 1) {
        answer.check = true;
        string list;
        size_t shown = 0;
        for (const auto& option : options) {
          if (shown++ == 4) {
            list += ", ...";
            break;
          }
          list += (list.empty() ? "" : ", ") + (option.empty() ? string("(plain)") : option);
        }
        notes.push_back("variants: " + list);
      }
      if (index > 0 && designationToken) {
        answer.check = true;
        notes.push_back(tokens.front() + " not found; matched the footprint's " + token);
      }
      for (const auto& note : notes) answer.note += (answer.note.empty() ? "" : "; ") + note;
      return true;
    }
  }
  if (!otherPackageNote.empty()) answer.note = otherPackageNote;
  return false;
}

}  // namespace

// ---------- public ----------

string vendorParameter(const VendorCandidate& candidate, const string& label) {
  for (const auto& parameter : candidate.parameters) {
    if (parameter.name == label) return parameter.value;
  }
  return {};
}

optional<double> parseVendorQuantity(const string& text, const string& unit) {
  size_t at = 0;
  while (at < text.size() && isspace(static_cast<unsigned char>(text[at])) != 0) ++at;
  const size_t start = at;
  while (at < text.size() && (isdigit(static_cast<unsigned char>(text[at])) != 0 || text[at] == '.' || text[at] == ',')) ++at;
  if (at == start) return nullopt;
  auto number = text.substr(start, at - start);
  number.erase(remove(number.begin(), number.end(), ','), number.end());
  if (number.empty() || number == ".") return nullopt;
  while (at < text.size() && text[at] == ' ') ++at;
  double multiplier = 1;
  static const vector<pair<string, double>> prefixes = {{"\xC2\xB5", 1e-6}, {"\xCE\xBC", 1e-6}, {"p", 1e-12},
                                                        {"n", 1e-9},        {"u", 1e-6},        {"m", 1e-3},
                                                        {"k", 1e3},         {"K", 1e3},         {"M", 1e6},
                                                        {"G", 1e9}};
  const auto rest = text.substr(at);
  if (!startsWith(rest, unit)) {
    bool matched = false;
    for (const auto& [prefix, factor] : prefixes) {
      if (startsWith(rest, prefix) && startsWith(rest.substr(prefix.size()), unit)) {
        multiplier = factor;
        matched = true;
        break;
      }
    }
    if (!matched) return nullopt;
  }
  double value = 0;
  if (!parseClassicDecimal(number, value)) return nullopt;
  return value * multiplier;
}

uint64_t vendorCandidateStock(const VendorCandidate& candidate) {
  uint64_t stock = candidate.stock.value_or(0);
  for (const auto& offer : candidate.offers) stock = max(stock, offer.stock.value_or(0));
  return stock;
}

optional<OfferCost> vendorCandidateCost(const VendorCandidate& candidate, uint64_t quantity) {
  optional<OfferCost> best;
  for (const auto& offer : candidate.offers) {
    const auto cost = orderableOffer(offer, max<uint64_t>(quantity, 1));
    if (cost.has_value() && (!best.has_value() || cost->totalMicros < best->totalMicros)) best = cost;
  }
  return best;
}

bool candidateEndOfLife(const VendorCandidate& candidate) { return lifecycleIsObsolete(candidate.status); }

optional<double> nearestStandardValue(double value, BomPartKind kind) {
  if (value <= 0) return nullopt;
  const auto series = seriesFor(kind);
  const auto decade = floor(log10(value));
  const auto mantissa = value / pow(10.0, decade);
  for (const auto step : series) {
    if (fabs(mantissa - step) / step < 0.0006 || fabs(mantissa - step * 10) / (step * 10) < 0.0006) return nullopt;
  }
  if (fabs(mantissa - 10.0) / 10.0 < 0.0006) return nullopt;
  double best = 0;
  for (int shift = -1; shift <= 1; ++shift) {
    for (const auto step : series) {
      const auto candidate = step * pow(10.0, decade + shift);
      if (best == 0 || fabs(log(candidate / value)) < fabs(log(best / value))) best = candidate;
    }
  }
  // Round away binary noise so 19.1k is 19100, not 19099.999.
  const auto scale = pow(10.0, floor(log10(best)) - 3);
  return round(best / scale) * scale;
}

BomPartSpec bomPartSpec(const BomLine& line) {
  BomPartSpec spec;
  const auto fp = footprintName(line.footprint);
  const auto designation = trim(line.designation);
  const auto package = packageFromFootprint(fp);
  for (const auto* code : kChipCodes) {
    if (package == code) spec.chipCode = code;
  }
  const auto ic = icPackageOf(fp);
  spec.icFamily = ic.family;
  spec.icPins = ic.pins;

  // "100nF/50V X7R", "10k 1%": the value comes first, ratings after it.
  vector<string> words;
  {
    string word;
    for (const char ch : designation) {
      if (ch == ' ' || ch == '/' || ch == ',' || ch == ';') {
        if (!word.empty()) words.push_back(word);
        word.clear();
      } else {
        word.push_back(ch);
      }
    }
    if (!word.empty()) words.push_back(word);
  }
  const auto first = words.empty() ? designation : words.front();
  static const regex volts(R"(^(\d+(?:\.\d+)?)V$)", regex::icase);
  static const regex dielectric(R"(^(X[5-8][RSTP]|C0G|NP0|COG|NPO|Y5V|Z5U)$)", regex::icase);
  for (size_t index = 1; index < words.size(); ++index) {
    smatch m;
    if (regex_match(words[index], m, volts)) spec.voltage = decimalOf(m[1]);
    if (regex_match(words[index], dielectric)) spec.dielectric = toUpper(words[index]);
  }

  smatch m;
  static const regex diameter(R"(_D(\d+(?:\.\d+)?)mm)");
  if (regex_search(fp, m, diameter)) spec.diameterMm = decimalOf(m[1]);
  static const regex positions(R"((?:^|_)[12]x(\d+)(?:_|$))");
  if (regex_search(fp, m, positions)) spec.positions = stoi(m[1]);
  static const regex pitch(R"(_P(\d+(?:\.\d+)?)mm)");
  if (regex_search(fp, m, pitch)) spec.pitchMm = decimalOf(m[1]);

  // Part numbers: an MPN column first, then the designation, then tokens of the footprint.
  if (!trim(line.manufacturerPartNumber).empty()) addUnique(spec.partNumbers, line.manufacturerPartNumber);
  if (toLower(line.supplier) == "digikey" && !trim(line.supplierPartNumber).empty()) {
    addUnique(spec.partNumbers, line.supplierPartNumber);
  }
  if (looksLikeMpn(designation)) addUnique(spec.partNumbers, designation);
  for (const auto& token : footprintTokens(fp)) addUnique(spec.partNumbers, token);
  static const regex series(R"(_(Alps|ALPS|Omron|CK|Panasonic|Wurth|TE|Molex|E-Switch)_([A-Z]{4,})$)");
  if (regex_search(fp, m, series)) spec.seriesCode = m[2];

  const bool hasPart = !spec.partNumbers.empty() || !spec.seriesCode.empty();
  const bool axial = fp.find("Axial") != string::npos;
  spec.axial = axial && spec.chipCode.empty();
  if (!trim(line.manufacturerPartNumber).empty()) {
    spec.kind = BomPartKind::PartNumber;
  } else if (startsWith(fp, "CP_")) {
    spec.kind = BomPartKind::Electrolytic;
    spec.value = parseValueOfKind(first, ValueKind::Capacitance);
  } else if (startsWith(fp, "R_") && (!spec.chipCode.empty() || axial) &&
             (spec.value = parseValueOfKind(first, ValueKind::Resistance))) {
    spec.kind = BomPartKind::Resistor;
  } else if (startsWith(fp, "C_") && !spec.chipCode.empty() && (spec.value = parseValueOfKind(first, ValueKind::Capacitance))) {
    spec.kind = BomPartKind::Capacitor;
  } else if (startsWith(fp, "L_") && !spec.chipCode.empty() && (spec.value = parseValueOfKind(first, ValueKind::Inductance))) {
    spec.kind = BomPartKind::Inductor;
  } else if (startsWith(fp, "Crystal") && (spec.value = parseValueOfKind(first, ValueKind::Frequency))) {
    spec.kind = BomPartKind::Crystal;
    static const regex smd(R"((\d{4})-\dPin_(\d+(?:\.\d+)?)x(\d+(?:\.\d+)?)mm)");
    if (regex_search(fp, m, smd)) {
      spec.crystalCode = m[1];
      spec.crystalSize = fixedDigits(decimalOf(m[2]), 2) + "mm x " + fixedDigits(decimalOf(m[3]), 2) + "mm";
    }
    spec.hc49 = fp.find("HC49") != string::npos;
    spec.partNumbers.clear();
  } else if (startsWith(fp, "Fuse") && (spec.value = parseAmps(first))) {
    spec.kind = BomPartKind::Fuse;
    spec.partNumbers.clear();
  } else if (startsWith(fp, "LED_") && !looksLikeMpn(designation)) {
    spec.kind = BomPartKind::Led;
    const auto lowered = toLower(designation);
    for (const auto& colour : colourWords()) {
      if (lowered.find(colour) != string::npos) {
        spec.colour = colour;
        break;
      }
    }
    spec.partNumbers.clear();
  } else if (hasPart) {
    spec.kind = BomPartKind::PartNumber;
  } else {
    spec.kind = BomPartKind::Generic;
  }
  if (spec.kind == BomPartKind::Resistor || spec.kind == BomPartKind::Capacitor) spec.partNumbers.clear();
  return spec;
}

BomVendorAnswer matchBomLineAtVendor(const BomLine& line, uint64_t quantity, const BomVendorSearch& search,
                                     int maxRequests) {
  BomVendorAnswer answer;
  const auto spec = bomPartSpec(line);
  Searcher searcher{search, maxRequests, answer};
  quantity = max<uint64_t>(quantity, 1);
  bool found = false;
  switch (spec.kind) {
    case BomPartKind::Resistor:
    case BomPartKind::Capacitor:
    case BomPartKind::Inductor:
    case BomPartKind::Electrolytic:
    case BomPartKind::Crystal:
    case BomPartKind::Fuse:
      found = matchByValue(spec, quantity, searcher, answer);
      if (!found && !searcher.failed && !spec.partNumbers.empty()) {
        found = matchByPartNumber(line, spec, quantity, searcher, answer);
      }
      if (!found && !searcher.failed && answer.note.empty() && !spec.value.has_value()) {
        answer.needsChoice = true;
      }
      break;
    case BomPartKind::Led:
      if (spec.colour.empty()) {
        answer.needsChoice = true;
        answer.note = "no colour in BOM: pick a part";
      } else {
        // The colour and the chip size are the whole spec; the value only carries the search.
        auto coloured = spec;
        coloured.value = 1.0;
        found = matchByValue(coloured, quantity, searcher, answer);
      }
      break;
    case BomPartKind::PartNumber:
      found = matchByPartNumber(line, spec, quantity, searcher, answer);
      break;
    case BomPartKind::Generic:
      answer.needsChoice = true;
      answer.note = "many parts fit: pick one";
      break;
  }
  if (found) return answer;
  if (searcher.failed) {
    answer.outcome = BomLookupOutcome::Failed;
    answer.part.reset();
    return answer;
  }
  answer.outcome = BomLookupOutcome::NoMatch;
  return answer;
}

string bomPickerQuery(const BomLine& line) {
  const auto spec = bomPartSpec(line);
  const auto fp = footprintName(line.footprint);
  if (!spec.partNumbers.empty()) return spec.partNumbers.front();
  if (!spec.seriesCode.empty()) return spec.seriesCode;
  if (spec.value.has_value()) {
    const auto queries = valueQueries(spec, *spec.value);
    if (!queries.empty()) return queries.front();
  }
  if (spec.kind == BomPartKind::Led) return spec.colour.empty() ? trim("LED " + spec.chipCode) : trim("LED " + spec.colour + " " + spec.chipCode);
  const auto pitch = spec.pitchMm > 0 ? formatNumber(spec.pitchMm) + "MM" : string("2.54MM");
  if (startsWith(fp, "PinHeader") && spec.positions > 0) return "CONN HEADER VERT " + to_string(spec.positions) + "POS " + pitch;
  if (startsWith(fp, "PinSocket") && spec.positions > 0) return "CONN RCPT " + to_string(spec.positions) + "POS " + pitch;
  if (startsWith(fp, "TerminalBlock") && spec.positions > 0) return "TERM BLK " + to_string(spec.positions) + "POS " + pitch;
  if (startsWith(toLower(fp), "sw_push_6mm")) return "SWITCH TACTILE SPST-NO 6MM";
  return trim(trim(line.designation) + " " + (spec.chipCode.empty() ? string() : spec.chipCode));
}

bool bomPickerFits(const BomLine& line, const VendorCandidate& candidate) {
  const auto spec = bomPartSpec(line);
  if (spec.value.has_value() && valueFits(spec, *spec.value, candidate)) return true;
  for (const auto& token : spec.partNumbers) {
    if (partRelation(token, candidate.manufacturerPartNumber).has_value() &&
        icPackageFits(spec, candidate) != optional<bool>(false)) {
      return true;
    }
  }
  return spec.kind == BomPartKind::Led && !spec.chipCode.empty() && chipFits(spec, candidate);
}

namespace {

vector<string> wordsOf(const string& text) {
  // A non-breaking space, which copied text can carry, separates words like a space does.
  string spaced = text;
  for (auto at = spaced.find("\xC2\xA0"); at != string::npos; at = spaced.find("\xC2\xA0", at + 1)) {
    spaced.replace(at, 2, " ");
  }
  vector<string> words;
  size_t start = 0;
  while (start < spaced.size()) {
    const auto space = spaced.find(' ', start);
    const auto end = space == string::npos ? spaced.size() : space;
    if (end > start) words.push_back(spaced.substr(start, end - start));
    start = end + 1;
  }
  return words;
}

// A value typed in the picker, found among its words: "0,33 uF 0603 (1608 Metric)" reads as 0.33 uF, and the
// other words are kept. A value is one word ("0.33uF") or two ("0,33 uF"). Words in brackets never start one.
struct TypedValue {
  double value = 0;
  vector<string> otherWords;
};

optional<TypedValue> readTypedValue(const BomLine& line, const string& typed) {
  optional<ValueKind> wanted;
  switch (bomPartSpec(line).kind) {
    case BomPartKind::Resistor:
      wanted = ValueKind::Resistance;
      break;
    case BomPartKind::Capacitor:
    case BomPartKind::Electrolytic:
      wanted = ValueKind::Capacitance;
      break;
    case BomPartKind::Inductor:
      wanted = ValueKind::Inductance;
      break;
    case BomPartKind::Crystal:
      wanted = ValueKind::Frequency;
      break;
    case BomPartKind::Fuse:
    case BomPartKind::Led:
    case BomPartKind::PartNumber:
    case BomPartKind::Generic:
      break;
  }
  if (!wanted) return nullopt;
  const auto words = wordsOf(typed);
  for (size_t first = 0; first < words.size(); ++first) {
    string joined;
    for (size_t last = first; last < min(words.size(), first + 2); ++last) {
      if (words[last].find_first_of("()") != string::npos) break;
      joined += words[last];
      if (const auto value = parseValueOfKind(joined, *wanted)) {
        TypedValue found{*value, {}};
        for (size_t index = 0; index < words.size(); ++index) {
          if (index < first || index > last) found.otherWords.push_back(words[index]);
        }
        return found;
      }
    }
  }
  return nullopt;
}

}  // namespace

optional<double> bomPickerTypedValue(const BomLine& line, const string& typed) {
  const auto found = readTypedValue(line, typed);
  return found.has_value() ? optional<double>(found->value) : nullopt;
}

string bomPickerReading(const BomLine& line, const string& typed) {
  const auto value = bomPickerTypedValue(line, typed);
  if (!value.has_value()) return {};
  const auto kind = bomPartSpec(line).kind;
  switch (kind) {
    case BomPartKind::Resistor:
      if (*value >= 1e6) return formatNumber(*value / 1e6) + " Mohm";
      if (*value >= 1e3) return formatNumber(*value / 1e3) + " kohm";
      return formatNumber(*value) + " ohm";
    case BomPartKind::Capacitor:
    case BomPartKind::Electrolytic:
    case BomPartKind::Inductor:
      return valueLabel(kind, *value);
    case BomPartKind::Crystal:
      return frequencyWords(*value);
    case BomPartKind::Fuse:
    case BomPartKind::Led:
    case BomPartKind::PartNumber:
    case BomPartKind::Generic:
      break;
  }
  return {};
}

vector<string> bomPickerQueries(const BomLine& line, const string& typed) {
  const auto words = trim(typed);
  const auto spec = bomPartSpec(line);
  if (const auto found = readTypedValue(line, words); found.has_value()) {
    auto typedSpec = spec;
    typedSpec.value = found->value;
    const auto plain = valueQueries(typedSpec, found->value);
    if (!plain.empty()) {
      // Words the user added ("X7R", "16V") narrow each query. The plain value queries follow, so a narrow search
      // that finds nothing still has the usual fallback.
      vector<string> queries;
      const auto addQuery = [&queries](const string& query) {
        if (find(queries.begin(), queries.end(), query) == queries.end()) queries.push_back(query);
      };
      for (const auto& query : plain) {
        auto narrowed = query;
        for (const auto& word : found->otherWords) {
          if (!containsInsensitive(narrowed, word)) narrowed += " " + word;
        }
        addQuery(narrowed);
      }
      for (const auto& query : plain) addQuery(query);
      return queries;
    }
  }
  if (!words.empty()) return {words};
  if (spec.partNumbers.empty() && spec.seriesCode.empty() && spec.value.has_value()) {
    auto queries = valueQueries(spec, *spec.value);
    if (!queries.empty()) return queries;
  }
  return {bomPickerQuery(line)};
}

namespace {

// True when the product states a value of the line's kind and it is not the line's: a 100nF capacitor
// for a 10uF line. A product that states no value is not ruled out here.
bool namesOtherValue(const BomPartSpec& spec, const VendorCandidate& candidate) {
  if (!spec.value.has_value()) return false;
  optional<double> value;
  switch (spec.kind) {
    case BomPartKind::Resistor: value = parseVendorQuantity(vendorParameter(candidate, "Resistance"), "Ohm"); break;
    case BomPartKind::Capacitor:
    case BomPartKind::Electrolytic: value = parseVendorQuantity(vendorParameter(candidate, "Capacitance"), "F"); break;
    case BomPartKind::Inductor: value = parseVendorQuantity(vendorParameter(candidate, "Inductance"), "H"); break;
    case BomPartKind::Crystal:
    case BomPartKind::Fuse:
    case BomPartKind::Led:
    case BomPartKind::PartNumber:
    case BomPartKind::Generic: return false;
  }
  return value.has_value() && !closeTo(value, *spec.value, 1e-6);
}

}  // namespace

BomPickerRanking rankBomPickerCandidates(const BomLine& line, uint64_t quantity,
                                         const vector<VendorCandidate>& candidates, size_t limit) {
  const auto spec = bomPartSpec(line);
  quantity = max<uint64_t>(quantity, 1);
  BomPickerRanking out;
  // Parts of another value are only noise beside parts that fit. Without any that fit they stay, as the
  // search may have asked for another value on purpose.
  const bool someFit = any_of(candidates.begin(), candidates.end(), [&](const VendorCandidate& candidate) {
    return buyableNow(candidate, quantity) && bomPickerFits(line, candidate);
  });
  vector<pair<RankKey, BomPickerEntry>> ranked;
  for (const auto& candidate : candidates) {
    if (!buyableNow(candidate, quantity)) {
      ++out.leftOut;
      continue;
    }
    const bool fits = bomPickerFits(line, candidate);
    if (!fits && someFit && namesOtherValue(spec, candidate)) {
      ++out.otherValue;
      continue;
    }
    auto key = rankKey(candidate, quantity, passivePreference(spec, candidate));
    key.insert(key.begin(), fits ? 0.0 : 1.0);
    ranked.push_back({key, {candidate, fits}});
  }
  stable_sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  for (auto& entry : ranked) {
    if (out.entries.size() == limit) break;
    out.entries.push_back(move(entry.second));
  }
  return out;
}

bool bomPickerSearchable(const string& store) { return store == kVendorProviderDigiKey || store == kVendorProviderTme; }

string bomPickerContext(const string& store, uint64_t quantity) {
  return store + ", need " + voiceCount(static_cast<int>(quantity), "piece", "pieces") +
         ". Only parts in stock at a normal price are listed.";
}

string bomPickerSummary(const string& store, size_t listed, size_t leftOut, size_t otherValue) {
  if (listed == 0 && leftOut == 0) return store + " has nothing for these words. Press / to change them.";
  if (listed == 0) {
    return store + " found " + voiceCount(static_cast<int>(leftOut), "part", "parts") +
           ", but none is in stock at a normal price. Press / to try other words.";
  }
  string text = store + " has " + voiceCount(static_cast<int>(listed), "part", "parts") + " in stock at a normal price.";
  if (leftOut > 0) text += " " + to_string(leftOut) + " left out: out of stock or not at a normal price.";
  if (otherValue > 0) text += " " + voiceCount(static_cast<int>(otherValue), "part", "parts") + " of another value left out.";
  return text;
}

}  // namespace inventatory
