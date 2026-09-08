// Inventatory - Item label detail selection helpers.

#include "label_printer/LabelPrinterPrivate.h"

#include "core/InventoryInternals.h"
#include "core/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <system_error>

namespace inventatory {

using namespace std;

namespace label_printer_detail {

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

  // Distributor descriptions such as "MOSFET N-CH 30V 5A" explain a part,
  // but are not its printable name. For discrete transistors, preserve the
  // manufacturer's actual part number when DigiKey supplied one.
  if (categoryContains(item, {"transistor", "mosfet", "fet", "discrete semiconductor"}) ||
      itemTextContains(item, {"mosfet", "trans npn", "trans pnp", "bjt transistor"})) {
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

}  // namespace label_printer_detail
}  // namespace inventatory
