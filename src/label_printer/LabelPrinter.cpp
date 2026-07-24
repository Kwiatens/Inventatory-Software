// Inventatory - Hardware Inventory Management System
// Zebra label generation and printer queue integration.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "label_printer/LabelPrinter.h"

#include "core/InventoryInternals.h"
#include "core/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>

#pragma comment(lib, "Winspool.lib")
#endif

namespace inventatory {

using namespace std;

namespace {

string windowsErrorText(unsigned long code) {
#ifdef _WIN32
  if (code == 0) {
    return "Unknown error";
  }

  LPWSTR buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "Windows error " + to_string(code);
  }

  wstring wide(buffer, length);
  LocalFree(buffer);
  string result;
  result.reserve(wide.size());
  for (wchar_t ch : wide) {
    if (ch == L'\r' || ch == L'\n') {
      continue;
    }
    if (ch <= 0x7f) {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('?');
    }
  }
  return trim(result);
#else
  (void)code;
  return "Windows error";
#endif
}

string narrowFromWide(const wstring& value) {
#ifdef _WIN32
  if (value.empty()) {
    return {};
  }
  const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (sizeNeeded <= 0) {
    return {};
  }
  string result(static_cast<size_t>(sizeNeeded), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), sizeNeeded, nullptr, nullptr);
  return result;
#else
  return {};
#endif
}

wstring widenFromUtf8(const string& value) {
#ifdef _WIN32
  if (value.empty()) {
    return {};
  }
  const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
  if (sizeNeeded <= 0) {
    return {};
  }
  wstring result(static_cast<size_t>(sizeNeeded), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), sizeNeeded);
  return result;
#else
  return {};
#endif
}

string uppercaseAscii(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(toupper(ch));
  });
  return value;
}

string lowerAscii(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  return value;
}

string shortCode(const string& value, size_t maxLength = 14) {
  return ellipsize(trim(value), maxLength);
}

string fieldOrBlank(const string& value, size_t maxLength);

string shortParameterLabel(const string& label) {
  const auto key = normalizeKey(label);
  if (key.empty()) {
    return {};
  }

  if (key.find("capacitance") != string::npos || key == "value") {
    return "C";
  }
  if (key.find("resistance") != string::npos) {
    return "R";
  }
  if (key.find("inductance") != string::npos) {
    return "L";
  }
  if (key.find("power") != string::npos || key.find("watts") != string::npos) {
    return "Pwr";
  }
  if (key.find("reversevoltage") != string::npos || key == "vr") {
    return "Vr";
  }
  if (key.find("operatingvoltage") != string::npos || key.find("voltagerated") != string::npos ||
      key.find("ratedvoltage") != string::npos) {
    return "Vr";
  }
  if (key.find("voltagesupply") != string::npos || key == "voltage") {
    return "Vdd";
  }
  if (key.find("voltageoutput") != string::npos || key == "vout") {
    return "Vout";
  }
  if (key.find("voltageinput") != string::npos || key == "vin") {
    return "Vin";
  }
  if (key.find("forwardvoltage") != string::npos || key == "vf") {
    return "Vf";
  }
  if (key.find("reversestandoff") != string::npos) {
    return "Vst";
  }
  if (key.find("breakdown") != string::npos) {
    return "Vbr";
  }
  if (key.find("clamping") != string::npos) {
    return "Vc";
  }
  if (key.find("currentpeakpulse") != string::npos || key.find("peakpulsecurrent") != string::npos) {
    return "Ipp";
  }
  if (key.find("peakpulsepower") != string::npos) {
    return "Ppp";
  }
  if (key.find("saturationcurrent") != string::npos || key.find("isat") != string::npos) {
    return "Isat";
  }
  if (key.find("currentrating") != string::npos || key == "current") {
    return "I";
  }
  if (key.find("currentcontinuousdrain") != string::npos || key == "id") {
    return "Id";
  }
  if (key.find("collectoremittervoltage") != string::npos || key == "vce" || key == "vceo") {
    return "Vce";
  }
  if (key.find("collectorcurrent") != string::npos || key == "ic") {
    return "Ic";
  }
  if (key.find("drainsourcevoltage") != string::npos || key == "vdss" || key == "vds") {
    return "Vds";
  }
  if (key.find("rdson") != string::npos) {
    return "Rds";
  }
  if (key.find("gatecharge") != string::npos || key == "qg") {
    return "Qg";
  }
  if (key == "hfe" || key.find("dccurrentgain") != string::npos) {
    return "hFE";
  }
  if (key.find("frequency") != string::npos) {
    return "F";
  }
  if (key.find("loadcapacitance") != string::npos) {
    return "CL";
  }
  if (key.find("operatingmode") != string::npos) {
    return "Mode";
  }
  if (key.find("temperature") != string::npos) {
    return "Temp";
  }
  if (key.find("sensortype") != string::npos || key == "type") {
    return "Type";
  }
  if (key.find("outputtype") != string::npos || key == "output") {
    return "Out";
  }
  if (key.find("resolution") != string::npos) {
    return "Res";
  }
  if (key.find("accuracy") != string::npos) {
    return "Acc";
  }
  if (key.find("features") != string::npos) {
    return "Feat";
  }
  if (key.find("pins") != string::npos || key.find("numberofpositions") != string::npos || key.find("pincount") != string::npos) {
    return "Pins";
  }
  if (key.find("connector") != string::npos) {
    return "Conn";
  }
  if (key.find("rows") != string::npos) {
    return "Rows";
  }
  if (key.find("pitch") != string::npos) {
    return "Pitch";
  }
  if (key.find("shielding") != string::npos) {
    return "Shield";
  }
  if (key.find("composition") != string::npos) {
    return "Comp";
  }
  if (key.find("temperaturecoefficient") != string::npos || key.find("tempco") != string::npos) {
    return "Tempco";
  }
  if (key.find("coreprocessor") != string::npos || key == "core") {
    return "Core";
  }
  if (key.find("clockspeed") != string::npos || key.find("clockfrequency") != string::npos || key == "speed") {
    return "Clk";
  }
  if (key == "flash" || key.find("programmemorysize") != string::npos) {
    return "Flash";
  }
  if (key == "ram" || key == "memory") {
    return "RAM";
  }
  if (key.find("package") != string::npos) {
    return "Pkg";
  }

  return trim(label);
}

string shortValueLine(const string& label, optional<string> value, size_t maxLength = 24) {
  if (!value) {
    return {};
  }

  const auto cleaned = trim(*value);
  if (cleaned.empty()) {
    return {};
  }

  const auto shortLabel = shortParameterLabel(label);
  if (shortLabel.empty()) {
    return fieldOrBlank(cleaned, maxLength);
  }

  return fieldOrBlank(shortLabel + " " + cleaned, maxLength);
}

string compactDescriptor(const string& value, size_t maxLength = 10) {
  auto cleaned = trim(value);
  if (cleaned.empty()) {
    return {};
  }

  const auto cut = cleaned.find_first_of(",;(/");
  if (cut != string::npos) {
    cleaned = trim(cleaned.substr(0, cut));
  }

  return ellipsize(cleaned, maxLength);
}

string dateOnly(time_t value) {
  const auto ts = nowTimestampString(value);
  if (ts.size() >= 10) {
    return ts.substr(0, 10);
  }
  return ts;
}

string compactJoin(const vector<string>& parts, const string& separator) {
  vector<string> filtered;
  for (const auto& part : parts) {
    const auto cleaned = trim(part);
    if (!cleaned.empty()) {
      filtered.push_back(cleaned);
    }
  }
  return join(filtered, separator.empty() ? ' ' : separator.front());
}

string normalizeResistanceValue(string value) {
  value = trim(value);
  if (value.empty()) {
    return value;
  }

  auto lowered = toLower(value);
  auto stripSuffix = [&](const string& suffix) {
    if (lowered.size() < suffix.size()) {
      return false;
    }
    if (lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) != 0) {
      return false;
    }
    value = trim(value.substr(0, value.size() - suffix.size()));
    lowered = toLower(value);
    return true;
  };

  stripSuffix(" ohms");
  stripSuffix(" ohm");
  stripSuffix("ohms");
  stripSuffix("ohm");

  value = trim(value);
  if (value.empty()) {
    return {};
  }

  return value + u8"\u03A9";
}

string fieldOrBlank(const string& value, size_t maxLength);

string fitSingleLineLabel(const string& value, size_t maxLength) {
  return fieldOrBlank(value, maxLength);
}

struct CableFlagFont {
  int height;
  int width;
};

CableFlagFont cableFlagFont(const string& text) {
  const auto length = text.size();
  if (length <= 4) return {68, 58};
  if (length <= 6) return {56, 38};
  if (length <= 9) return {44, 26};
  if (length <= 13) return {36, 20};
  return {28, 16};
}

vector<string> wrapLabelLines(const string& value, size_t maxWidth, size_t maxLines) {
  vector<string> lines;
  if (maxWidth == 0 || maxLines == 0) {
    return lines;
  }

  istringstream input(trim(value));
  string word;
  string current;

  auto flushCurrent = [&]() {
    if (!current.empty()) {
      lines.push_back(current);
      current.clear();
    }
  };

  while (input >> word) {
    if (word.size() > maxWidth) {
      word = ellipsize(word, maxWidth);
    }

    if (current.empty()) {
      current = word;
      continue;
    }

    if (current.size() + 1 + word.size() <= maxWidth) {
      current.push_back(' ');
      current += word;
      continue;
    }

    flushCurrent();
    if (lines.size() >= maxLines) {
      break;
    }
    current = word;
  }

  flushCurrent();
  if (lines.size() > maxLines) {
    lines.resize(maxLines);
  }

  if (!input.eof() && !lines.empty()) {
    lines.back() = ellipsize(lines.back(), maxWidth);
  }

  return lines;
}

string sanitiseZplFragment(const string& value) {
  string output;
  output.reserve(value.size());
  bool previousSpace = false;
  for (char ch : value) {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (ch == '^' || ch == '~') {
      if (!previousSpace && !output.empty()) {
        output.push_back(' ');
        previousSpace = true;
      }
      continue;
    }
    if (ch == '\r' || ch == '\n' || ch == '\t' || iscntrl(uch)) {
      if (!previousSpace && !output.empty()) {
        output.push_back(' ');
        previousSpace = true;
      }
      continue;
    }
    if (isspace(uch) != 0) {
      if (!previousSpace && !output.empty()) {
        output.push_back(' ');
        previousSpace = true;
      }
      continue;
    }
    output.push_back(ch);
    previousSpace = false;
  }
  return trim(output);
}

string fieldOrBlank(const string& value, size_t maxLength) {
  return sanitiseZplFragment(ellipsize(trim(value), maxLength));
}

string collectLineFromValues(initializer_list<string> values, const string& separator, size_t maxLength) {
  vector<string> parts;
  for (const auto& value : values) {
    const auto cleaned = trim(value);
    if (!cleaned.empty()) {
      parts.push_back(cleaned);
    }
  }
  if (parts.empty()) {
    return {};
  }
  return fieldOrBlank(join(parts, separator.empty() ? ' ' : separator.front()), maxLength);
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

optional<string> firstParameter(const InventoryItem& item, initializer_list<const char*> names) {
  return parameterValue(item, names);
}

optional<string> firstInductanceParameter(const InventoryItem& item) {
  if (const auto value = parameterValueMatching(item, {"Inductance", "Value"}, looksLikeInductanceValue)) {
    return value;
  }
  return extractInductanceFromText(item.notes + " " + item.partName + " " + item.sku);
}

bool itemTextContains(const InventoryItem& item, initializer_list<const char*> needles) {
  const auto textMatches = [&](const string& text) {
    for (const auto* needle : needles) {
      if (containsInsensitive(text, needle)) {
        return true;
      }
    }
    return false;
  };

  if (textMatches(item.category) || textMatches(displayCategory(item.category)) || textMatches(item.partName) ||
      textMatches(item.manufacturer) || textMatches(item.location) || textMatches(item.notes) ||
      textMatches(item.digikeyPartNumber) || textMatches(item.sku)) {
    return true;
  }

  for (const auto& tag : item.tags) {
    if (textMatches(tag)) {
      return true;
    }
  }

  for (const auto& parameter : item.parameters) {
    if (textMatches(parameter.name) || textMatches(parameter.value)) {
      return true;
    }
  }

  return false;
}

vector<string> itemTextTokens(const InventoryItem& item) {
  string text = item.category + " " + displayCategory(item.category) + " " + item.partName + " " +
                item.manufacturer + " " + item.location + " " + item.notes + " " + item.digikeyPartNumber +
                " " + item.sku;
  for (const auto& tag : item.tags) text += " " + tag;
  for (const auto& parameter : item.parameters) text += " " + parameter.name + " " + parameter.value;

  vector<string> tokens;
  string current;
  for (unsigned char ch : text) {
    if (isalnum(ch)) {
      current.push_back(static_cast<char>(tolower(ch)));
    } else if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

bool itemTextHasToken(const InventoryItem& item, initializer_list<const char*> tokens) {
  const auto haystack = itemTextTokens(item);
  for (const auto* token : tokens) {
    const auto normalized = normalizeKey(token);
    if (normalized.empty()) {
      continue;
    }
    if (find(haystack.begin(), haystack.end(), normalized) != haystack.end()) {
      return true;
    }
  }
  return false;
}

bool hasParameter(const InventoryItem& item, initializer_list<const char*> names) {
  return findParameter(item.parameters, names) != nullptr;
}

string sensorContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"3 axis", "3-axis", "three axis", "imu", "inertial measurement unit"}) ||
      (itemTextContains(item, {"accelerometer"}) && itemTextContains(item, {"gyroscope", "gyro"}))) {
    return "3 Axis IMU";
  }
  if (itemTextContains(item, {"temperature sensor", "temp sensor", "thermometer", "thermocouple"}) ||
      (itemTextContains(item, {"temperature"}) && !itemTextContains(item, {"coefficient"}))) {
    return "Temp Sensor";
  }
  if (itemTextContains(item, {"pressure"})) {
    return "Pressure Sensor";
  }
  if (itemTextContains(item, {"humidity"})) {
    return "Humidity Sensor";
  }
  if (itemTextContains(item, {"current sensor", "current sensing", "current monitor"})) {
    return "Current Sensor";
  }
  if (itemTextContains(item, {"accelerometer", "accel"})) {
    return "Accelerometer";
  }
  if (itemTextContains(item, {"gyroscope", "gyro"})) {
    return "Gyroscope";
  }
  if (itemTextContains(item, {"magnetometer"})) {
    return "Magnetometer";
  }
  if (itemTextContains(item, {"hall effect", "hall sensor"})) {
    return "Hall Sensor";
  }
  if (itemTextContains(item, {"proximity", "distance", "time of flight", "tof", "gesture"})) {
    return "Proximity Sensor";
  }
  if (itemTextContains(item, {"ambient light", "light sensor", "color sensor", "optical"})) {
    return "Light Sensor";
  }
  if (itemTextContains(item, {"gas sensor", "air quality", "voc", "co2", "air quality sensor"})) {
    return "Gas Sensor";
  }
  if (itemTextContains(item, {"touch sensor", "capacitive touch"})) {
    return "Touch Sensor";
  }
  return {};
}

string diodeContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"tvs", "transient voltage suppressor", "esd protection", "surge protection"})) {
    return "TVS Diode";
  }
  if (itemTextContains(item, {"schottky"})) {
    return "Schottky Diode";
  }
  if (itemTextContains(item, {"rectifier"})) {
    return "Rectifier Diode";
  }
  if (itemTextContains(item, {"zener"})) {
    return "Zener Diode";
  }
  return "Diode";
}

bool startsWithInsensitive(const string& value, const string& prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  return lowerAscii(value.substr(0, prefix.size())) == lowerAscii(prefix);
}

string diodeMainLabelValue(const InventoryItem& item) {
  const auto partName = trim(item.partName);
  const auto sku = trim(item.sku);

  if (partName.empty()) {
    if (!sku.empty()) {
      return sku;
    }
    return item.category;
  }

  const auto cleaned = [&](const string& prefix) -> string {
    if (!startsWithInsensitive(partName, prefix)) {
      return {};
    }
    const auto stripped = trim(partName.substr(prefix.size()));
    return stripped;
  };

  if (const auto stripped = cleaned("tvs diode "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("diode tvs "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("transient voltage suppressor diode "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("schottky diode "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("rectifier diode "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("zener diode "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("diode "); !stripped.empty()) return stripped;

  return partName;
}

bool isIcLikeItem(const InventoryItem& item) {
  if (categoryContains(item, {"integrated circuit", "integrated circuits", "mcu", "microcontroller", "memory",
                              "logic", "amplifier", "comparator", "reference", "regulator", "power management",
                              "pmic", "sensor", "interface", "transceiver", "oscillator", "clock", "timer",
                              "converter", "data acquisition", "charger", "supervisor", "protection", "driver",
                              "codec", "audio", "power path", "load switch", "gate driver",
                              "motor driver", "h-bridge"})) {
    return true;
  }

  return itemTextContains(item, {"integrated circuit", "integrated circuits", "microcontroller", "microprocessor",
                                 "power management ic", "pmic", "buck converter", "boost converter",
                                 "buck-boost", "buck boost", "dc-dc converter", "switching regulator",
                                 "low dropout", "ldo", "voltage regulator", "op amp", "op-amp",
                                 "operational amplifier", "instrumentation amplifier", "comparator",
                                 "voltage reference", "logic buffer", "logic gate", "logic inverter",
                                 "flip-flop", "flip flop", "latch", "mux", "demux", "multiplexer",
                                 "demultiplexer", "transceiver", "level shifter", "voltage translator",
                                 "motor driver", "gate driver", "h-bridge", "battery charger", "load switch",
                                 "voltage supervisor", "reset ic", "watchdog", "temperature sensor",
                                 "pressure sensor", "humidity sensor", "accelerometer", "gyroscope",
                                 "magnetometer", "adc", "dac", "data converter", "clock generator",
                                 "oscillator", "rtc", "memory", "flash", "eeprom", "sram"});
}

string powerIcContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"buck-boost", "buck boost"})) {
    return "Buck-Boost Conv.";
  }
  if (itemTextContains(item, {"buck converter", "buck regulator", "step-down", "step down", "synchronous buck"})) {
    return "Buck Converter";
  }
  if (itemTextContains(item, {"boost converter", "boost regulator", "step-up", "step up"})) {
    return "Boost Converter";
  }
  if (itemTextContains(item, {"battery charger", "charger"})) {
    return "Battery Charger";
  }
  if (itemTextContains(item, {"load switch", "power distribution switch", "hot swap", "efuse", "e-fuse"})) {
    return "Load Switch";
  }
  if (itemTextContains(item, {"voltage supervisor", "supervisor", "reset ic", "power-on reset", "brownout",
                              "voltage detector", "watchdog"})) {
    return "Voltage Supervisor";
  }
  if (itemTextContains(item, {"protection ic", "protection array", "esd protection", "surge protection",
                              "overvoltage protection", "reverse polarity", "current limit", "power path"})) {
    return "Protection IC";
  }
  if (itemTextContains(item, {"low dropout", "ldo", "linear regulator", "voltage regulator", "regulator"})) {
    return "Regulator";
  }
  if (itemTextContains(item, {"dc-dc", "dc dc", "switching regulator", "power management", "pmic"})) {
    return "Power IC";
  }
  return {};
}

string analogIcContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"op amp", "op-amp", "operational amplifier", "operational amp",
                              "gain bandwidth", "slew rate", "input offset voltage", "rail-to-rail",
                              "common-mode rejection"})) {
    return "OP-AMP";
  }
  if (itemTextContains(item, {"comparator", "window comparator"})) {
    return "Comparator";
  }
  if (itemTextContains(item, {"voltage reference", "reference voltage", "bandgap reference", "shunt reference",
                              "reference ic"})) {
    return "Voltage Ref.";
  }
  if (itemTextContains(item, {"instrumentation amplifier", "current sense amplifier", "amplifier"})) {
    return "Amplifier";
  }
  return {};
}

string sensorIcContextHeader(const InventoryItem& item) {
  const auto sensor = sensorContextHeader(item);
  if (!sensor.empty()) {
    return sensor;
  }
  return "Sensor IC";
}

string dataConverterContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"analog to digital", "analog-to-digital", "adc"})) {
    return "ADC";
  }
  if (itemTextContains(item, {"digital to analog", "digital-to-analog", "dac"})) {
    return "DAC";
  }
  if (itemTextContains(item, {"data converter", "data acquisition"})) {
    return "Data Conv.";
  }
  return {};
}

string timingIcContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"rtc", "real time clock", "real-time clock"})) {
    return "RTC";
  }
  if (itemTextContains(item, {"clock generator", "clock synthesizer", "frequency synthesizer", "pll"})) {
    return "Clock Gen.";
  }
  if (categoryContains(item, {"timer", "timers", "programmable timers"}) ||
      itemTextContains(item, {"555 type", "timer/oscillator", "timer oscillator", "timers and oscillators",
                              "programmable timer", "delay timer", "watchdog timer", "timer ic",
                              "monostable", "astable", "multivibrator", "one-shot", "one shot",
                              "interval timer", "pulse timer"}) ||
      itemTextHasToken(item, {"timer", "timers", "counter", "counters"})) {
    return "Timer IC";
  }
  if (itemTextContains(item, {"oscillator", "osc"})) {
    return "Oscillator";
  }
  return {};
}

string driverIcContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"h-bridge", "h bridge"})) {
    return "H-Bridge";
  }
  if (itemTextContains(item, {"motor driver", "stepper driver", "servo driver"})) {
    return "Motor Driver";
  }
  if (itemTextContains(item, {"gate driver", "mosfet driver", "high side driver", "low side driver"})) {
    return "Gate Driver";
  }
  if (itemTextContains(item, {"led driver", "display driver", "line driver"})) {
    return "Driver IC";
  }
  if (itemTextContains(item, {"driver"})) {
    return "Driver IC";
  }
  return {};
}

string logicIcContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"transceiver", "bus transceiver"})) {
    return "Transceiver";
  }
  if (itemTextContains(item, {"level shifter", "logic translator", "voltage translator"})) {
    return "Level Shifter";
  }
  if (itemTextContains(item, {"multiplexer", "demultiplexer", "mux", "demux"})) {
    return "Mux/Demux";
  }
  if (itemTextContains(item, {"flip-flop", "flip flop"})) {
    return "Flip-Flop";
  }
  if (itemTextContains(item, {"latch"})) {
    return "Latch";
  }
  if (itemTextContains(item, {"inverter", "not gate"})) {
    return "Inverter";
  }
  if (itemTextContains(item, {"logic gate", "and gate", "or gate", "nand", "nor", "xor", "xnor"})) {
    return "Logic Gate";
  }
  if (itemTextContains(item, {"logic buffer", "bus buffer", "buffer"})) {
    return "Logic Buffer";
  }
  if (itemTextContains(item, {"logic"})) {
    return "Logic IC";
  }
  if (itemTextContains(item, {"interface", "phy", "usb", "can", "rs-485", "ethernet", "spi", "i2c", "uart"})) {
    return "Interface IC";
  }
  return {};
}

string memoryIcContextHeader(const InventoryItem& item) {
  if (categoryContains(item, {"memory"}) ||
      hasParameter(item, {"Memory Type", "Memory Format", "Memory Size", "Program Memory Size",
                          "Program Memory Type", "Memory Interface"})) {
    return "Memory IC";
  }
  if (itemTextContains(item, {"memory", "flash memory", "eeprom", "sram", "dram", "fram", "non-volatile memory",
                              "volatile memory"})) {
    return "Memory IC";
  }
  return {};
}

string integratedCircuitContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"microcontroller", "mcu", "microprocessor", "processor"})) {
    return "MCU";
  }
  if (const auto power = powerIcContextHeader(item); !power.empty()) {
    return power;
  }
  if (const auto dataConverter = dataConverterContextHeader(item); !dataConverter.empty()) {
    return dataConverter;
  }
  if (const auto sensor = sensorIcContextHeader(item); !sensor.empty() && sensor != "Sensor IC") {
    return sensor;
  }
  if (const auto timing = timingIcContextHeader(item); !timing.empty()) {
    return timing;
  }
  if (const auto memory = memoryIcContextHeader(item); !memory.empty()) {
    return memory;
  }
  if (const auto analog = analogIcContextHeader(item); !analog.empty()) {
    return analog;
  }
  if (const auto driver = driverIcContextHeader(item); !driver.empty()) {
    return driver;
  }
  if (const auto logic = logicIcContextHeader(item); !logic.empty()) {
    return logic;
  }
  if (itemTextContains(item, {"sensor", "imu", "accelerometer", "gyroscope", "gyro", "magnetometer",
                              "temperature", "humidity", "pressure", "current sensor"})) {
    return sensorIcContextHeader(item);
  }
  if (itemTextContains(item, {"transceiver", "interface"})) {
    return "Interface IC";
  }
  if (itemTextContains(item, {"power management", "pmic"})) {
    return "Power IC";
  }
  if (categoryContains(item, {"integrated circuit", "integrated circuits"})) {
    return "Integrated Circuit";
  }
  if (categoryContains(item, {"power management", "regulator", "charger"})) {
    return "Power IC";
  }
  if (categoryContains(item, {"logic"})) {
    return "Logic IC";
  }
  if (categoryContains(item, {"amplifier", "comparator", "reference"})) {
    return "Analog IC";
  }
  if (categoryContains(item, {"sensor"})) {
    return "Sensor IC";
  }
  if (categoryContains(item, {"timing", "clock", "oscillator", "rtc"})) {
    return "Timing IC";
  }
  if (categoryContains(item, {"memory"})) {
    return "Memory IC";
  }
  return "Integrated Circuit";
}

string transistorContextHeader(const InventoryItem& item) {
  if (itemTextContains(item, {"mosfet", "fet"})) {
    return "MOSFET";
  }
  if (itemTextContains(item, {"bjt", "npn", "pnp", "transistor"})) {
    return "Transistor";
  }
  return "Transistor";
}

string fallbackContextHeader(const InventoryItem& item) {
  const auto category = trim(displayCategory(item.category));
  if (!category.empty()) {
    return category;
  }
  if (!trim(item.partName).empty()) {
    return trim(item.partName);
  }
  if (!trim(item.sku).empty()) {
    return trim(item.sku);
  }
  return "Part";
}

string mainLabelValue(const InventoryItem& item) {
  if (categoryContains(item, {"capacitor"})) {
    return collectLineFromValues({firstParameter(item, {"Capacitance", "Value"}).value_or({}),
                                  firstParameter(item, {"Tolerance"}).value_or({})},
                                 " ", 24);
  }
  if (categoryContains(item, {"resistor"})) {
    return collectLineFromValues({normalizeResistanceValue(firstParameter(item, {"Resistance", "Value"}).value_or({})),
                                  firstParameter(item, {"Tolerance"}).value_or({})},
                                 " ", 24);
  }
  if (categoryContains(item, {"inductor", "choke", "coil"})) {
    return collectLineFromValues({firstInductanceParameter(item).value_or({}),
                                  firstParameter(item, {"Current Rating", "Current"}).value_or({})},
                                 " ", 24);
  }

  if (categoryContains(item, {"diode", "rectifier", "schottky", "transient voltage suppressor"}) ||
      itemTextContains(item, {"diode", "rectifier", "schottky", "zener"})) {
    return diodeMainLabelValue(item);
  }

  if (!trim(item.partName).empty()) {
    return item.partName;
  }
  if (!trim(item.sku).empty()) {
    return item.sku;
  }
  return item.category;
}

string shortPackageLine(const InventoryItem& item) {
  const auto package = firstParameter(item, {"Package / Case", "Package Case", "Case / Package", "Case Package",
                                             "Supplier Device Package", "Device Package", "Package"});
  const auto size = firstParameter(item, {"Size / Dimension", "Dimensions"});
  if (package && !trim(*package).empty()) {
    return fieldOrBlank(compactDescriptor(*package), 10);
  }
  if (size && !trim(*size).empty()) {
    return fieldOrBlank(compactDescriptor(*size), 10);
  }
  return {};
}

string manufacturerLine(const InventoryItem& item) {
  const auto manufacturer = trim(item.manufacturer);
  if (!manufacturer.empty()) {
    return fitSingleLineLabel(manufacturer, 20);
  }
  if (!trim(item.partName).empty()) {
    return fitSingleLineLabel(item.partName, 20);
  }
  if (!trim(item.sku).empty()) {
    return fitSingleLineLabel(item.sku, 20);
  }
  return fitSingleLineLabel(displayCategory(item.category), 20);
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
  return fitSingleLineLabel(cleaned, 16);
}

vector<string> fallbackDetailLines(const InventoryItem& item, size_t maxLines) {
  vector<string> lines;
  for (const auto& field : electricalFieldsForItem(item)) {
    const auto value = trim(field.value);
    if (value.empty()) {
      continue;
    }
    if (containsInsensitive(field.label, "package")) {
      continue;
    }
    lines.push_back(shortValueLine(field.label, value, 24));
    if (lines.size() >= maxLines) {
      break;
    }
  }
  return lines;
}

vector<string> parameterLinesForItem(const InventoryItem& item) {
  vector<string> lines;

  if (categoryContains(item, {"capacitor"})) {
    if (const auto capacitance = firstParameter(item, {"Capacitance", "Value"})) {
      lines.push_back(shortValueLine("Capacitance", capacitance, 24));
    }
    if (const auto voltage = firstParameter(item, {"Operating Voltage", "Voltage", "Voltage - Rated", "Rated Voltage"})) {
      lines.push_back(shortValueLine("Operating Voltage", voltage, 24));
    }
    if (const auto dielectric = firstParameter(item, {"Type", "Dielectric", "Dielectric Type"})) {
      lines.push_back(shortValueLine("Type", dielectric, 24));
    }
    if (const auto esr = firstParameter(item, {"ESR", "ESR (Equivalent Series Resistance)"})) {
      lines.push_back(shortValueLine("ESR", esr, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"resistor"})) {
    if (const auto resistance = firstParameter(item, {"Resistance", "Value"})) {
      lines.push_back(shortValueLine("Resistance", resistance, 24));
    }
    if (const auto power = firstParameter(item, {"Power Dissipation", "Power (Watts)", "Power Rating", "Power",
                                                 "Power - Max", "Watts"})) {
      lines.push_back(shortValueLine("Power", power, 24));
    }
    if (const auto composition = firstParameter(item, {"Composition"})) {
      lines.push_back(shortValueLine("Composition", composition, 24));
    }
    if (const auto tempco = firstParameter(item, {"Temperature Coefficient", "Tempco"})) {
      lines.push_back(shortValueLine("Tempco", tempco, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"inductor", "choke", "coil"})) {
    if (const auto inductance = firstInductanceParameter(item)) {
      lines.push_back(shortValueLine("Inductance", inductance, 24));
    }
    if (const auto current = firstParameter(item, {"Current Rating", "Current Rating (Amps)", "Current"})) {
      lines.push_back(shortValueLine("Current Rating", current, 24));
    }
    if (const auto saturation = firstParameter(item, {"Saturation Current", "Current - Saturation (Isat)"})) {
      lines.push_back(shortValueLine("Saturation Current", saturation, 24));
    }
    string frequencyLine;
    if (const auto frequency = firstParameter(item, {"Frequency - Self Resonant", "Frequency"})) {
      frequencyLine = shortParameterLabel("Frequency") + " " + trim(*frequency);
    }
    const auto shielded = normalizedShieldingLine(firstParameter(item, {"Shielding"}));
    if (!shielded.empty()) {
      if (!frequencyLine.empty()) {
        frequencyLine += ' ';
      }
      frequencyLine += shielded;
    }
    if (!frequencyLine.empty()) {
      lines.push_back(fitSingleLineLabel(frequencyLine, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"mcu", "microcontroller"})) {
    if (const auto voltage = firstParameter(item, {"Operating Voltage", "Voltage - Supply", "Voltage - Supply (Min/Max)", "Voltage"})) {
      lines.push_back(shortValueLine("Voltage - Supply", voltage, 24));
    }

    string coreLine;
    if (const auto core = firstParameter(item, {"Core", "Core Processor"})) {
      coreLine = trim(*core);
    }
    if (const auto clock = firstParameter(item, {"Clock Speed", "Clock Frequency", "Speed"})) {
      if (!coreLine.empty()) {
        coreLine += " @ ";
      }
      coreLine += trim(*clock);
    }
    if (!coreLine.empty()) {
      lines.push_back(fitSingleLineLabel("Core " + coreLine, 24));
    }

    string memoryLine;
    if (const auto flash = firstParameter(item, {"Flash", "Program Memory Size"})) {
      memoryLine = "Flash " + trim(*flash);
    }
    if (const auto ram = firstParameter(item, {"RAM", "Memory"})) {
      if (!memoryLine.empty()) {
        memoryLine += " / ";
      }
      memoryLine += "RAM " + trim(*ram);
    }
    if (!memoryLine.empty()) {
      lines.push_back(fitSingleLineLabel(memoryLine, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"transistor", "mosfet", "fet", "discrete semiconductor"})) {
    if (categoryContains(item, {"mosfet", "fet"})) {
      if (const auto drainSource = firstParameter(item, {"Drain-Source Voltage", "Drain to Source Voltage (Vdss)", "Vds", "Vdss"})) {
        lines.push_back(shortValueLine("Vds", drainSource, 24));
      }
      if (const auto current = firstParameter(item, {"Continuous Drain Current", "Current - Continuous Drain (Id) @ 25°C", "Current", "Id"})) {
        lines.push_back(shortValueLine("Id", current, 24));
      }
      vector<string> thirdLine;
      if (const auto rds = firstParameter(item, {"Rds On", "Rds On (Max) @ Id, Vgs", "RDS(ON)"})) {
        thirdLine.push_back(shortValueLine("Rds", rds, 24));
      }
      if (const auto gateCharge = firstParameter(item, {"Gate Charge", "Gate Charge (Qg) (Max) @ Vgs"})) {
        thirdLine.push_back(shortValueLine("Qg", gateCharge, 24));
      }
      if (!thirdLine.empty()) {
        if (thirdLine.size() == 1) {
          lines.push_back(thirdLine[0]);
        } else {
          lines.push_back(fitSingleLineLabel(thirdLine[0] + " / " + thirdLine[1], 24));
        }
      }
      return lines;
    }

    if (const auto collectorEmitter = firstParameter(item, {"Collector-Emitter Voltage", "Collector Emitter Voltage", "Vce", "Vceo"})) {
      lines.push_back(shortValueLine("Vce", collectorEmitter, 24));
    } else if (const auto voltage = firstParameter(item, {"Voltage", "Voltage - Collector Emitter", "Voltage - CE", "Vceo"})) {
      lines.push_back(shortValueLine("Vce", voltage, 24));
    }
    if (const auto current = firstParameter(item, {"Collector Current", "Current", "Ic", "Continuous Collector Current"})) {
      lines.push_back(shortValueLine("Ic", current, 24));
    }
    if (const auto gain = firstParameter(item, {"hFE", "DC Current Gain", "Gain"})) {
      lines.push_back(shortValueLine("hFE", gain, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"diode", "rectifier", "schottky"}) || itemTextContains(item, {"diode", "rectifier", "schottky"})) {
    if (const auto forward = firstParameter(item, {"Forward Voltage", "Voltage - Forward (Vf) (Max) @ If", "Vf"})) {
      lines.push_back(shortValueLine("Vf", forward, 24));
    }
    if (const auto reverse = firstParameter(item, {"Reverse Voltage", "Voltage - DC Reverse (Vr) (Max)", "Peak Reverse Voltage", "Vr"})) {
      lines.push_back(shortValueLine("Vr", reverse, 24));
    }
    if (const auto current = firstParameter(item, {"Current", "Current - Average Rectified (Io)", "If", "Forward Current"})) {
      lines.push_back(shortValueLine("Io", current, 24));
    }
    if (const auto technology = firstParameter(item, {"Technology"})) {
      lines.push_back(shortValueLine("Tech", technology, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"connector"}) || itemTextContains(item, {"connector"})) {
    if (const auto pins = firstParameter(item, {"Pins", "Number of Positions", "Pin Count"})) {
      lines.push_back(shortValueLine("Pins", pins, 24));
    }
    if (const auto connectorType = firstParameter(item, {"Connector Type"})) {
      lines.push_back(shortValueLine("Conn", connectorType, 24));
    }
    if (const auto rows = firstParameter(item, {"Rows", "Number of Rows"})) {
      lines.push_back(shortValueLine("Rows", rows, 24));
    }
    if (const auto pitch = firstParameter(item, {"Pitch", "Pitch - Mating"})) {
      lines.push_back(shortValueLine("Pitch", pitch, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"regulator", "voltage regulator", "power management"}) ||
      itemTextContains(item, {"regulator", "ldo", "buck", "boost"})) {
    if (const auto outputVoltage = firstParameter(item, {"Output Voltage", "Voltage - Output", "Vout"})) {
      lines.push_back(shortValueLine("Vout", outputVoltage, 24));
    }
    if (const auto inputVoltage = firstParameter(item, {"Voltage - Input", "Vin"})) {
      lines.push_back(shortValueLine("Vin", inputVoltage, 24));
    }
    if (const auto current = firstParameter(item, {"Output Current", "Current - Output", "Iout"})) {
      lines.push_back(shortValueLine("Iout", current, 24));
    }
    if (const auto type = firstParameter(item, {"Type", "Output Type"})) {
      lines.push_back(shortValueLine("Type", type, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"crystal", "oscillator", "resonator"}) ||
      itemTextContains(item, {"crystal", "oscillator", "resonator"})) {
    if (const auto frequency = firstParameter(item, {"Frequency"})) {
      lines.push_back(shortValueLine("Frequency", frequency, 24));
    }
    if (const auto loadCapacitance = firstParameter(item, {"Load Capacitance"})) {
      lines.push_back(shortValueLine("Load Capacitance", loadCapacitance, 24));
    }
    if (const auto esr = firstParameter(item, {"ESR", "Equivalent Series Resistance"})) {
      lines.push_back(shortValueLine("ESR", esr, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"sensor", "temperature sensor", "pressure sensor"}) ||
      itemTextContains(item, {"sensor", "imu"})) {
    if (const auto type = firstParameter(item, {"Type", "Sensor Type"})) {
      lines.push_back(shortValueLine("Type", type, 24));
    }
    if (const auto output = firstParameter(item, {"Output", "Output Type"})) {
      lines.push_back(shortValueLine("Out", output, 24));
    }
    if (const auto voltage = firstParameter(item, {"Voltage - Supply"})) {
      lines.push_back(shortValueLine("Vdd", voltage, 24));
    }
    if (const auto resolution = firstParameter(item, {"Resolution"})) {
      lines.push_back(shortValueLine("Res", resolution, 24));
    }
    return lines;
  }

  if (categoryContains(item, {"circuit protection", "fuse", "tvs", "transient voltage suppressor"}) ||
      itemTextContains(item, {"tvs", "transient voltage suppressor", "surge protection", "esd protection"})) {
    if (const auto standoff = firstParameter(item, {"Voltage - Reverse Standoff (Typ)", "Reverse Standoff"})) {
      lines.push_back(shortValueLine("Vst", standoff, 24));
    } else if (const auto breakdown = firstParameter(item, {"Voltage - Breakdown (Min)", "Breakdown"})) {
      lines.push_back(shortValueLine("Vbr", breakdown, 24));
    }
    if (const auto clamping = firstParameter(item, {"Voltage - Clamping (Max) @ Ipp", "Clamping"})) {
      lines.push_back(shortValueLine("Vc", clamping, 24));
    }
    if (const auto current = firstParameter(item, {"Current - Peak Pulse (10/1000µs)", "Current - Peak Pulse (10/1000Âµs)",
                                                   "Peak Pulse Current", "Current Rating", "Current"})) {
      lines.push_back(shortValueLine("Ipp", current, 24));
    }
    if (const auto power = firstParameter(item, {"Power - Peak Pulse", "Peak Pulse Power"})) {
      lines.push_back(shortValueLine("Ppp", power, 24));
    }
    return lines;
  }

  return fallbackDetailLines(item, 3);
}

string makeJobName(const InventoryItem& item) {
  const auto id = trim(item.inventatoryId);
  if (!id.empty()) {
    return "Inventatory Label " + id;
  }
  if (!trim(item.partName).empty()) {
    return "Inventatory Label " + item.partName;
  }
  return "Inventatory Label";
}

string rackDisplayCategory(string value) {
  value = displayCategory(value);
  replace(value.begin(), value.end(), '-', ' ');
  replace(value.begin(), value.end(), '_', ' ');
  value = trim(value);
  if (value.empty()) {
    return "RACK";
  }
  return uppercaseAscii(fieldOrBlank(value, 20));
}

string rackLabelText(const string& code) {
  const auto number = rackNumberFromCode(code);
  if (number > 0) {
    ostringstream out;
    out << "RACK " << setw(2) << setfill('0') << number;
    return out.str();
  }
  const auto cleaned = trim(code);
  return cleaned.empty() ? "RACK" : fieldOrBlank("RACK " + cleaned, 12);
}

vector<string> rackCategoryLines(const string& category) {
  vector<string> words;
  istringstream input(trim(category));
  string word;
  while (input >> word) {
    words.push_back(word);
  }

  if (words.size() <= 1) {
    return {fieldOrBlank(category, 20)};
  }

  if (words.size() == 2) {
    return {fieldOrBlank(words[0], 14), fieldOrBlank(words[1], 14)};
  }

  const size_t splitAt = (words.size() + 1) / 2;
  vector<string> firstWords(words.begin(), words.begin() + splitAt);
  vector<string> secondWords(words.begin() + splitAt, words.end());
  vector<string> lines = {join(firstWords, ' '), join(secondWords, ' ')};
  for (auto& line : lines) {
    line = fieldOrBlank(line, 14);
  }
  return lines;
}

string rackCategoryFieldData(const vector<string>& lines) {
  if (lines.empty()) {
    return "RACK";
  }
  string output;
  for (const auto& line : lines) {
    if (!output.empty()) {
      output += "\\&";
    }
    output += sanitizeLabelText(line);
  }
  return output;
}

string makeRackJobName(const InventatoryRack& rack) {
  const auto code = trim(rack.code);
  return code.empty() ? "Inventatory Rack" : "Inventatory Rack " + code;
}

#ifdef _WIN32
PrinterQueueInfo makePrinterInfo(const wstring& name, const wstring& driver, const wstring& port, DWORD status,
                                 bool isDefault) {
  PrinterQueueInfo info;
  info.name = narrowFromWide(name);
  info.driverName = narrowFromWide(driver);
  info.portName = narrowFromWide(port);
  info.isDefault = isDefault;

  vector<string> statusParts;
  if (status == 0) {
    info.isReady = true;
    statusParts.push_back("Ready");
  } else {
    if (status & PRINTER_STATUS_PAUSED) {
      statusParts.push_back("Paused");
    }
    if (status & PRINTER_STATUS_OFFLINE) {
      statusParts.push_back("Offline");
    }
    if (status & PRINTER_STATUS_ERROR) {
      statusParts.push_back("Error");
    }
    if (status & PRINTER_STATUS_PAPER_OUT) {
      statusParts.push_back("Label stock empty");
    }
    if (status & PRINTER_STATUS_NOT_AVAILABLE) {
      statusParts.push_back("Not available");
    }
    if (status & PRINTER_STATUS_NO_TONER) {
      statusParts.push_back("Media error");
    }
    if (status & PRINTER_STATUS_DOOR_OPEN) {
      statusParts.push_back("Door open");
    }
    info.isReady = statusParts.empty();
  }
  if (statusParts.empty()) {
    statusParts.push_back("Ready");
  }
  info.statusText = join(statusParts, ';');
  return info;
}

class WindowsPrinterBackend final : public PrinterBackend {
 public:
  vector<PrinterQueueInfo> enumeratePrinters() const override {
    vector<PrinterQueueInfo> printers;
    DWORD bytesNeeded = 0;
    DWORD count = 0;
    const DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    EnumPrintersW(flags, nullptr, 2, nullptr, 0, &bytesNeeded, &count);
    if (bytesNeeded == 0) {
      return printers;
    }

    vector<BYTE> buffer(bytesNeeded);
    if (!EnumPrintersW(flags, nullptr, 2, buffer.data(), bytesNeeded, &bytesNeeded, &count)) {
      return printers;
    }

    const auto* entries = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
    printers.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
      const auto& entry = entries[index];
      printers.push_back(makePrinterInfo(entry.pPrinterName ? entry.pPrinterName : L"",
                                        entry.pDriverName ? entry.pDriverName : L"",
                                        entry.pPortName ? entry.pPortName : L"", entry.Status,
                                        (entry.Attributes & PRINTER_ATTRIBUTE_DEFAULT) != 0));
    }

    sort(printers.begin(), printers.end(), [](const PrinterQueueInfo& lhs, const PrinterQueueInfo& rhs) {
      if (lhs.isDefault != rhs.isDefault) {
        return lhs.isDefault > rhs.isDefault;
      }
      return lowerAscii(lhs.name) < lowerAscii(rhs.name);
    });
    return printers;
  }

  PrinterCheckResult probePrinter(const string& printerName) const override {
    PrinterCheckResult result;
    if (trim(printerName).empty()) {
      result.message = "No printer configured";
      return result;
    }

    const auto wideName = widenFromUtf8(printerName);
    if (wideName.empty()) {
      result.message = "Printer name could not be converted";
      return result;
    }

    PRINTER_DEFAULTSW defaults{};
    defaults.DesiredAccess = PRINTER_ACCESS_USE;
    HANDLE handle = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(wideName.c_str()), &handle, &defaults)) {
      result.message = "Unable to open printer queue: " + windowsErrorText(GetLastError());
      return result;
    }

    unique_ptr<void, decltype(&ClosePrinter)> printerHandle(handle, ClosePrinter);
    DWORD bytesNeeded = 0;
    GetPrinterW(handle, 2, nullptr, 0, &bytesNeeded);
    if (bytesNeeded == 0) {
      result.ok = true;
      result.message = "Printer queue opened";
      return result;
    }

    vector<BYTE> buffer(bytesNeeded);
    if (!GetPrinterW(handle, 2, buffer.data(), bytesNeeded, &bytesNeeded)) {
      result.message = "Unable to query printer status: " + windowsErrorText(GetLastError());
      return result;
    }

    const auto* info = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
    const auto queue = makePrinterInfo(info->pPrinterName ? info->pPrinterName : L"",
                                       info->pDriverName ? info->pDriverName : L"",
                                       info->pPortName ? info->pPortName : L"", info->Status,
                                       (info->Attributes & PRINTER_ATTRIBUTE_DEFAULT) != 0);
    result.ok = queue.isReady;
    result.message = queue.statusText;
    return result;
  }

  bool sendRawJob(const string& printerName, const string& jobName, const string& zpl, string* error) const override {
    const auto wideName = widenFromUtf8(printerName);
    if (wideName.empty()) {
      if (error != nullptr) {
        *error = "Printer name could not be converted";
      }
      return false;
    }

    PRINTER_DEFAULTSW defaults{};
    defaults.DesiredAccess = PRINTER_ACCESS_USE;
    HANDLE handle = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(wideName.c_str()), &handle, &defaults)) {
      if (error != nullptr) {
        *error = "Unable to open printer queue: " + windowsErrorText(GetLastError());
      }
      return false;
    }

    unique_ptr<void, decltype(&ClosePrinter)> printerHandle(handle, ClosePrinter);

    DOC_INFO_1W doc{};
    const auto jobWide = widenFromUtf8(jobName);
    const auto rawWide = widenFromUtf8("RAW");
    doc.pDocName = const_cast<LPWSTR>(jobWide.empty() ? L"Inventatory Label" : jobWide.c_str());
    doc.pOutputFile = nullptr;
    doc.pDatatype = const_cast<LPWSTR>(rawWide.empty() ? L"RAW" : rawWide.c_str());

    if (StartDocPrinterW(handle, 1, reinterpret_cast<LPBYTE>(&doc)) == 0) {
      if (error != nullptr) {
        *error = "Unable to start printer job: " + windowsErrorText(GetLastError());
      }
      return false;
    }

    bool success = false;
    const auto endDoc = [&]() {
      EndDocPrinter(handle);
    };

    do {
      if (!StartPagePrinter(handle)) {
        if (error != nullptr) {
          *error = "Unable to start printer page: " + windowsErrorText(GetLastError());
        }
        break;
      }

      const auto body = zpl;
      DWORD bytesWritten = 0;
      if (!WritePrinter(handle, const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), &bytesWritten) ||
          bytesWritten != body.size()) {
        if (error != nullptr) {
          *error = "Unable to write print data: " + windowsErrorText(GetLastError());
        }
        break;
      }

      if (!EndPagePrinter(handle)) {
        if (error != nullptr) {
          *error = "Unable to finish printer page: " + windowsErrorText(GetLastError());
        }
        break;
      }

      success = true;
    } while (false);

    endDoc();
    return success;
  }
};
#endif

string partContextHeader(const InventoryItem& item) {
  return partShortDescription(item);
}

}  // namespace

LabelPrinterService::LabelPrinterService(unique_ptr<PrinterBackend> backend)
    : backend_(move(backend)) {
#ifdef _WIN32
  if (backend_ == nullptr) {
    backend_ = make_unique<WindowsPrinterBackend>();
  }
#endif
}

LabelPrinterService::~LabelPrinterService() = default;

vector<PrinterQueueInfo> LabelPrinterService::enumeratePrinters() const {
  if (backend_ == nullptr) {
    return {};
  }
  return backend_->enumeratePrinters();
}

bool LabelPrinterService::loadConfig(const filesystem::path& path) {
  configPath_ = path;
  ifstream input(configPath_);
  if (!input) {
    configuredPrinter_.clear();
    return false;
  }

  string value;
  if (!(input >> quoted(value))) {
    configuredPrinter_.clear();
    return false;
  }

  configuredPrinter_ = trim(value);
  return !configuredPrinter_.empty();
}

bool LabelPrinterService::saveConfig(const filesystem::path& path) const {
  filesystem::create_directories(path.parent_path());
  ofstream output(path, ios::trunc);
  if (!output) {
    return false;
  }

  output << quoted(configuredPrinter_) << '\n';
  return true;
}

void LabelPrinterService::setConfiguredPrinter(string printerName) {
  configuredPrinter_ = trim(printerName);
}

const string& LabelPrinterService::configuredPrinter() const {
  return configuredPrinter_;
}

bool LabelPrinterService::hasConfiguredPrinter() const {
  return !trim(configuredPrinter_).empty();
}

optional<PrinterQueueInfo> LabelPrinterService::configuredPrinterInfo() const {
  if (!hasConfiguredPrinter()) {
    return nullopt;
  }

  const auto printers = enumeratePrinters();
  const auto needle = lowerAscii(trim(configuredPrinter_));
  for (const auto& printer : printers) {
    if (lowerAscii(trim(printer.name)) == needle) {
      return printer;
    }
  }
  return nullopt;
}

PrinterCheckResult LabelPrinterService::probeConfiguredPrinter() const {
  if (backend_ == nullptr) {
    return {false, "Printer backend unavailable"};
  }
  return backend_->probePrinter(configuredPrinter_);
}

InventatoryLabelPlan LabelPrinterService::buildLabelPlan(const InventoryItem& item, string rackLocation) const {
  InventatoryLabelPlan plan;
  const auto parameterLines = parameterLinesForItem(item);
  plan.categoryHeader = partContextHeader(item);
  plan.mainValue = mainLabelValue(item);
  plan.packageLine = shortPackageLine(item);
  plan.manufacturerLine = manufacturerLine(item);
  if (!parameterLines.empty()) {
    plan.parameterLine1 = parameterLines[0];
  }
  if (parameterLines.size() > 1) {
    plan.parameterLine2 = parameterLines[1];
  }
  if (parameterLines.size() > 2) {
    plan.parameterLine3 = parameterLines[2];
  }
  plan.inventatoryId = trim(item.inventatoryId);
  plan.scannerHint = buildVisibleInventatoryId(item);
  if (trim(plan.scannerHint).empty()) {
    plan.scannerHint = shortCode(plan.inventatoryId, 16);
  }
  plan.barcodeHint = normalizeMachineCode(item.machineCode);
  plan.rackLocation = trim(rackLocation);

  return plan;
}

string LabelPrinterService::buildZpl(const InventoryItem& item, string rackLocation) const {
  const auto plan = buildLabelPlan(item, move(rackLocation));
  const auto categoryHeader = fitSingleLineLabel(plan.categoryHeader, 16);
  const auto mainValue = fitSingleLineLabel(plan.mainValue, 14);
  const auto packageLine = fitSingleLineLabel(plan.packageLine, 24);
  const auto manufacturerLine = fitSingleLineLabel(plan.manufacturerLine, 20);
  const auto parameterLine1 = fitSingleLineLabel(plan.parameterLine1, 24);
  const auto parameterLine2 = fitSingleLineLabel(plan.parameterLine2, 24);
  const auto parameterLine3 = fitSingleLineLabel(plan.parameterLine3, 24);
  const auto scannerHint = fitSingleLineLabel(plan.scannerHint, 22);
  const auto barcodeHint = fitSingleLineLabel(plan.barcodeHint, 14);
  const auto rackHint = fitSingleLineLabel(plan.rackLocation, 12);
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n";
  out << "^PW256\r\n";
  out << "^LL200\r\n";
  out << "^LH0,0\r\n";
  out << "^PR3\r\n";
  out << "^MD12\r\n";
  out << "\r\n";

  out << "^FX --- Header ---\r\n";
  out << "^FO5,0^GB180,24,24,B,6^FS\r\n";
  out << "^FO12,6^A0N,17,17^FR^FD" << sanitizeLabelText(categoryHeader) << "^FS\r\n";
  out << "^FO200,6^A0N,17,17^FR^FDInventatory^FS\r\n";
  out << "\r\n";

  out << "^FX --- Main value ---\r\n";
  out << "^FO10,33^A0N,34,31^FD" << sanitizeLabelText(mainValue) << "^FS\r\n";
  out << "\r\n";

  out << "^FX --- Package ---\r\n";
  if (!packageLine.empty()) {
    out << "^FO10,70^A0N,14,14^FD" << sanitizeLabelText(packageLine) << "^FS\r\n";
  }
  out << "\r\n";

  out << "^FX --- Thin divider ---\r\n";
  out << "^FO10,93^GB146,1,1^FS\r\n";
  out << "\r\n";

  out << "^FX --- Manufacturer / parameters ---\r\n";
  if (!manufacturerLine.empty()) {
    out << "^FO10,100^A0N,16,16^FD" << sanitizeLabelText(manufacturerLine) << "^FS\r\n";
  }
  if (!parameterLine1.empty()) {
    out << "^FO10,118^A0N,15,15^FD" << sanitizeLabelText(parameterLine1) << "^FS\r\n";
  }
  if (!parameterLine2.empty()) {
    out << "^FO10,136^A0N,15,15^FD" << sanitizeLabelText(parameterLine2) << "^FS\r\n";
  }
  if (!parameterLine3.empty()) {
    out << "^FO10,154^A0N,15,15^FD" << sanitizeLabelText(parameterLine3) << "^FS\r\n";
  }
  out << "\r\n";

  out << "^FX --- QR code ---\r\n";
  // Keep the QR symbol compact so it wraps less on curved labels.
  out << "^FO170,60^BQN,2,3^FDLA," << sanitizeLabelText(barcodeHint) << "^FS\r\n";
  out << "\r\n";

  out << "^FX --- Human readable Inventatory ID ---\r\n";
  out << "^FO56,173^A0N,10,10^FD" << sanitizeLabelText(scannerHint) << "^FS\r\n";

  if (!rackHint.empty()) {
    out << "^FX --- Inventatory rack location ---\r\n";
    out << "^FO10,173^A0N,18,18^FD" << sanitizeLabelText(rackHint) << "^FS\r\n";
  }

  out << "^XZ\r\n";
  return out.str();
}

bool LabelPrinterService::printItemLabel(const InventoryItem& item, string* error, string rackLocation) const {
  if (backend_ == nullptr) {
    if (error != nullptr) {
      *error = "Printer backend unavailable";
    }
    return false;
  }

  if (!hasConfiguredPrinter()) {
    if (error != nullptr) {
      *error = "No printer configured";
    }
    return false;
  }

  const auto zpl = buildZpl(item, move(rackLocation));
  return backend_->sendRawJob(configuredPrinter_, makeJobName(item), zpl, error);
}

string LabelPrinterService::buildWireLabelZpl(const string& text) const {
  const auto label = fitSingleLineLabel(text, 18);
  if (label.empty()) return {};
  const auto font = cableFlagFont(label);
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n^PW256\r\n^LL200\r\n^LH0,0\r\n^PR3\r\n^MD12\r\n";
  out << "^FX --- Cable flag: normal-facing half ---\r\n";
  out << "^FO10,12^A0N," << font.height << ',' << font.width << "^FB236,1,0,C^FD"
      << sanitizeLabelText(label) << "^FS\r\n";
  out << "^FO14,88^GB228,2,2^FS\r\n";
  out << "^FX --- Cable flag: lower half, matching orientation ---\r\n";
  out << "^FO10,112^A0N," << font.height << ',' << font.width << "^FB236,1,0,C^FD"
      << sanitizeLabelText(label) << "^FS\r\n";
  out << "^FO14,88^GB228,2,2^FS\r\n";
  out << "^XZ\r\n";
  return out.str();
}

bool LabelPrinterService::printWireLabel(const string& text, string* error) const {
  if (backend_ == nullptr) {
    if (error != nullptr) *error = "Printer backend unavailable";
    return false;
  }
  if (!hasConfiguredPrinter()) {
    if (error != nullptr) *error = "No printer configured";
    return false;
  }
  const auto zpl = buildWireLabelZpl(text);
  if (zpl.empty()) {
    if (error != nullptr) *error = "Wire label text is empty";
    return false;
  }
  return backend_->sendRawJob(configuredPrinter_, "Inventatory Wire Label", zpl, error);
}

InventatoryRackLabelPlan LabelPrinterService::buildRackLabelPlan(const InventatoryRack& rack) const {
  InventatoryRackLabelPlan plan;
  plan.categoryText = rackDisplayCategory(rack.componentType);
  plan.rackText = rackLabelText(rack.code);
  return plan;
}

string LabelPrinterService::buildRackLabelZpl(const InventatoryRack& rack) const {
  const auto plan = buildRackLabelPlan(rack);
  const auto categoryLines = rackCategoryLines(plan.categoryText);
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n";
  out << "^PW256\r\n";
  out << "^LL200\r\n";
  out << "^LH0,0\r\n";
  out << "^PR3\r\n";
  out << "^MD12\r\n";
  out << "\r\n";

  out << "^FX --- Black header bar ---\r\n";
  out << "^FO4,4^GB248,28,28,B,5^FS\r\n";
  out << "^FO12,11^A0N,16,16^FR^FDInventatory RACK^FS\r\n";
  out << "\r\n";

  out << "^FX --- Main category text ---\r\n";
  if (categoryLines.size() <= 1) {
    out << "^FO6,55^A0N,40,34^FB244,1,0,C^FD" << rackCategoryFieldData(categoryLines) << "^FS\r\n";
  } else {
    out << "^FO6,48^A0N,27,24^FB244,2,4,C^FD" << rackCategoryFieldData(categoryLines) << "^FS\r\n";
  }
  out << "\r\n";

  out << "^FX --- Thin separator under category ---\r\n";
  out << "^FO34,105^GB188,2,2^FS\r\n";
  out << "\r\n";

  out << "^FX --- Rack ID pill ---\r\n";
  out << "^FO43,128^GB170,42,42,B,5^FS\r\n";
  out << "^FO43,138^A0N,23,23^FR^FB170,1,0,C^FD" << sanitizeLabelText(plan.rackText) << "^FS\r\n";
  out << "\r\n";

  out << "^XZ\r\n";
  return out.str();
}

bool LabelPrinterService::printRackLabel(const InventatoryRack& rack, string* error) const {
  if (backend_ == nullptr) {
    if (error != nullptr) {
      *error = "Printer backend unavailable";
    }
    return false;
  }

  if (!hasConfiguredPrinter()) {
    if (error != nullptr) {
      *error = "No printer configured";
    }
    return false;
  }

  const auto zpl = buildRackLabelZpl(rack);
  return backend_->sendRawJob(configuredPrinter_, makeRackJobName(rack), zpl, error);
}

string LabelPrinterService::summaryText() const {
  if (!hasConfiguredPrinter()) {
    return "Printer: not configured";
  }

  const auto info = configuredPrinterInfo();
  if (!info) {
    return "Printer: " + configuredPrinter_ + " [missing]";
  }

  ostringstream out;
  out << "Printer: " << info->name << " [" << info->statusText << "]";
  return out.str();
}

string sanitizeLabelText(const string& value) {
  return sanitiseZplFragment(value);
}

}  // namespace inventatory
