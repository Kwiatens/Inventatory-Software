// Inventatory - Component category and context classification helpers.

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
  // The diode type (Zener/Schottky/TVS/Rectifier) is already printed in the
  // header bar via diodeContextHeader, so the main line should carry the
  // actual part identifier instead of the catalog description, which often
  // repeats the type ("DIODE ZENER 4.7V ..."). Prefer the concrete
  // manufacturer/vendor part number over the descriptive part name.
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
  return "Unclassified Component";
}

}  // namespace label_printer_detail
}  // namespace inventatory
