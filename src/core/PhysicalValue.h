// Inventatory - Hardware Inventory Management System
// Physical value parsing and comparison for component parameters.

#pragma once

#include <optional>
#include <string>

namespace inventatory {

enum class PhysicalValueType {
  Resistance,
  Capacitance,
  Inductance,
  Frequency,
  Unknown,
};

struct PhysicalValueTolerances {
  double resistance = 0.01;
  double capacitance = 0.01;
  double inductance = 0.05;
  double frequency = 0.01;
};

struct PhysicalValue {
  double value;
  PhysicalValueType type;
};

// Parses a string like "100nF", "0.1uF", "10k Ohm", "4R7", "1MHz" into a
// normalized physical value. Returns nullopt if the string cannot be parsed.
std::optional<PhysicalValue> parsePhysicalValue(const std::string& text);

// Compares two parameter value strings by parsing both as physical values and
// checking if they are within the given tolerance (e.g., 0.01 for 1%).
// Returns true if both parse to the same type and are within tolerance.
bool physicalValueMatches(const std::string& a, const std::string& b, double tolerance);

// Returns the tolerance for a given physical value type.
double defaultTolerance(PhysicalValueType type);

// Maps a parameter name to its physical value type.
// Recognizes common aliases: "res", "cap", "ind", "freq", etc.
PhysicalValueType parameterNameToType(const std::string& name);

}  // namespace inventatory
