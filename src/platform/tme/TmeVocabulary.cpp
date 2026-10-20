// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "platform/tme/TmeVocabulary.h"

#include <cctype>
#include <utility>
#include <vector>

namespace inventatory {
using namespace std;

namespace {

// Lower-case letters and digits only, so "Max. off-state voltage" and "max off state voltage" are one key.
string vocabularyKey(const string& text) {
  string key;
  for (const unsigned char character : text) {
    if (isalnum(character)) key += static_cast<char>(tolower(character));
  }
  return key;
}

// Lower-case words of the category name, split at punctuation.
vector<string> categoryWords(const string& categoryName) {
  vector<string> words;
  string current;
  for (const unsigned char character : categoryName) {
    if (isalnum(character)) {
      current += static_cast<char>(tolower(character));
    } else if (!current.empty()) {
      words.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) words.push_back(current);
  return words;
}

// A keyword of four letters or fewer must match a whole word or its plural: "led" is not in
// "handled". Longer keywords match inside a word, so "resistor" matches "microswitches".
bool wordMatches(const string& word, const string& keyword) {
  if (keyword.size() <= 4) return word == keyword || word == keyword + "s";
  return word.find(keyword) != string::npos;
}

struct FamilyRule {
  const char* family;
  vector<const char*> keywords;
};

// The first matching rule wins. Protection parts come first, so a TVS diode is not a diode.
const vector<FamilyRule>& familyRules() {
  static const vector<FamilyRule> rules = {
      {"Circuit Protection", {"tvs", "fuse", "varistor"}},
      {"Potentiometers, Variable Resistors", {"trimmer", "potentiometer", "rheostat"}},
      {"Resistors", {"resistor"}},
      {"Capacitors", {"capacitor"}},
      {"Inductors, Coils, Chokes", {"inductor", "coil", "choke"}},
      {"Filters", {"ferrite", "filter"}},
      {"Crystals, Oscillators, Resonators", {"crystal", "resonator", "oscillator", "quartz", "generator"}},
      {"Connectors, Interconnects", {"connector", "header"}},
      {"Relays", {"relay"}},
      {"Switches", {"switch", "tact"}},
      {"Sensors, Transducers", {"sensor", "transducer"}},
      {"Optoelectronics", {"led", "leds", "optocoupler", "photo"}},
      {"Discrete Semiconductor Products",
       {"transistor", "mosfet", "bjt", "jfet", "diode", "thyristor", "triac", "diac", "scr"}},
      {"Integrated Circuits (ICs)",
       {"integ", "microcontroller", "microprocessor", "regulator", "ldo", "amplifier", "operational", "comparator",
        "reference", "memor", "eeprom", "flash", "interface", "driver", "logic", "gate", "inverter", "register",
        "watchdog", "reset", "converter", "timer", "multiplexer", "transceiver", "avr", "supervisor"}},
  };
  return rules;
}

// TME parameter keys (as vocabularyKey normalises them) and the name the app reads them under.
const vector<pair<string, string>>& parameterNames() {
  static const vector<pair<string, string>> names = {
      {"mounting", "Mounting Type"},
      {"kindofpackage", "Packaging"},
      {"manufacturerseries", "Series"},
      {"operatingvoltage", "Operating Voltage"},
      {"operatingtemperature", "Operating Temperature"},
      {"typeofresistor", "Composition"},
      {"supplyvoltage", "Voltage - Supply"},
      {"voltagesupplyrange", "Voltage - Supply"},
      {"inputvoltage", "Voltage - Input"},
      {"outputvoltage", "Output Voltage"},
      {"outputcurrent", "Output Current"},
      {"loadcurrent", "Current - Average Rectified (Io)"},
      {"maximumoutputcurrent", "Current - Output"},
      {"typeofintegratedcircuit", "Function"},
      {"kindofintegratedcircuit", "Type"},
      {"luminosity", "Luminous Intensity"},
      {"ledcurrent", "Current - Test"},
      // A reel quantity, not a package. Its name contains "package", which the label reads as one.
      {"manufacturerstandardpackage", "Standard Quantity"},
      {"quantityinsetpackage", "Standard Quantity"},
      {"operatingcurrent", "Current Rating (Amps)"},
      {"dcsupplycurrent", "Current - Supply"},
      {"breakdownvoltage", "Voltage - Breakdown (Min)"},
      {"zenervoltage", "Voltage - Zener (Nom) (Vz)"},
      {"maxforwardvoltage", "Forward Voltage"},
      {"maxforwardimpulsecurrent", "Current - Non Rep. Surge 50, 60Hz (Ifsm)"},
      {"peakpulsepowerdissipation", "Peak Pulse Power"},
      {"powerdissipation", "Power Dissipation"},
      {"typeofdiode", "Diode Type"},
      {"drainsourcevoltage", "Drain to Source Voltage (Vdss)"},
      {"draincurrent", "Continuous Drain Current"},
      {"onstateresistance", "Rds On"},
      {"gatesourcevoltage", "Vgs (Max)"},
      {"gatecharge", "Gate Charge"},
      {"kindofchannel", "Channel Type"},
      {"bodydimensions", "Size / Dimension"},
      {"height", "Height - Seated (Max)"},
      {"leads", "Lead Style"},
      {"terminalpitch", "Pitch - Mating"},
      {"contactspitch", "Pitch - Mating"},
      {"numberofpins", "Number of Positions"},
      {"typeofconnector", "Connector Type"},
      {"ledcolour", "Color"},
      {"resonantfrequency", "Frequency - Self Resonant"},
      {"resonatortolerance", "Frequency Tolerance"},
      {"typeofsensor", "Sensor Type"},
      {"converterresolution", "Resolution"},
      {"contactsconfiguration", "Circuit"},
      {"dccontactsratingr", "Contact Rating @ Voltage"},
      {"kindofinterface", "Interface"},
      {"kindofmemory", "Memory Type"},
      {"kindofcore", "Core Size"},
      {"kindofarchitecture", "Core Processor"},
      {"kindofvoltageregulator", "Output Type"},
      {"temperaturecoefficient", "Temperature Coefficient"},
  };
  return names;
}

}  // namespace

string tmeCategoryFamily(const string& categoryName) {
  const auto words = categoryWords(categoryName);
  for (const auto& rule : familyRules()) {
    for (const auto* keyword : rule.keywords) {
      for (const auto& word : words) {
        if (wordMatches(word, keyword)) return rule.family;
      }
    }
  }
  return {};
}

string tmeParameterValue(const string& value) {
  string text = value;
  // "from...to" is a range; DigiKey writes it with the word "to".
  for (size_t at = text.find("..."); at != string::npos; at = text.find("...", at + 4)) {
    text.replace(at, 3, " to ");
  }
  while (text.find("  ") != string::npos) text.replace(text.find("  "), 2, " ");
  // The "DC" after a voltage or current is a qualifier TME adds; DigiKey does not print it. "AC/DC" keeps it.
  string trimmed = text;
  while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
  if (trimmed.size() > 2 && trimmed.compare(trimmed.size() - 2, 2, "DC") == 0) {
    auto before = trimmed.substr(0, trimmed.size() - 2);
    while (!before.empty() && before.back() == ' ') before.pop_back();
    if (!before.empty() && isalnum(static_cast<unsigned char>(before.back()))) text = before;
  }
  while (!text.empty() && text.front() == ' ') text.erase(text.begin());
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

vector<Parameter> tmeParameters(const string& name, const string& value, const string& family) {
  const auto key = vocabularyKey(name);
  const auto text = tmeParameterValue(value);
  string lowered = text;
  for (auto& character : lowered) character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
  if (key == "typeoftransistor") {
    // The value names the device: a MOSFET's channel, or a bipolar transistor's polarity.
    if (lowered.find("mosfet") != string::npos) {
      return {{"FET Type", lowered.rfind("p-", 0) == 0 ? "P-Channel" : "N-Channel"}};
    }
    if (lowered == "npn" || lowered == "pnp") {
      return {{"Transistor Type", text}};
    }
  }
  if (key == "memory") {
    // Gives each memory its own figure, as DigiKey does. Other values are kept as sent.
    vector<Parameter> parameters;
    size_t start = 0;
    while (start < text.size()) {
      auto end = text.find(',', start);
      if (end == string::npos) end = text.size();
      auto entry = text.substr(start, end - start);
      entry.erase(0, entry.find_first_not_of(' '));
      entry.erase(entry.find_last_not_of(' ') + 1);
      const auto space = entry.find(' ');
      if (space != string::npos) {
        string kind = entry.substr(space + 1);
        for (auto& character : kind) character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
        const auto size = entry.substr(0, space);
        if (kind == "flash") parameters.push_back({"Program Memory Size", size});
        if (kind == "sram") parameters.push_back({"RAM Size", size});
        if (kind == "eeprom") parameters.push_back({"EEPROM Size", size});
      }
      start = end + 1;
    }
    if (!parameters.empty()) return parameters;
  }
  if (key == "ledlens") {
    // DigiKey calls a clear lens "Clear".
    return {{"Lens Transparency", lowered == "transparent" ? "Clear" : text}};
  }
  if (key == "typeofdiode" || key == "diodetype") {
    // "LED" repeats what the label already shows, so it is dropped.
    if (lowered == "led") return {};
    vector<Parameter> parameters = {{"Diode Type", text}};
    if (lowered.find("schottky") != string::npos) {
      parameters.push_back({"Technology", "Schottky"});
    } else if (lowered == "universal" || lowered == "switching" || lowered == "rectifying") {
      parameters.push_back({"Technology", "Standard"});
    }
    return parameters;
  }
  return {{tmeParameterName(name, family), text}};
}

string tmeParameterName(const string& name, const string& family) {
  const auto key = vocabularyKey(name);
  // A protection part's off-state maximum is its reverse standoff voltage; a rectifier's is Vr.
  if (key == "maxoffstatevoltage") {
    return family == "Circuit Protection" ? "Voltage - Reverse Standoff (Typ)" : "Voltage - DC Reverse (Vr) (Max)";
  }
  // An LED's operating voltage is its forward voltage, not a reverse voltage.
  if (key == "operatingvoltage" && family == "Optoelectronics") return "Voltage - Forward (Vf) (Typ)";
  // A crystal's or oscillator's capacitance is the load capacitance its frequency is specified with.
  if (key == "capacitance" && family == "Crystals, Oscillators, Resonators") return "Load Capacitance";
  for (const auto& [tmeKey, appName] : parameterNames()) {
    if (tmeKey == key) return appName;
  }
  return name;
}

}  // namespace inventatory
