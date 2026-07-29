// Inventatory - Hardware Inventory Management System
// Deterministic, provider-backed part label resolution.

#include "core/PartDescriptor.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>
#include <vector>

namespace inventatory {

using namespace std;

namespace {

struct LabelContext {
  string provider;
  vector<string> categories;
  string title;
  vector<Parameter> parameters;
};

string normalizedKey(const string& value) {
  string key;
  key.reserve(value.size());
  for (const auto ch : value) {
    if (isalnum(static_cast<unsigned char>(ch))) {
      key.push_back(static_cast<char>(tolower(static_cast<unsigned char>(ch))));
    }
  }
  return key;
}

vector<string> splitCategoryPath(const string& value) {
  vector<string> categories;
  size_t start = 0;
  while (start < value.size()) {
    const auto slash = value.find(" / ", start);
    const auto dash = value.find(" - ", start);
    const auto separator = slash == string::npos ? dash : (dash == string::npos ? slash : min(slash, dash));
    const auto segment = trim(value.substr(start, separator == string::npos ? string::npos : separator - start));
    if (!segment.empty()) {
      categories.push_back(segment);
    }
    if (separator == string::npos) {
      break;
    }
    start = separator + 3;
  }
  return categories;
}

void appendCategoryPath(vector<string>& target, const string& value) {
  const auto trimmed = trim(value);
  if (trimmed.empty()) {
    return;
  }
  target.push_back(trimmed);
  for (const auto& segment : splitCategoryPath(trimmed)) {
    if (segment != trimmed) {
      target.push_back(segment);
    }
  }
}

LabelContext makeContext(const InventoryItem& item) {
  LabelContext context;
  context.provider = toLower(trim(item.vendorMetadata.provider));
  for (const auto& category : item.vendorMetadata.categoryPath) {
    appendCategoryPath(context.categories, category);
  }
  if (!trim(item.vendorMetadata.categoryId).empty()) {
    appendCategoryPath(context.categories, item.vendorMetadata.categoryId);
  }
  if (context.categories.empty()) {
    context.categories = splitCategoryPath(item.category);
  }
  if (!trim(item.category).empty()) {
    appendCategoryPath(context.categories, item.category);
  }
  context.title = trim(item.vendorMetadata.title);
  if (context.title.empty()) {
    context.title = item.partName;
  }
  context.parameters = item.vendorMetadata.parameters.empty() ? item.parameters : item.vendorMetadata.parameters;
  return context;
}

bool hasExactCategory(const LabelContext& context, initializer_list<const char*> names) {
  for (const auto& category : context.categories) {
    const auto categoryKey = normalizedKey(category);
    for (const auto* name : names) {
      if (categoryKey == normalizedKey(name)) {
        return true;
      }
    }
  }
  return false;
}

bool hasCategoryPhrase(const LabelContext& context, initializer_list<const char*> phrases) {
  for (const auto& category : context.categories) {
    const auto categoryKey = normalizedKey(category);
    for (const auto* phrase : phrases) {
      const auto phraseKey = normalizedKey(phrase);
      if (!phraseKey.empty() && categoryKey.find(phraseKey) != string::npos) {
        return true;
      }
    }
  }
  return false;
}

bool hasParameter(const LabelContext& context, initializer_list<const char*> names,
                  initializer_list<const char*> valueFragments = {}) {
  for (const auto& parameter : context.parameters) {
    const auto parameterName = normalizedKey(parameter.name);
    bool nameMatched = false;
    for (const auto* name : names) {
      if (parameterName == normalizedKey(name)) {
        nameMatched = true;
        break;
      }
    }
    if (!nameMatched) {
      continue;
    }
    if (valueFragments.size() == 0) {
      return true;
    }
    const auto parameterValue = normalizedKey(parameter.value);
    for (const auto* fragment : valueFragments) {
      if (parameterValue.find(normalizedKey(fragment)) != string::npos) {
        return true;
      }
    }
  }
  return false;
}

bool guardedTitleHas(const LabelContext& context, initializer_list<const char*> fragments) {
  const auto titleKey = normalizedKey(context.title);
  for (const auto* fragment : fragments) {
    if (titleKey.find(normalizedKey(fragment)) != string::npos) {
      return true;
    }
  }
  return false;
}

string printAlias(string label) {
  // The printer scales label text to the available width. Keep the complete
  // deterministic description here instead of silently cutting it at 16
  // characters, which discarded meaningful subtype information.
  return trim(label);
}

PartDescriptor resolved(string purposeLabel, string printLabel, PartLabelSource source) {
  PartDescriptor descriptor;
  descriptor.purposeLabel = move(purposeLabel);
  descriptor.printLabel = printAlias(move(printLabel));
  descriptor.source = source;
  descriptor.usedFallback = source == PartLabelSource::Fallback;
  descriptor.shortDescription = descriptor.printLabel;
  return descriptor;
}

PartDescriptor discreteSemiconductorRule(const LabelContext& context) {
  // DigiKey places the useful discrete subtype in either its taxonomy leaf or
  // a structured "Type" parameter.  Resolve those before the broad family
  // mapping below: returning "Diode" for a Schottky or "Transistor" for an
  // NPN BJT throws away the information that distinguishes the stock item.
  // Older scanned items can predate persisted DigiKey taxonomy and therefore
  // retain only "Discrete Semiconductor Products". Their part name and
  // Technology parameter are still structured enough to identify the family.
  const bool isIntegratedCircuitFamily = hasCategoryPhrase(
      context, {"integrated circuits", "power management", "logic", "interface", "linear", "data acquisition", "clock timing"});
  const bool isLegacyDiscreteFamily = hasCategoryPhrase(context, {"discrete semiconductor"});
  const bool hasTvsEvidence = hasCategoryPhrase(context, {"tvs", "transient voltage suppressor"}) ||
                              hasParameter(context, {"Diode Type", "Type", "Technology"}, {"tvs", "transient voltage suppressor"}) ||
                              guardedTitleHas(context, {"tvs diode", "tvs"});
  const bool isDiode = hasCategoryPhrase(context, {"diodes", "rectifiers", "varactor"}) || hasTvsEvidence ||
                       (isLegacyDiscreteFamily && !isIntegratedCircuitFamily && guardedTitleHas(context, {"diode", "rectifier"}));
  const bool isTransistor = hasCategoryPhrase(context, {"transistors", "bipolar bjt"}) ||
                            (isLegacyDiscreteFamily && !isIntegratedCircuitFamily &&
                             guardedTitleHas(context, {"transistor", "trans npn", "trans pnp"})) ||
                            (isLegacyDiscreteFamily && hasParameter(context, {"Technology"}, {"bjt", "bipolar"}));
  const bool isMosfet = hasCategoryPhrase(context, {"mosfets", "mosfet", "fet arrays"}) ||
                        (isLegacyDiscreteFamily && !isIntegratedCircuitFamily && guardedTitleHas(context, {"mosfet"})) ||
                        (isLegacyDiscreteFamily && hasParameter(context, {"Technology"}, {"mosfet"}));
  const bool isJfet = hasCategoryPhrase(context, {"jfets", "jfet"}) ||
                      (isLegacyDiscreteFamily && !isIntegratedCircuitFamily && guardedTitleHas(context, {"jfet"}));
  const bool isThyristor = hasCategoryPhrase(context, {"thyristors", "scrs", "triacs", "diacs"}) ||
                           (isLegacyDiscreteFamily && !isIntegratedCircuitFamily && guardedTitleHas(context, {"thyristor", "triac", "diac"}));

  if (isDiode) {
    if (hasCategoryPhrase(context, {"transient voltage suppressors", "tvs", "esd protection"}) ||
        hasParameter(context, {"Diode Type", "Type", "Technology"}, {"tvs", "esd"}) ||
        guardedTitleHas(context, {"tvs", "esd"})) {
      return resolved("TVS Diode", "TVS Diode", PartLabelSource::VendorCategory);
    }
    if (hasCategoryPhrase(context, {"schottky"}) || hasParameter(context, {"Diode Type", "Type", "Technology"}, {"schottky"}) ||
        guardedTitleHas(context, {"schottky"})) {
      return resolved("Schottky Diode", "Schottky Diode", PartLabelSource::VendorCategory);
    }
    if (hasCategoryPhrase(context, {"zener"}) || hasParameter(context, {"Diode Type", "Type", "Technology"}, {"zener"}) ||
        guardedTitleHas(context, {"zener"})) {
      return resolved("Zener Diode", "Zener Diode", PartLabelSource::VendorCategory);
    }
    if (hasCategoryPhrase(context, {"bridge rectifiers"})) return resolved("Bridge Rectifier", "Bridge Rectifier", PartLabelSource::VendorCategory);
    if (hasCategoryPhrase(context, {"diode arrays", "diode array"})) return resolved("Diode Array", "Diode Array", PartLabelSource::VendorCategory);
    if (hasCategoryPhrase(context, {"varactor"}) || hasParameter(context, {"Diode Type", "Type"}, {"varactor"})) return resolved("Varactor Diode", "Varactor Diode", PartLabelSource::VendorCategory);
    if (hasCategoryPhrase(context, {"rf diodes"}) || hasParameter(context, {"Diode Type", "Type"}, {"rf", "pin"})) return resolved("RF Diode", "RF Diode", PartLabelSource::VendorCategory);
    if (hasCategoryPhrase(context, {"rectifiers"}) || guardedTitleHas(context, {"rectifier"})) return resolved("Rectifier Diode", "Rectifier Diode", PartLabelSource::VendorCategory);
    return resolved("Diode", "Diode", PartLabelSource::VendorCategory);
  }

  if (isMosfet) {
    if (hasParameter(context, {"FET Type", "Transistor Type", "Channel Type"}, {"p channel"}) || guardedTitleHas(context, {"p channel mosfet", "p mosfet", "mosfet p ch"})) return resolved("P-Channel MOSFET", "P-MOSFET", PartLabelSource::VendorCategory);
    if (hasParameter(context, {"FET Type", "Transistor Type", "Channel Type"}, {"n channel"}) || guardedTitleHas(context, {"n channel mosfet", "n mosfet", "mosfet n ch"})) return resolved("N-Channel MOSFET", "N-MOSFET", PartLabelSource::VendorCategory);
    if (hasCategoryPhrase(context, {"fet arrays"})) return resolved("MOSFET Array", "MOSFET Array", PartLabelSource::VendorCategory);
    return resolved("MOSFET", "MOSFET", PartLabelSource::VendorCategory);
  }

  if (isJfet) {
    if (hasParameter(context, {"FET Type", "Transistor Type", "Channel Type"}, {"p channel"}) || guardedTitleHas(context, {"p channel jfet"})) return resolved("P-Channel JFET", "P-JFET", PartLabelSource::VendorCategory);
    if (hasParameter(context, {"FET Type", "Transistor Type", "Channel Type"}, {"n channel"}) || guardedTitleHas(context, {"n channel jfet"})) return resolved("N-Channel JFET", "N-JFET", PartLabelSource::VendorCategory);
    return resolved("JFET", "JFET", PartLabelSource::VendorCategory);
  }

  if (isTransistor) {
    if (hasCategoryPhrase(context, {"transistor arrays"})) return resolved("BJT Array", "BJT Array", PartLabelSource::VendorCategory);
    if (hasParameter(context, {"Transistor Type", "Type"}, {"npn"}) || guardedTitleHas(context, {"npn transistor", "npn bjt", "trans npn"})) return resolved("NPN BJT Transistor", "BJT NPN", PartLabelSource::VendorCategory);
    if (hasParameter(context, {"Transistor Type", "Type"}, {"pnp"}) || guardedTitleHas(context, {"pnp transistor", "pnp bjt", "trans pnp"})) return resolved("PNP BJT Transistor", "BJT PNP", PartLabelSource::VendorCategory);
    return resolved("BJT Transistor", "BJT", PartLabelSource::VendorCategory);
  }

  if (isThyristor) {
    if (hasCategoryPhrase(context, {"triacs"}) || hasParameter(context, {"Type"}, {"triac"})) return resolved("TRIAC", "TRIAC", PartLabelSource::VendorCategory);
    if (hasCategoryPhrase(context, {"diacs"}) || hasParameter(context, {"Type"}, {"diac"})) return resolved("DIAC", "DIAC", PartLabelSource::VendorCategory);
    if (hasCategoryPhrase(context, {"scrs"}) || hasParameter(context, {"Type"}, {"scr"})) return resolved("Silicon Controlled Rectifier", "SCR", PartLabelSource::VendorCategory);
    return resolved("Thyristor", "Thyristor", PartLabelSource::VendorCategory);
  }
  return {};
}

PartDescriptor integratedCircuitRule(const LabelContext& context) {
  // Older scans often retain only the broad "Integrated Circuits" category.
  // Within that trusted family, DigiKey's Function/Topology fields and the
  // standardized product description identify the actual IC role.
  const bool isIc = hasCategoryPhrase(context, {"integrated circuits", "power management", "pmic", "voltage regulators",
                                                "linear amplifiers", "linear comparators", "data acquisition", "clock timing",
                                                "logic", "interface", "memory", "microcontrollers", "embedded processors"});
  if (!isIc) return {};
  const bool isMemoryCategory = hasCategoryPhrase(context, {"memory"});

  // A memory taxonomy is stronger evidence than a word such as "timer" in a
  // memory-controller description. Resolve its concrete memory technology,
  // then retain the honest Memory IC fallback when the technology is absent.
  if (isMemoryCategory) {
    if (hasParameter(context, {"Memory Type"}, {"eeprom"}) || guardedTitleHas(context, {"eeprom"})) return resolved("EEPROM", "EEPROM", PartLabelSource::VendorRule);
    if (hasParameter(context, {"Memory Type"}, {"flash"}) || guardedTitleHas(context, {"flash memory", "nor flash", "nand flash"})) return resolved("Flash Memory", "Flash Memory", PartLabelSource::VendorRule);
    if (hasParameter(context, {"Memory Type"}, {"fram", "feram"}) || guardedTitleHas(context, {"fram", "feram"})) return resolved("FRAM Memory", "FRAM", PartLabelSource::VendorRule);
    if (hasParameter(context, {"Memory Type"}, {"sram"}) || guardedTitleHas(context, {"sram"})) return resolved("SRAM Memory", "SRAM", PartLabelSource::VendorRule);
    if (hasParameter(context, {"Memory Type"}, {"dram", "sdram", "ddr"}) || guardedTitleHas(context, {"sdram", "ddr", "dram"})) return resolved("DRAM Memory", "DRAM", PartLabelSource::VendorRule);
    return resolved("Memory IC", "Memory IC", PartLabelSource::VendorCategory);
  }

  const bool isDcDc = hasParameter(context, {"Function"}, {"dc dc converter", "switching regulator"}) ||
                      hasParameter(context, {"Topology"}) || guardedTitleHas(context, {"reg buck", "buck converter", "boost converter", "buck boost converter"});
  if (isDcDc && (hasParameter(context, {"Topology"}, {"buck boost"}) || guardedTitleHas(context, {"buck boost"}))) return resolved("Buck-Boost Converter", "Buck-Boost Conv.", PartLabelSource::VendorRule);
  if (isDcDc && (hasParameter(context, {"Topology"}, {"buck"}) || guardedTitleHas(context, {"reg buck", "buck converter"}))) return resolved("Buck Converter", "Buck Converter", PartLabelSource::VendorRule);
  if (isDcDc && (hasParameter(context, {"Topology"}, {"boost"}) || guardedTitleHas(context, {"boost converter"}))) return resolved("Boost Converter", "Boost Converter", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"battery charger"}) || guardedTitleHas(context, {"battery charger", "li ion charger", "liion charger"})) return resolved("Battery Charger", "Battery Charger", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"fuel gauge", "battery monitor"}) || guardedTitleHas(context, {"fuel gauge", "battery monitor"})) return resolved("Battery Monitor", "Battery Monitor", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"load switch"}) || guardedTitleHas(context, {"load switch"})) return resolved("Load Switch", "Load Switch", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"power mux", "power multiplexer"}) || guardedTitleHas(context, {"power mux", "power multiplexer"})) return resolved("Power Multiplexer", "Power Mux", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"hot swap"}) || guardedTitleHas(context, {"hot swap"})) return resolved("Hot-Swap Controller", "Hot-Swap Ctrl.", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"led driver"}) || guardedTitleHas(context, {"led driver"})) return resolved("LED Driver", "LED Driver", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Type", "Function"}, {"voltage supervisor", "reset supervisor"}) || guardedTitleHas(context, {"voltage supervisor", "reset supervisor"})) return resolved("Voltage Supervisor", "Volt Supervisor", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Type", "Function"}, {"ldo", "linear regulator"}) || guardedTitleHas(context, {"ldo regulator", "linear regulator", "linear reg"})) return resolved("Linear Voltage Regulator", "Linear Regulator", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Voltage Reference Type"}) || guardedTitleHas(context, {"voltage reference"})) return resolved("Voltage Reference", "Voltage Ref.", PartLabelSource::VendorRule);

  if (hasParameter(context, {"Memory Type"}, {"eeprom"}) || guardedTitleHas(context, {"eeprom"})) return resolved("EEPROM", "EEPROM", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Memory Type"}, {"flash"}) || guardedTitleHas(context, {"flash memory", "nor flash", "nand flash"})) return resolved("Flash Memory", "Flash Memory", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Memory Type"}, {"fram", "feram"}) || guardedTitleHas(context, {"fram", "feram"})) return resolved("FRAM Memory", "FRAM", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Memory Type"}, {"sram"}) || guardedTitleHas(context, {"sram"})) return resolved("SRAM Memory", "SRAM", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Memory Type"}, {"dram", "sdram", "ddr"}) || guardedTitleHas(context, {"sdram", "ddr", "dram"})) return resolved("DRAM Memory", "DRAM", PartLabelSource::VendorRule);

  if (hasParameter(context, {"Function"}, {"analog to digital converter"}) || guardedTitleHas(context, {"analog to digital converter"})) return resolved("Analog-to-Digital Converter", "ADC", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"digital to analog converter"}) || guardedTitleHas(context, {"digital to analog converter"})) return resolved("Digital-to-Analog Converter", "DAC", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"operational amplifier"}) || guardedTitleHas(context, {"operational amplifier", "op amp"})) return resolved("Operational Amplifier", "OP-AMP", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"comparator"}) || guardedTitleHas(context, {"comparator"})) return resolved("Comparator", "Comparator", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"analog switch"}) || guardedTitleHas(context, {"analog switch"})) return resolved("Analog Switch", "Analog Switch", PartLabelSource::VendorRule);

  if (hasParameter(context, {"Function"}, {"real time clock"}) || guardedTitleHas(context, {"real time clock", "rtc"})) return resolved("Real-Time Clock", "RTC", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function", "Type"}, {"timer", "one shot", "monostable", "astable"}) || guardedTitleHas(context, {"timer", "one shot", "monostable", "astable"})) return resolved("Timer IC", "Timer IC", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"clock generator", "frequency synthesizer", "pll"}) || guardedTitleHas(context, {"clock generator", "frequency synthesizer", "pll"})) return resolved("Clock Generator", "Clock Gen.", PartLabelSource::VendorRule);

  if (hasParameter(context, {"Function"}, {"logic gate"}) || guardedTitleHas(context, {"logic gate", "nand gate", "nor gate", "and gate", "or gate", "xor gate"})) return resolved("Logic Gate", "Logic Gate", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"logic buffer"}) || guardedTitleHas(context, {"logic buffer", "bus buffer"})) return resolved("Logic Buffer", "Logic Buffer", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"voltage translator", "level shifter"}) || guardedTitleHas(context, {"voltage translator", "level shifter"})) return resolved("Level Shifter", "Level Shifter", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"flip flop"}) || guardedTitleHas(context, {"flip flop"})) return resolved("Flip-Flop", "Flip-Flop", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"shift register"}) || guardedTitleHas(context, {"shift register"})) return resolved("Shift Register", "Shift Register", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"counter"}) || guardedTitleHas(context, {"counter"})) return resolved("Counter IC", "Counter IC", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"multiplexer", "demultiplexer"}) || guardedTitleHas(context, {"multiplexer", "demultiplexer"})) return resolved("Multiplexer/Demultiplexer", "Mux/Demux", PartLabelSource::VendorRule);

  if (hasParameter(context, {"Function"}, {"usb uart", "usb to uart"}) || guardedTitleHas(context, {"usb uart", "usb to uart"})) return resolved("USB-UART Bridge", "USB-UART Bridge", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Interface"}, {"can", "rs485", "rs 485", "rs232", "rs 232", "lin"}) ||
      hasParameter(context, {"Function"}, {"transceiver"}) || guardedTitleHas(context, {"can transceiver", "rs485 transceiver", "rs232 transceiver"})) return resolved("Transceiver", "Transceiver", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"gate driver"}) || guardedTitleHas(context, {"gate driver"})) return resolved("Gate Driver", "Gate Driver", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Output Type"}, {"h bridge"}) || guardedTitleHas(context, {"h bridge"})) return resolved("H-Bridge Motor Driver", "H-Bridge", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Function"}, {"motor driver"}) || guardedTitleHas(context, {"motor driver"})) return resolved("Motor Driver", "Motor Driver", PartLabelSource::VendorRule);
  if (hasParameter(context, {"Core Processor", "Program Memory Size"}) || guardedTitleHas(context, {"microcontroller", "mcu"})) return resolved("Microcontroller", "Microcontroller", PartLabelSource::VendorRule);
  return {};
}

PartDescriptor specificCategoryMapping(const LabelContext& context) {
  // These are taxonomy leaves supplied by DigiKey, not product-name guesses.
  // Keep them ahead of their family mappings so a concrete component is never
  // reduced to the parent just because the full category path also contains it.
  if (hasCategoryPhrase(context, {"chip resistor", "resistors chip"})) return resolved("Surface-Mount Resistor", "SMD Resistor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"through hole resistors"})) return resolved("Through-Hole Resistor", "THT Resistor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"chassis mount resistors"})) return resolved("Chassis-Mount Resistor", "Chassis Resistor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"resistor networks", "resistor arrays"})) return resolved("Resistor Array", "Resistor Array", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"ceramic capacitors"})) return resolved("Ceramic Capacitor", "Ceramic Cap", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"aluminum electrolytic capacitors"})) return resolved("Aluminum Electrolytic Capacitor", "Al Electrolytic", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"tantalum capacitors"})) return resolved("Tantalum Capacitor", "Tantalum Cap", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"film capacitors"})) return resolved("Film Capacitor", "Film Cap", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"mica", "ptfe capacitors"})) return resolved("Mica Capacitor", "Mica Cap", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"fixed inductors"})) return resolved("Fixed Inductor", "Fixed Inductor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"adjustable inductors"})) return resolved("Adjustable Inductor", "Adjustable Ind.", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"power transformers"})) return resolved("Power Transformer", "Power Transformer", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"audio transformers"})) return resolved("Audio Transformer", "Audio Transformer", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"pulse transformers"})) return resolved("Pulse Transformer", "Pulse Transformer", PartLabelSource::VendorCategory);

  if (hasCategoryPhrase(context, {"photodiodes"})) return resolved("Photodiode", "Photodiode", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"phototransistors"})) return resolved("Phototransistor", "Phototransistor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"tactile switches"})) return resolved("Tactile Switch", "Tactile Switch", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"pushbutton switches"})) return resolved("Pushbutton Switch", "Pushbutton", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"toggle switches"})) return resolved("Toggle Switch", "Toggle Switch", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"slide switches"})) return resolved("Slide Switch", "Slide Switch", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"dip switches"})) return resolved("DIP Switch", "DIP Switch", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"signal relays"})) return resolved("Signal Relay", "Signal Relay", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"power relays"})) return resolved("Power Relay", "Power Relay", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"solid state relays"})) return resolved("Solid-State Relay", "SSR", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"usb connectors", "usb dvi hdmi connectors"})) return resolved("USB Connector", "USB Connector", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"rf connectors"})) return resolved("RF Connector", "RF Connector", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"circular connectors"})) return resolved("Circular Connector", "Circular Conn.", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"d sub connectors"})) return resolved("D-Sub Connector", "D-Sub Connector", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"headers", "wire to board connectors"})) return resolved("Board Header", "Board Header", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"connector contacts"})) return resolved("Connector Contact", "Connector Contact", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"connector housings"})) return resolved("Connector Housing", "Connector Housing", PartLabelSource::VendorCategory);

  if (hasCategoryPhrase(context, {"ac dc converters"})) return resolved("AC-DC Converter", "AC-DC Converter", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"dc dc converters"})) return resolved("DC-DC Converter", "DC-DC Converter", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"rf amplifiers"})) return resolved("RF Amplifier", "RF Amplifier", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"rf mixers"})) return resolved("RF Mixer", "RF Mixer", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"rf switches"})) return resolved("RF Switch", "RF Switch", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"rf attenuators"})) return resolved("RF Attenuator", "RF Attenuator", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"baluns"})) return resolved("Balun", "Balun", PartLabelSource::VendorCategory);
  return {};
}

PartDescriptor categoryMapping(const LabelContext& context) {
  // Exact vendor category names are stable evidence.  These mappings deliberately
  // do not inspect notes, MPNs, tags, or arbitrary free text.
  // Protection and discrete semiconductors: specific leaves precede their
  // deliberately broad parent families.
  if (hasExactCategory(context, {"fuse holders"})) return resolved("Fuse Holder", "Fuse Holder", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"fuses", "thermal fuses", "ptc resettable fuses"})) return resolved("Fuse", "Fuse", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"circuit breakers"})) return resolved("Circuit Breaker", "Circuit Breaker", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"gas discharge tube arresters gdts", "gas discharge tubes"})) return resolved("Gas Discharge Tube", "GDT", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"varistors movs", "varistors"})) return resolved("Varistor", "Varistor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"transient voltage suppressors", "tvs diodes", "esd protection devices"})) return resolved("TVS Diode", "TVS Diode", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"schottky diodes"})) return resolved("Schottky Diode", "Schottky Diode", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"zener diodes"})) return resolved("Zener Diode", "Zener Diode", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"bridge rectifiers"})) return resolved("Bridge Rectifier", "Bridge Rectifier", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"diode arrays", "diode arrays and rectifiers"})) return resolved("Diode Array", "Diode Array", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"rf diodes", "varactor diodes", "rectifiers", "rectifier diodes", "diodes"})) return resolved("Diode", "Diode", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"mosfets", "fet arrays"})) return resolved("MOSFET", "MOSFET", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"igbts"})) return resolved("IGBT", "IGBT", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"thyristors", "scrs", "triacs"})) return resolved("Thyristor", "Thyristor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"transistors", "bipolar transistors", "transistor arrays"})) return resolved("Transistor", "Transistor", PartLabelSource::VendorCategory);

  // Passive, magnetic, and timing components.
  if (hasExactCategory(context, {"resistor networks arrays", "resistor networks", "resistor arrays"})) return resolved("Resistor Array", "Resistor Array", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"trimmer potentiometers", "rotary potentiometers rheostats", "potentiometers"})) return resolved("Potentiometer", "Potentiometer", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"resistors", "chip resistors surface mount", "through hole resistors", "chassis mount resistors"})) return resolved("Resistor", "Resistor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"capacitor networks arrays", "capacitor arrays"})) return resolved("Capacitor Array", "Capacitor Array", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"supercapacitors"})) return resolved("Supercapacitor", "Supercapacitor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"capacitors", "ceramic capacitors", "aluminum electrolytic capacitors", "tantalum capacitors", "film capacitors", "mica and ptfe capacitors"})) return resolved("Capacitor", "Capacitor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"ferrite beads and chips", "ferrite beads"})) return resolved("Ferrite Bead", "Ferrite Bead", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"common mode chokes"})) return resolved("Common Mode Choke", "CM Choke", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"fixed inductors", "adjustable inductors", "inductors"})) return resolved("Inductor", "Inductor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"power transformers", "audio transformers", "pulse transformers", "current sense transformers", "transformers"})) return resolved("Transformer", "Transformer", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"emi rfi filters lc rc networks", "emi rfi filters", "feed through capacitors", "ceramic filters", "saw filters"})) return resolved("EMI Filter", "EMI Filter", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"crystals"})) return resolved("Crystal", "Crystal", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"resonators"})) return resolved("Resonator", "Resonator", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"crystal oscillators", "oscillators", "programmable oscillators"})) return resolved("Oscillator", "Oscillator", PartLabelSource::VendorCategory);

  // Optoelectronics, interconnect, and electromechanical categories.
  if (hasExactCategory(context, {"light emitting diodes", "leds", "led lighting", "led indication"})) return resolved("Light-Emitting Diode", "LED", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"photodiodes", "phototransistors", "optical sensors"})) return resolved("Optical Sensor", "Optical Sensor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"optical isolators", "optoisolators", "optocouplers"})) return resolved("Optocoupler", "Optocoupler", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"display modules lcd oled graphic", "display modules"})) return resolved("Display Module", "Display Module", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"pushbutton switches", "tactile switches", "toggle switches", "slide switches", "rotary switches", "dip switches", "reed switches", "keylock switches", "thumbwheel switches", "switches"})) return resolved("Switch", "Switch", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"encoders", "rotary encoders"})) return resolved("Rotary Encoder", "Rotary Encoder", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"relays", "signal relays", "power relays", "reed relays", "solid state relays"})) return resolved("Relay", "Relay", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"terminal blocks", "terminal block accessories"})) return resolved("Terminal Block", "Terminal Block", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"connector contacts", "connector housings", "headers", "wire to board connectors", "circular connectors", "d sub connectors", "usb connectors", "memory connectors", "rf connectors", "barrel connectors", "card edge connectors", "connectors interconnects", "connectors"})) return resolved("Connector", "Connector", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"cable assemblies", "wire cable"})) return resolved("Cable Assembly", "Cable Assembly", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"fans", "fan accessories"})) return resolved("Fan", "Fan", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"motors ac dc", "motors", "solenoids actuators"})) return resolved("Motor / Actuator", "Motor", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"speakers", "buzzer elements piezo benders", "microphones"})) return resolved("Audio Transducer", "Audio Transducer", PartLabelSource::VendorCategory);

  // Power, RF, development, and common integrated-circuit leaves.
  if (hasExactCategory(context, {"ac dc converters", "dc dc converters", "power supplies board mount", "power modules"})) return resolved("Power Supply", "Power Supply", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"dc dc switching regulators", "switching regulators"})) return resolved("Switching Regulator", "Switch Regulator", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"linear voltage regulators", "ldo voltage regulators"})) return resolved("Linear Voltage Regulator", "Linear Regulator", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"battery chargers", "battery management"})) return resolved("Battery Charger", "Battery Charger", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"batteries rechargeable", "batteries non rechargeable"})) return resolved("Battery", "Battery", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"battery holders clips contacts"})) return resolved("Battery Holder", "Battery Holder", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"antennas", "rf antennas"})) return resolved("Antenna", "Antenna", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"rf amplifiers", "rf mixers", "rf switches", "rf attenuators", "baluns"})) return resolved("RF Component", "RF Component", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"rf transceiver modules and modems", "rf modules"})) return resolved("RF Module", "RF Module", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"development boards kits programmers", "evaluation boards", "programmers emulators and debuggers"})) return resolved("Development Board", "Dev Board", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"memory", "memory ic", "memory integrated circuits"})) return resolved("Memory IC", "Memory IC", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"microcontrollers", "mcus"})) return resolved("Microcontroller", "Microcontroller", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"digital isolators"})) return resolved("Digital Isolator", "Digital Isolator", PartLabelSource::VendorCategory);
  if (hasExactCategory(context, {"interface controllers", "interface specialized"})) return resolved("Interface IC", "Interface IC", PartLabelSource::VendorCategory);

  // Some vendors expose a descriptive leaf rather than a category path.  The
  // phrases below are still category evidence, not a global product-title scan.
  if (hasCategoryPhrase(context, {"temperature sensors", "thermistors"})) return resolved("Temperature Sensor", "Temp Sensor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"pressure sensors", "pressure sensor"})) return resolved("Pressure Sensor", "Pressure Sensor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"humidity moisture sensors"})) return resolved("Humidity Sensor", "Humidity Sensor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"motion sensors", "accelerometers", "gyroscopes", "inertial measurement"})) return resolved("Motion Sensor", "Motion Sensor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"proximity occupancy sensors", "proximity sensors"})) return resolved("Proximity Sensor", "Proximity Sensor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"magnetic sensors", "hall effect sensors"})) return resolved("Magnetic Sensor", "Magnetic Sensor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"current sensors", "current sense amplifiers"})) return resolved("Current Sensor", "Current Sensor", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"isolation amplifiers", "analog isolators"})) return resolved("Isolation Amplifier", "Isolation Ampl.", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"heat sinks", "thermal pads", "thermal interface"})) return resolved("Thermal Hardware", "Thermal Hardware", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"enclosures boxes cases"})) return resolved("Enclosure", "Enclosure", PartLabelSource::VendorCategory);
  if (hasCategoryPhrase(context, {"wire cable", "cable assemblies"})) return resolved("Cable", "Cable", PartLabelSource::VendorCategory);
  return {};
}

PartDescriptor parentCategoryRule(const LabelContext& context) {
  // DigiKey can return a stable parent category rather than the final leaf.
  // A title is accepted only when that parent already identifies the family.
  if (hasCategoryPhrase(context, {"circuit protection"}) && guardedTitleHas(context, {"fuse"})) {
    return resolved("Fuse", "Fuse", PartLabelSource::VendorRule);
  }
  if (hasCategoryPhrase(context, {"inductors coils chokes"}) && guardedTitleHas(context, {"fixed ind", "inductor", "choke"})) {
    return resolved("Inductor", "Inductor", PartLabelSource::VendorRule);
  }
  if (hasCategoryPhrase(context, {"switches"}) &&
      guardedTitleHas(context, {"switch", "pushbutton", "tactile", "toggle", "slide", "rotary"})) {
    return resolved("Switch", "Switch", PartLabelSource::VendorRule);
  }
  if (hasCategoryPhrase(context, {"connectors interconnects", "connectors"}) &&
      guardedTitleHas(context, {"connector", "header", "socket", "terminal"})) {
    return resolved("Connector", "Connector", PartLabelSource::VendorRule);
  }
  return {};
}

PartDescriptor categoryAndParameterRule(const LabelContext& context) {
  const bool isIc = hasExactCategory(context, {"integrated circuits", "integrated circuits ics", "power management ics", "power management pmics", "logic", "interface", "linear amplifiers", "linear comparators", "data acquisition", "clock timing", "voltage regulators"});
  const bool isPower = hasExactCategory(context, {"power management ics", "power management pmics", "voltage regulators"}) ||
                       (isIc && hasParameter(context, {"Function"}, {"dc dc converter", "battery charger", "load switch"}));

  if (isPower && hasParameter(context, {"Topology"}, {"buck boost"})) return resolved("Buck-Boost Converter", "Buck-Boost Conv.", PartLabelSource::VendorRule);
  if (isPower && hasParameter(context, {"Topology"}, {"buck"})) return resolved("Buck Converter", "Buck Converter", PartLabelSource::VendorRule);
  if (isPower && hasParameter(context, {"Topology"}, {"boost"})) return resolved("Boost Converter", "Boost Converter", PartLabelSource::VendorRule);
  if (isPower && hasParameter(context, {"Function"}, {"battery charger"})) return resolved("Battery Charger", "Battery Charger", PartLabelSource::VendorRule);
  if (isPower && hasParameter(context, {"Function"}, {"load switch"})) return resolved("Load Switch", "Load Switch", PartLabelSource::VendorRule);
  if (isPower && (hasParameter(context, {"Type"}, {"ldo", "linear"}) || hasExactCategory(context, {"voltage regulators"}))) return resolved("Linear Voltage Regulator", "Linear Regulator", PartLabelSource::VendorRule);
  if (isPower && hasParameter(context, {"Type"}, {"voltage supervisor"})) return resolved("Voltage Supervisor", "Volt Supervisor", PartLabelSource::VendorRule);

  if (isIc && hasParameter(context, {"Voltage Reference Type"})) return resolved("Voltage Reference", "Voltage Ref.", PartLabelSource::VendorRule);
  if (isIc && hasParameter(context, {"Function"}, {"protection"})) return resolved("Protection IC", "Protection IC", PartLabelSource::VendorRule);
  if ((hasExactCategory(context, {"linear amplifiers"}) && (hasParameter(context, {"Gain Bandwidth", "Slew Rate"}) || guardedTitleHas(context, {"operational amplifier", "op amp"}))) ||
      (isIc && guardedTitleHas(context, {"operational amplifier", "op amp"}))) return resolved("Operational Amplifier", "OP-AMP", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"linear comparators"}) || (isIc && hasParameter(context, {"Function"}, {"comparator"}))) return resolved("Comparator", "Comparator", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"data acquisition"}) && hasParameter(context, {"Resolution"}) && guardedTitleHas(context, {"analog to digital", "adc"})) return resolved("Analog-to-Digital Converter", "ADC", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"data acquisition"}) && hasParameter(context, {"Resolution"}) && guardedTitleHas(context, {"digital to analog", "dac"})) return resolved("Digital-to-Analog Converter", "DAC", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"clock timing"}) && guardedTitleHas(context, {"real time clock", "rtc"})) return resolved("Real-Time Clock", "RTC", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"clock timing"}) && guardedTitleHas(context, {"clock generator", "pll"})) return resolved("Clock Generator", "Clock Gen.", PartLabelSource::VendorRule);
  if ((hasExactCategory(context, {"clock timing"}) &&
       (guardedTitleHas(context, {"timer", "one shot", "monostable", "astable"}) ||
        hasParameter(context, {"Type", "Function"}, {"timer", "one shot", "monostable", "astable"}))) ||
      (isIc && (guardedTitleHas(context, {"timer", "one shot", "monostable", "astable"}) ||
                hasParameter(context, {"Function"}, {"timer", "one shot", "monostable", "astable"})))) {
    return resolved("Timer IC", "Timer IC", PartLabelSource::VendorRule);
  }
  if (isIc && hasParameter(context, {"Function"}, {"logic gate"})) return resolved("Logic Gate", "Logic Gate", PartLabelSource::VendorRule);
  if (isIc && hasParameter(context, {"Function"}, {"logic buffer"})) return resolved("Logic Buffer", "Logic Buffer", PartLabelSource::VendorRule);
  if (isIc && hasParameter(context, {"Function"}, {"voltage translator"})) return resolved("Level Shifter", "Level Shifter", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"interface"}) && hasParameter(context, {"Interface"}, {"can", "rs485", "rs 485", "rs232", "rs 232"})) return resolved("Transceiver", "Transceiver", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"interface"}) && hasParameter(context, {"Function"}, {"multiplexer"})) return resolved("Multiplexer/Demultiplexer", "Mux/Demux", PartLabelSource::VendorRule);
  if (isIc && hasParameter(context, {"Output Type"}, {"h bridge"})) return resolved("H-Bridge Motor Driver", "H-Bridge", PartLabelSource::VendorRule);
  if (isIc && hasParameter(context, {"Function"}, {"motor driver"})) return resolved("Motor Driver", "Motor Driver", PartLabelSource::VendorRule);
  if (isIc && hasParameter(context, {"Function"}, {"gate driver"})) return resolved("Gate Driver", "Gate Driver", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"sensors"}) && hasParameter(context, {"Sensor Type"}, {"temperature"})) return resolved("Temperature Sensor", "Temp Sensor", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"sensors"}) && hasParameter(context, {"Sensor Type"}, {"pressure"})) return resolved("Pressure Sensor", "Pressure Sensor", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"sensors"}) && hasParameter(context, {"Sensor Type"}, {"accelerometer", "gyroscope", "imu"})) return resolved("Inertial Measurement Unit", "3 Axis IMU", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"sensors"}) && hasParameter(context, {"Function"}, {"current monitor"})) return resolved("Current Sensor", "Current Sensor", PartLabelSource::VendorRule);
  if (hasExactCategory(context, {"microcontrollers", "mcus"}) && hasParameter(context, {"Core Processor", "Program Memory Size"})) return resolved("Microcontroller", "Microcontroller", PartLabelSource::VendorRule);
  return {};
}

PartDescriptor parentFamily(const LabelContext& context) {
  if (hasExactCategory(context, {"integrated circuits", "integrated circuits ics"})) return resolved("Integrated Circuit", "IC", PartLabelSource::ParentFamily);
  if (hasExactCategory(context, {"power management ics", "power management pmics"})) return resolved("Power Management IC", "Power IC", PartLabelSource::ParentFamily);
  if (hasExactCategory(context, {"data acquisition"})) return resolved("Data Converter", "Data Converter", PartLabelSource::ParentFamily);
  if (hasExactCategory(context, {"clock timing"})) return resolved("Timing IC", "Timing IC", PartLabelSource::ParentFamily);
  if (hasExactCategory(context, {"logic"})) return resolved("Logic IC", "Logic IC", PartLabelSource::ParentFamily);
  if (hasExactCategory(context, {"interface"})) return resolved("Interface IC", "Interface IC", PartLabelSource::ParentFamily);
  if (hasExactCategory(context, {"sensors"})) return resolved("Sensor", "Sensor", PartLabelSource::ParentFamily);
  return {};
}

bool isGenericCategory(const string& category) {
  const auto key = normalizedKey(category);
  for (const auto* generic : {"", "productindex", "products", "components", "electroniccomponents", "misc", "other", "unknown", "uncategorized"}) {
    if (key == generic) {
      return true;
    }
  }
  return false;
}

string fallbackCategory(const LabelContext& context) {
  // The last category segment is normally the provider's most useful leaf. It
  // remains honest for categories we do not yet map, while avoiding the former
  // meaningless "Part" label.
  for (auto it = context.categories.rbegin(); it != context.categories.rend(); ++it) {
    const auto category = trim(*it);
    if (!category.empty() && !isGenericCategory(category)) {
      return category;
    }
  }
  return "Unclassified Component";
}

}  // namespace

PartDescriptor describePart(const InventoryItem& item) {
  if (!trim(item.labelOverride).empty()) {
    return resolved(item.labelOverride, item.labelOverride, PartLabelSource::ManualOverride);
  }

  const auto context = makeContext(item);
  if (auto descriptor = discreteSemiconductorRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = integratedCircuitRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = specificCategoryMapping(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = categoryMapping(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = parentCategoryRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = categoryAndParameterRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = parentFamily(context); !descriptor.printLabel.empty()) return descriptor;
  const auto fallback = fallbackCategory(context);
  return resolved(fallback, fallback, PartLabelSource::Fallback);
}

string partShortDescription(const InventoryItem& item) {
  return describePart(item).printLabel;
}

string partPurposeLabel(const InventoryItem& item) {
  return describePart(item).purposeLabel;
}

}  // namespace inventatory
