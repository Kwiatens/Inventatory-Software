// Inventatory - Component category and context classification helpers.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "core/parts/PartDescriptor.h"
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

bool startsWithInsensitive(const string& value, const string& prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  return lowerAscii(value.substr(0, prefix.size())) == lowerAscii(prefix);
}

string diodeMainLabelValue(const InventoryItem& item) {
  // The diode type (Zener/Schottky/TVS/Rectifier) is already printed in the
  // header bar, so the main line should carry the
  // actual part identifier instead of the catalog description, which often
  // repeats the type ("DIODE ZENER 4.7V ..."). Prefer the concrete
  // manufacturer/vendor part number over the descriptive part name.
  // The distributor's own number ("641-1127-1-ND") is not what is printed on
  // the part, so the manufacturer part number comes first.
  if (const auto mpn = trim(item.vendorMetadata.manufacturerPartNumber); !mpn.empty()) {
    return mpn;
  }
  const auto sku = trim(item.sku);
  if (!sku.empty()) {
    return sku;
  }

  const auto digikeyPartNumber = trim(item.digikeyPartNumber);
  if (!digikeyPartNumber.empty()) {
    return digikeyPartNumber;
  }

  const auto partName = trim(item.partName);
  if (partName.empty()) {
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
  if (const auto stripped = cleaned("diode schottky "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("rectifier diode "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("diode rectifier "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("zener diode "); !stripped.empty()) return stripped;
  if (const auto stripped = cleaned("diode zener "); !stripped.empty()) return stripped;
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

}  // namespace label_printer_detail
}  // namespace inventatory
