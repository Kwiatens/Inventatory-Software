// Inventatory - Item label detail selection helpers.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "core/parts/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>

namespace inventatory {

using namespace std;

namespace label_printer_detail {

namespace {

// The measured value printed as the main text of a passive part, without its
// tolerance. Empty when the part is not a passive or the value is missing.
string measuredMainValue(const InventoryItem& item) {
  const auto compactValue = [](string value) {
    value = trim(value);
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

bool endsWith(const string& value, const string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
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

bool isNonstandardPackage(const string& value) {
  const auto key = normalizeKey(value);
  return key.empty() || key.find("nonstandard") != string::npos || key == "custom" || key == "other";
}

// "12.00" -> "12", "0.50" -> "0.5".
string trimTrailingZeros(string number) {
  if (number.find('.') != string::npos) {
    while (!number.empty() && number.back() == '0') number.pop_back();
    if (!number.empty() && number.back() == '.') number.pop_back();
  }
  return number;
}

// "0.157" L x 0.157" W (4.00mm x 4.00mm)" -> "4x4mm".
string metricSizeLine(const string& value) {
  static const regex pattern(R"((\d+(?:\.\d+)?)\s*mm\s*[xX]\s*(\d+(?:\.\d+)?)\s*mm)");
  smatch match;
  if (!regex_search(value, match, pattern)) {
    return {};
  }
  return trimTrailingZeros(match[1].str()) + "x" + trimTrailingZeros(match[2].str()) + "mm";
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

bool hasUnit(const string& value, Unit unit) {
  if (unit == Unit::Any) return true;
  // Compiled once, in Unit order after Any: building a std::regex is far costlier than matching it.
  static const regex kUnitPatterns[] = {
      regex(u8R"(\d\s*(?:[pnumkKMG]|µ)?V(?:DC|AC|dc|ac)?(?![A-Za-z]))"),
      regex(u8R"(\d\s*(?:[pnumkKMG]|µ)?A(?![A-Za-z]))"),
      regex(u8R"(\d\s*(?:[pnumkKMG]|µ)?W(?![A-Za-z]))"),
      regex(u8R"(\d\s*(?:[pnumkKMG]|µ)?(?:Ω|Ohm))"),
      regex(u8R"(\d\s*[kKMG]?Hz)"),
      regex(u8R"(\d\s*(?:°\s*C|C(?![A-Za-z])))"),
      regex(u8R"(\d\s*mm(?![A-Za-z]))"),
  };
  return regex_search(value, kUnitPatterns[static_cast<size_t>(unit) - 1]);
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

class TileCollector {
 public:
  explicit TileCollector(const InventoryItem& item) : item_(item) {}

  // Each parameter feeds at most one tile. Names match exactly first, then, for
  // long names only, as a prefix ("Voltage - Rated" finds "Voltage - Rated (DC)").
  // Short names such as "Type" never match inside longer ones: they would
  // otherwise pick up "Mounting Type".
  void add(const string& caption, initializer_list<const char*> names, Unit unit = Unit::Any) {
    for (const bool prefixPass : {false, true}) {
      for (const auto* name : names) {
        const auto wanted = normalizeKey(name);
        if (prefixPass && wanted.size() < 10) continue;
        for (size_t index = 0; index < item_.parameters.size(); ++index) {
          const auto& parameter = item_.parameters[index];
          const auto key = normalizeKey(parameter.name);
          const bool matches = prefixPass ? key.compare(0, wanted.size(), wanted) == 0 : key == wanted;
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
  value.erase(remove(value.begin(), value.end(), ' '), value.end());
  return fieldOrBlank(value, 8);
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
      const auto compact = compactDescriptor(*package, 12);
      if (!compact.empty() && !isNonstandardPackage(compact)) {
        return fieldOrBlank(compact, 12);
      }
    }
  }
  if (const auto size = firstParameter(item, {"Size / Dimension", "Dimensions"})) {
    if (auto metric = metricSizeLine(*size); !metric.empty()) {
      return fieldOrBlank(metric, 12);
    }
    return fieldOrBlank(compactDescriptor(*size, 12), 12);
  }
  return {};
}

string manufacturerLine(const InventoryItem& item) {
  return fieldOrBlank(trim(item.manufacturer), 64);
}

vector<string> manufacturerCandidates(const string& name) {
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
  static const regex metricInBrackets(R"(\((\d+(?:\.\d+)?)\s*mm\))");
  smatch metric;
  if (text.find('"') != string::npos && regex_search(text, metric, metricInBrackets)) {
    text = trimTrailingZeros(metric[1].str()) + "mm";
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
  if (startsWithInsensitive(text, "ARM ") && text.size() > 4) {
    text = trim(text.substr(4));
  }
  return sanitiseZplFragment(text);
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

vector<LabelParameterTile> fallbackParameterTiles(const InventoryItem& item, size_t maxTiles) {
  vector<LabelParameterTile> tiles;
  const auto main = mainLabelValue(item);
  for (const auto& field : electricalFieldsForItem(item)) {
    const auto value = labelTileValue(field.value);
    if (isPlaceholderValue(value) || value == main || looksLikeMountingValue(value) || !fitsTile(value)) {
      continue;
    }
    if (containsInsensitive(field.label, "package")) {
      continue;
    }
    auto caption = toUpper(shortParameterLabel(field.label));
    while (!caption.empty() && (caption.back() == ':' || caption.back() == ' ')) caption.pop_back();
    if (caption.empty()) continue;
    tiles.push_back({caption, value});
    if (tiles.size() >= maxTiles) {
      break;
    }
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
    return tiles.take();
  }

  if (categoryContains(item, {"resistor"})) {
    tiles.add("POWER", {"Power (Watts)", "Power Dissipation", "Power Rating", "Power - Max", "Power", "Watts"}, Unit::Watt);
    tiles.add("TEMPCO", {"Temperature Coefficient", "Tempco"});
    tiles.add("TYPE", {"Composition"});
    tiles.add("MAX V", {"Voltage - Max", "Max Working Voltage", "Voltage Rating"}, Unit::Volt);
    return tiles.take();
  }

  if (categoryContains(item, {"inductor", "choke", "coil"})) {
    tiles.add("RATED", {"Current Rating (Amps)", "Current Rating", "Current"}, Unit::Amp);
    tiles.add("ISAT", {"Current - Saturation (Isat)", "Saturation Current", "Current - Saturation"}, Unit::Amp);
    tiles.add("DCR", {"DC Resistance (DCR)", "DC Resistance", "DCR"}, Unit::Ohm);
    tiles.addValue("SHIELD", normalizedShieldingLine(firstParameter(item, {"Shielding"})));
    tiles.add("SRF", {"Frequency - Self Resonant"}, Unit::Hertz);
    return tiles.take();
  }

  if (categoryContains(item, {"mcu", "microcontroller"})) {
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
    return tiles.take();
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
    tiles.add("GAIN", {"DC Current Gain (hFE) (Min) @ Ic, Vce", "DC Current Gain", "hFE"});
    tiles.add("POWER", {"Power - Max"}, Unit::Watt);
    return tiles.take();
  }

  if (isFuse) {
    tiles.add("RATED", {"Current Rating (Amps)", "Current Rating"}, Unit::Amp);
    tiles.add("VDC", {"Voltage Rating - DC", "Voltage - DC"}, Unit::Volt);
    tiles.add("VAC", {"Voltage Rating - AC", "Voltage - AC"}, Unit::Volt);
    tiles.add("SPEED", {"Response Time"});
    return tiles.take();
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
    return tiles.take();
  }

  if (isDiode) {
    tiles.add("VZ", {"Voltage - Zener (Nom) (Vz)", "Zener Voltage"}, Unit::Volt);
    tiles.add("VR", {"Voltage - DC Reverse (Vr) (Max)", "Reverse Voltage", "Peak Reverse Voltage", "Vr"}, Unit::Volt);
    tiles.add("IO", {"Current - Average Rectified (Io)", "Forward Current", "Current"}, Unit::Amp);
    tiles.add("VF", {"Voltage - Forward (Vf) (Max) @ If", "Forward Voltage", "Vf"}, Unit::Volt);
    tiles.add("IFSM", {"Current - Non Rep. Surge 50, 60Hz (Ifsm)", "Ifsm"}, Unit::Amp);
    tiles.add("TECH", {"Technology"});
    return tiles.take();
  }

  if (categoryContains(item, {"connector"}) || itemTextContains(item, {"connector"})) {
    tiles.add("PINS", {"Number of Positions", "Pins", "Pin Count"});
    tiles.add("TYPE", {"Connector Type"});
    tiles.add("ROWS", {"Number of Rows", "Rows"});
    tiles.add("PITCH", {"Pitch - Mating", "Pitch"});
    return tiles.take();
  }

  if (categoryContains(item, {"regulator", "voltage regulator", "power management"}) ||
      itemTextContains(item, {"regulator", "ldo", "buck", "boost"})) {
    tiles.add("VOUT", {"Voltage - Output (Min/Fixed)", "Voltage - Output", "Output Voltage", "Vout"}, Unit::Volt);
    tiles.add("VIN", {"Voltage - Input (Max)", "Voltage - Input", "Vin"}, Unit::Volt);
    tiles.add("IOUT", {"Current - Output", "Output Current", "Iout"}, Unit::Amp);
    tiles.add("TYPE", {"Output Type", "Type"});
    return tiles.take();
  }

  if (categoryContains(item, {"crystal", "oscillator", "resonator"}) ||
      itemTextContains(item, {"crystal", "oscillator", "resonator"})) {
    tiles.add("FREQ", {"Frequency"}, Unit::Hertz);
    tiles.add("LOAD C", {"Load Capacitance"});
    tiles.add("ESR", {"ESR (Equivalent Series Resistance)", "Equivalent Series Resistance", "ESR"}, Unit::Ohm);
    tiles.add("TOL", {"Frequency Tolerance", "Frequency Stability"});
    return tiles.take();
  }

  if (categoryContains(item, {"sensor", "temperature sensor", "pressure sensor"}) ||
      itemTextContains(item, {"sensor", "imu"})) {
    tiles.add("TYPE", {"Sensor Type", "Type"});
    tiles.add("OUTPUT", {"Output Type", "Output"});
    tiles.add("SUPPLY", {"Voltage - Supply"}, Unit::Volt);
    tiles.add("RES", {"Resolution"});
    return tiles.take();
  }

  if (categoryContains(item, {"switch"}) || nameMentions(item, {"switch"})) {
    tiles.add("CIRCUIT", {"Circuit"});
    tiles.add("FUNCTION", {"Switch Function"});
    tiles.add("RATING", {"Contact Rating @ Voltage", "Contact Rating"});
    tiles.add("ACTUATOR", {"Actuator Type"});
    return tiles.take();
  }

  // Timers, oscillators and other ICs without a dedicated list: supply, speed,
  // current and temperature are what a drawer label needs.
  if (isIcLikeItem(item)) {
    tiles.add("SUPPLY", {"Voltage - Supply (Vcc/Vdd)", "Voltage - Supply", "Supply Voltage", "Operating Voltage"}, Unit::Volt);
    tiles.add("FREQ", {"Frequency", "Clock Frequency"}, Unit::Hertz);
    tiles.add("CURRENT", {"Current - Supply", "Current - Output"}, Unit::Amp);
    tiles.add("TEMP", {"Operating Temperature"}, Unit::Celsius);
    return tiles.take();
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
