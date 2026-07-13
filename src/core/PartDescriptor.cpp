// HIMS - Hardware Inventory Management System
// Shared short part description classifier.

#include "core/PartDescriptor.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace hims {

using namespace std;

namespace {

struct DescriptorContext {
  string category;
  string categoryKey;
  string text;
  string textKey;
  vector<string> tokens;
  vector<pair<string, string>> parameters;
};

struct CandidateScore {
  string label;
  int score = 0;
};

string normalizeDescriptorKey(string value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return normalized;
}

string displayCategoryText(string value) {
  value = trim(value);
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

bool containsKey(const string& haystackKey, const char* needle) {
  const auto needleKey = normalizeDescriptorKey(needle);
  return !needleKey.empty() && haystackKey.find(needleKey) != string::npos;
}

bool containsAnyKey(const string& haystackKey, initializer_list<const char*> needles) {
  for (const auto* needle : needles) {
    if (containsKey(haystackKey, needle)) {
      return true;
    }
  }
  return false;
}

vector<string> tokenizeDescriptorText(const string& value) {
  vector<string> tokens;
  string token;
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      token.push_back(static_cast<char>(tolower(ch)));
    } else if (!token.empty()) {
      tokens.push_back(token);
      token.clear();
    }
  }
  if (!token.empty()) {
    tokens.push_back(token);
  }
  return tokens;
}

bool sameOrContainsKey(const string& lhs, const char* rhs) {
  const auto lhsKey = normalizeDescriptorKey(lhs);
  const auto rhsKey = normalizeDescriptorKey(rhs);
  return !lhsKey.empty() && !rhsKey.empty() &&
         (lhsKey == rhsKey || (rhsKey.size() >= 6 && lhsKey.find(rhsKey) != string::npos));
}

DescriptorContext makeContext(const InventoryItem& item) {
  DescriptorContext context;
  context.category = displayCategoryText(item.category);
  context.categoryKey = normalizeDescriptorKey(context.category);

  ostringstream text;
  text << item.category << ' ' << context.category << ' ' << item.partName << ' ' << item.manufacturer << ' '
       << item.location << ' ' << item.notes << ' ' << item.digikeyPartNumber << ' ' << item.sku;
  for (const auto& tag : item.tags) {
    text << ' ' << tag;
  }
  for (const auto& parameter : item.parameters) {
    const auto name = trim(parameter.name);
    const auto value = trim(parameter.value);
    context.parameters.push_back({name, value});
    text << ' ' << name << ' ' << value;
  }

  context.text = toLower(text.str());
  context.textKey = normalizeDescriptorKey(context.text);
  context.tokens = tokenizeDescriptorText(context.text);
  return context;
}

bool categoryHas(const DescriptorContext& context, initializer_list<const char*> needles) {
  return containsAnyKey(context.categoryKey, needles);
}

bool textHas(const DescriptorContext& context, initializer_list<const char*> needles) {
  return containsAnyKey(context.textKey, needles);
}

bool textHasToken(const DescriptorContext& context, initializer_list<const char*> needles) {
  for (const auto* needle : needles) {
    const auto needleKey = normalizeDescriptorKey(needle);
    if (needleKey.empty()) {
      continue;
    }
    if (find(context.tokens.begin(), context.tokens.end(), needleKey) != context.tokens.end()) {
      return true;
    }
  }
  return false;
}

bool hasParameterNamed(const DescriptorContext& context, initializer_list<const char*> names) {
  for (const auto& [name, value] : context.parameters) {
    (void)value;
    for (const auto* candidate : names) {
      if (sameOrContainsKey(name, candidate)) {
        return true;
      }
    }
  }
  return false;
}

bool parameterValueHas(const DescriptorContext& context, initializer_list<const char*> names,
                       initializer_list<const char*> values) {
  for (const auto& [name, value] : context.parameters) {
    bool nameMatched = false;
    for (const auto* candidate : names) {
      if (sameOrContainsKey(name, candidate)) {
        nameMatched = true;
        break;
      }
    }
    if (nameMatched && containsAnyKey(normalizeDescriptorKey(value), values)) {
      return true;
    }
  }
  return false;
}

bool anyParameterValueHas(const DescriptorContext& context, initializer_list<const char*> values) {
  for (const auto& [name, value] : context.parameters) {
    (void)name;
    if (containsAnyKey(normalizeDescriptorKey(value), values)) {
      return true;
    }
  }
  return false;
}

void addCandidate(vector<CandidateScore>& candidates, string label, int score) {
  if (score <= 0 || label.empty()) {
    return;
  }
  candidates.push_back({move(label), score});
}

int passiveScore(const DescriptorContext& context, initializer_list<const char*> categories,
                 initializer_list<const char*> parameters, initializer_list<const char*> textNeedles) {
  int score = 0;
  if (categoryHas(context, categories)) {
    score += 120;
  }
  if (hasParameterNamed(context, parameters)) {
    score += 100;
  }
  if (textHas(context, textNeedles)) {
    score += 45;
  }
  return score;
}

int diodeScore(const DescriptorContext& context, initializer_list<const char*> textNeedles) {
  int score = categoryHas(context, {"diode", "rectifier", "schottky", "transient voltage suppressor"}) ? 120 : 0;
  if (textHas(context, textNeedles)) {
    score += 90;
  }
  if (hasParameterNamed(context, {"Forward Voltage", "Reverse Voltage", "Voltage - Forward", "Voltage - DC Reverse"})) {
    score += 60;
  }
  return score;
}

int icBaseScore(const DescriptorContext& context) {
  int score = categoryHas(context, {"integrated circuit", "integrated circuits", "power management", "pmic",
                                    "logic", "amplifier", "comparator", "reference", "memory", "driver",
                                    "interface", "data acquisition", "clock", "timing", "timer", "converter",
                                    "regulator", "microcontroller", "mcu"})
                  ? 80
                  : 0;
  if (textHas(context, {"integrated circuit", "power management ic", "pmic"})) {
    score += 50;
  }
  return score;
}

vector<CandidateScore> scoreCandidates(const DescriptorContext& context) {
  vector<CandidateScore> candidates;
  const int icBase = icBaseScore(context);
  const bool timerEvidence = categoryHas(context, {"timer", "timers", "programmable timers"}) ||
                             parameterValueHas(context, {"Type", "Function"}, {"555", "timer", "one shot",
                                                                                "one-shot", "monostable", "astable",
                                                                                "multivibrator"}) ||
                             textHas(context, {"555 type", "timer oscillator", "timer/oscillator",
                                               "programmable timer", "delay timer", "timer ic", "monostable",
                                               "one shot", "one-shot", "sngl timer", "single timer"}) ||
                             textHasToken(context, {"timer", "timers", "ne555", "lm555", "tlc555", "counter",
                                                    "counters"});

  addCandidate(candidates, "Capacitor",
               passiveScore(context, {"capacitor"}, {"Capacitance"}, {"capacitor", "capacitance"}));
  addCandidate(candidates, "Resistor",
               passiveScore(context, {"resistor"}, {"Resistance"}, {"resistor", "resistance", "ohm"}));
  addCandidate(candidates, "LED",
               passiveScore(context, {"indicator", "led"}, {"Color", "Forward Voltage"}, {"led", "light emitting"}));
  addCandidate(candidates, "Connector",
               passiveScore(context, {"connector"}, {"Connector Type", "Number of Positions", "Pins"},
                            {"connector", "header", "receptacle"}));
  addCandidate(candidates, "Inductor",
               passiveScore(context, {"inductor", "choke", "coil"}, {"Inductance"},
                            {"inductor", "fixed ind", "choke", "coil"}));
  addCandidate(candidates, "Crystal",
               passiveScore(context, {"crystal", "oscillator", "resonator"}, {"Frequency", "Load Capacitance"},
                            {"crystal", "resonator"}));

  addCandidate(candidates, "TVS Diode", diodeScore(context, {"tvs", "transient voltage suppressor", "esd protection"}));
  addCandidate(candidates, "Schottky Diode", diodeScore(context, {"schottky"}));
  addCandidate(candidates, "Rectifier Diode", diodeScore(context, {"rectifier"}));
  addCandidate(candidates, "Zener Diode", diodeScore(context, {"zener"}));
  addCandidate(candidates, "Diode", diodeScore(context, {"diode"}));

  addCandidate(candidates, "MOSFET",
               passiveScore(context, {"mosfet", "fet"}, {"Drain-Source Voltage", "Rds On"},
                            {"mosfet", "power mosfet"}));
  addCandidate(candidates, "Transistor",
               passiveScore(context, {"transistor", "discrete semiconductor"}, {"Collector Current"},
                            {"transistor", "bjt", "npn", "pnp"}));

  addCandidate(candidates, "MCU",
               max(passiveScore(context, {"mcu", "microcontroller"}, {"Core Processor", "Program Memory Size"},
                                {"microcontroller", "microprocessor", "processor", "mcu"}),
                   icBase + (textHas(context, {"microcontroller", "microprocessor", "mcu"}) ? 90 : 0)));

  addCandidate(candidates, "Buck-Boost Conv.",
               icBase + (parameterValueHas(context, {"Topology", "Function", "Type"}, {"buck boost", "buck-boost"}) ? 170 : 0) +
                   (textHas(context, {"buck boost", "buck-boost"}) ? 120 : 0));
  const bool buckBoostEvidence = parameterValueHas(context, {"Topology", "Function", "Type"},
                                                   {"buck boost", "buck-boost"}) ||
                                 textHas(context, {"buck boost", "buck-boost"});
  addCandidate(candidates, "Buck Converter",
               icBase + (parameterValueHas(context, {"Topology"}, {"buck"}) ? 150 : 0) +
                   (parameterValueHas(context, {"Function", "Type"}, {"dc dc", "dcdc", "switching regulator"}) ? 65 : 0) +
                   (textHas(context, {"buck converter", "buck regulator", "step down", "step-down", "synchronous buck"})
                        ? 120
                        : 0) -
                   (buckBoostEvidence ? 140 : 0));
  addCandidate(candidates, "Boost Converter",
               icBase + (parameterValueHas(context, {"Topology"}, {"boost"}) ? 150 : 0) +
                   (parameterValueHas(context, {"Function", "Type"}, {"dc dc", "dcdc", "switching regulator"}) ? 65 : 0) +
                   (textHas(context, {"boost converter", "boost regulator", "step up", "step-up"}) ? 120 : 0) -
                   (buckBoostEvidence ? 140 : 0));
  addCandidate(candidates, "Battery Charger",
               icBase + (textHas(context, {"battery charger", "charger"}) ? 130 : 0) +
                   (parameterValueHas(context, {"Function", "Type"}, {"charger"}) ? 130 : 0));
  addCandidate(candidates, "Load Switch",
               icBase + (parameterValueHas(context, {"Function", "Type"}, {"load switch", "hot swap", "efuse",
                                                                            "e-fuse"})
                             ? 145
                             : 0) +
                   (textHas(context, {"load switch", "power distribution switch", "hot swap", "efuse", "e-fuse"})
                        ? 130
                        : 0));
  addCandidate(candidates, "Voltage Supervisor",
               icBase + (parameterValueHas(context, {"Function", "Type"}, {"voltage supervisor", "supervisor",
                                                                            "reset ic", "watchdog",
                                                                            "voltage detector"})
                             ? 145
                             : 0) +
                   (textHas(context, {"voltage supervisor", "reset ic", "power-on reset", "watchdog",
                                      "voltage detector", "brownout"})
                             ? 130
                             : 0));
  addCandidate(candidates, "Protection IC",
               icBase + (parameterValueHas(context, {"Function", "Type"}, {"protection", "esd"}) ? 130 : 0) +
                   (textHas(context, {"protection ic", "protection array", "esd protection", "surge protection",
                                      "overvoltage protection"})
                        ? 120
                        : 0));
  addCandidate(candidates, "Regulator",
               icBase + (categoryHas(context, {"regulator", "voltage regulator"}) ? 100 : 0) +
                   (textHas(context, {"low dropout", "ldo", "linear regulator", "voltage regulator"}) ? 120 : 0));
  addCandidate(candidates, "Power IC",
               icBase + (categoryHas(context, {"power management", "pmic"}) ? 90 : 0) +
                   (textHas(context, {"power management", "pmic", "switching regulator", "dc dc", "dcdc"}) ? 80 : 0));

  addCandidate(candidates, "OP-AMP",
               icBase + (textHas(context, {"op amp", "op-amp", "operational amplifier", "gain bandwidth",
                                           "slew rate", "rail to rail", "rail-to-rail"})
                             ? 145
                             : 0));
  addCandidate(candidates, "Comparator",
               icBase + (parameterValueHas(context, {"Function", "Type"}, {"comparator"}) ? 145 : 0) +
                   (textHas(context, {"comparator", "window comparator"}) ? 130 : 0));
  addCandidate(candidates, "Voltage Ref.",
               icBase + (hasParameterNamed(context, {"Voltage Reference Type"}) ? 150 : 0) +
                   (textHas(context, {"voltage reference", "reference voltage", "bandgap reference", "shunt reference"})
                        ? 130
                        : 0) -
                   (timerEvidence ? 220 : 0));
  addCandidate(candidates, "Amplifier",
               icBase + (textHas(context, {"instrumentation amplifier", "current sense amplifier", "amplifier"}) ? 95 : 0));

  addCandidate(candidates, "ADC",
               icBase + (textHas(context, {"analog to digital", "analog-to-digital", "adc"}) ? 135 : 0));
  addCandidate(candidates, "DAC",
               icBase + (textHas(context, {"digital to analog", "digital-to-analog", "dac"}) ? 135 : 0));
  addCandidate(candidates, "Data Conv.",
               icBase + (textHas(context, {"data converter", "data acquisition"}) ? 100 : 0));

  addCandidate(candidates, "RTC", icBase + (textHas(context, {"rtc", "real time clock", "real-time clock"}) ? 145 : 0));
  addCandidate(candidates, "Clock Gen.",
               icBase + (textHas(context, {"clock generator", "clock synthesizer", "frequency synthesizer", "pll"}) ? 135 : 0));
  addCandidate(candidates, "Timer IC",
               icBase + (categoryHas(context, {"timer", "timers", "programmable timers"}) ? 130 : 0) +
                   (parameterValueHas(context, {"Type", "Function"}, {"555", "timer", "one shot", "one-shot",
                                                                        "monostable", "astable", "multivibrator"})
                        ? 170
                        : 0) +
                   (textHas(context, {"555 type", "timer oscillator", "timer/oscillator", "programmable timer",
                                      "delay timer", "timer ic", "monostable", "one shot", "one-shot",
                                      "sngl timer", "single timer"})
                        ? 140
                        : 0) +
                   (textHasToken(context, {"timer", "timers", "ne555", "lm555", "tlc555"}) ? 160 : 0));
  addCandidate(candidates, "Oscillator",
               icBase + (textHas(context, {"oscillator", "osc"}) ? 70 : 0) - (timerEvidence ? 90 : 0));

  const bool hBridgeEvidence = textHas(context, {"h bridge", "h-bridge"}) ||
                               parameterValueHas(context, {"Function", "Output Type", "Type"}, {"h bridge", "h-bridge"});
  addCandidate(candidates, "H-Bridge", icBase + (hBridgeEvidence ? 170 : 0));
  addCandidate(candidates, "Motor Driver",
               icBase + (parameterValueHas(context, {"Function", "Output Type"}, {"motor driver", "motor control"}) ? 140 : 0) +
                   (textHas(context, {"motor driver", "stepper driver", "servo driver"}) ? 130 : 0) -
                   (hBridgeEvidence ? 120 : 0));
  addCandidate(candidates, "Gate Driver", icBase + (textHas(context, {"gate driver", "mosfet driver"}) ? 130 : 0));
  addCandidate(candidates, "Driver IC", icBase + (textHas(context, {"led driver", "display driver", "line driver"}) ? 95 : 0));

  addCandidate(candidates, "Transceiver", icBase + (textHas(context, {"transceiver", "bus transceiver"}) ? 130 : 0));
  addCandidate(candidates, "Level Shifter",
               icBase + (textHas(context, {"level shifter", "logic translator", "voltage translator"}) ? 130 : 0));
  addCandidate(candidates, "Mux/Demux",
               icBase + (textHas(context, {"multiplexer", "demultiplexer", "mux", "demux"}) ? 130 : 0));
  addCandidate(candidates, "Flip-Flop", icBase + (textHas(context, {"flip flop", "flip-flop"}) ? 130 : 0));
  addCandidate(candidates, "Latch", icBase + (textHas(context, {"latch"}) ? 120 : 0));
  addCandidate(candidates, "Inverter", icBase + (textHas(context, {"inverter", "not gate"}) ? 120 : 0));
  addCandidate(candidates, "Logic Gate",
               icBase + (parameterValueHas(context, {"Function", "Type"}, {"logic gate"}) ? 135 : 0) +
                   (textHas(context, {"logic gate", "and gate", "or gate", "nand", "nor", "xor", "xnor"}) ? 125 : 0));
  addCandidate(candidates, "Logic Buffer",
               icBase + (parameterValueHas(context, {"Function", "Type"}, {"logic buffer", "buffer"}) ? 135 : 0) +
                   (textHas(context, {"logic buffer", "bus buffer", "tri state buffer", "tri-state buffer"}) ? 125 : 0));
  addCandidate(candidates, "Interface IC",
               icBase + (textHas(context, {"interface", "phy", "usb", "can", "rs485", "rs-485", "ethernet",
                                           "spi", "i2c", "uart"})
                             ? 80
                             : 0));

  addCandidate(candidates, "Memory IC",
               icBase + (hasParameterNamed(context, {"Memory Type", "Memory Format", "Memory Size",
                                                     "Program Memory Size", "Memory Interface"})
                             ? 150
                             : 0) +
                   (categoryHas(context, {"memory"}) ? 85 : 0) +
                   (textHas(context, {"flash memory", "eeprom", "sram", "dram", "fram", "non volatile memory",
                                      "non-volatile memory"})
                        ? 110
                        : 0));

  addCandidate(candidates, "3 Axis IMU",
               passiveScore(context, {"sensor"}, {"Sensor Type", "Type"},
                            {"3 axis", "3-axis", "imu", "inertial measurement unit", "accelerometer gyroscope"}) +
                   (parameterValueHas(context, {"Sensor Type", "Type"}, {"3 axis", "3-axis", "imu",
                                                                          "accelerometer gyroscope"})
                        ? 120
                        : 0));
  addCandidate(candidates, "Temp Sensor",
               (parameterValueHas(context, {"Sensor Type", "Type"}, {"temperature", "thermocouple"}) ? 180 : 0) +
                   (categoryHas(context, {"temperature sensor"}) ? 150 : 0) +
                   (categoryHas(context, {"sensor"}) && textHas(context, {"temperature sensor", "temp sensor",
                                                                          "thermometer", "thermocouple"})
                        ? 130
                        : 0) -
                   (timerEvidence ? 160 : 0));
  addCandidate(candidates, "Pressure Sensor",
               (parameterValueHas(context, {"Sensor Type", "Type"}, {"pressure"}) ? 160 : 0) +
                   (categoryHas(context, {"sensor"}) && textHas(context, {"pressure"}) ? 120 : 0));
  addCandidate(candidates, "Humidity Sensor",
               (parameterValueHas(context, {"Sensor Type", "Type"}, {"humidity"}) ? 160 : 0) +
                   (categoryHas(context, {"sensor"}) && textHas(context, {"humidity"}) ? 120 : 0));
  addCandidate(candidates, "Current Sensor",
               (parameterValueHas(context, {"Sensor Type", "Type", "Function"}, {"current sensor", "current monitor"})
                    ? 160
                    : 0) +
                   (categoryHas(context, {"sensor"}) && textHas(context, {"current sensor", "current monitor"}) ? 120 : 0));
  addCandidate(candidates, "Accelerometer",
               (parameterValueHas(context, {"Sensor Type", "Type"}, {"accelerometer"}) ? 160 : 0) +
                   (categoryHas(context, {"sensor"}) && textHas(context, {"accelerometer", "accel"}) ? 120 : 0));
  addCandidate(candidates, "Gyroscope",
               (parameterValueHas(context, {"Sensor Type", "Type"}, {"gyroscope", "gyro"}) ? 160 : 0) +
                   (categoryHas(context, {"sensor"}) && textHas(context, {"gyroscope", "gyro"}) ? 120 : 0));
  addCandidate(candidates, "Magnetometer",
               (parameterValueHas(context, {"Sensor Type", "Type"}, {"magnetometer"}) ? 160 : 0) +
                   (categoryHas(context, {"sensor"}) && textHas(context, {"magnetometer"}) ? 120 : 0));
  addCandidate(candidates, "Hall Sensor",
               (categoryHas(context, {"sensor"}) && textHas(context, {"hall effect", "hall sensor"}) ? 130 : 0));
  addCandidate(candidates, "Proximity Sensor",
               (categoryHas(context, {"sensor"}) && textHas(context, {"proximity", "distance", "time of flight",
                                                                       "tof", "gesture"})
                    ? 125
                    : 0));
  addCandidate(candidates, "Light Sensor",
               (categoryHas(context, {"sensor"}) && textHas(context, {"ambient light", "light sensor", "color sensor",
                                                                       "optical"})
                    ? 125
                    : 0));
  addCandidate(candidates, "Gas Sensor",
               (categoryHas(context, {"sensor"}) && textHas(context, {"gas sensor", "air quality", "voc", "co2"})
                    ? 125
                    : 0));
  addCandidate(candidates, "Touch Sensor",
               (categoryHas(context, {"sensor"}) && textHas(context, {"touch sensor", "capacitive touch"}) ? 125 : 0));
  addCandidate(candidates, "Sensor IC", icBase + (categoryHas(context, {"sensor"}) ? 80 : 0));
  addCandidate(candidates, "Sensor", categoryHas(context, {"sensor"}) ? 95 : 0);

  addCandidate(candidates, "Integrated Circuit", icBase + (categoryHas(context, {"integrated circuit"}) ? 70 : 0));
  addCandidate(candidates, "Logic IC", icBase + (categoryHas(context, {"logic"}) ? 85 : 0));
  addCandidate(candidates, "Analog IC", icBase + (categoryHas(context, {"amplifier", "comparator", "reference"}) ? 85 : 0));
  addCandidate(candidates, "Timing IC", icBase + (categoryHas(context, {"timing", "clock", "oscillator", "rtc"}) ? 85 : 0));

  return candidates;
}

string fallbackDescription(const DescriptorContext& context, const InventoryItem& item) {
  if (!trim(context.category).empty()) {
    return trim(context.category);
  }
  if (!trim(item.partName).empty()) {
    return trim(item.partName);
  }
  if (!trim(item.sku).empty()) {
    return trim(item.sku);
  }
  return "Part";
}

}  // namespace

PartDescriptor describePart(const InventoryItem& item) {
  const auto context = makeContext(item);
  const auto candidates = scoreCandidates(context);

  CandidateScore best;
  best.score = numeric_limits<int>::min();
  for (const auto& candidate : candidates) {
    if (candidate.score > best.score) {
      best = candidate;
    }
  }

  PartDescriptor descriptor;
  descriptor.shortDescription = best.score >= 100 ? best.label : fallbackDescription(context, item);
  return descriptor;
}

string partShortDescription(const InventoryItem& item) {
  return describePart(item).shortDescription;
}

}  // namespace hims
