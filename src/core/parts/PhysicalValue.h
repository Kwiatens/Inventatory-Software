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

struct PhysicalValue {
  double value;
  PhysicalValueType type;
};

enum class PhysicalValueMatchBand {
  None,
  Exact,
  Workable,
  Possible,
};

struct PhysicalValueComparison {
  PhysicalValueType type = PhysicalValueType::Unknown;
  double targetValue = 0.0;
  double candidateValue = 0.0;
  double relativeDifference = 0.0;
  double signedRelativeDifference = 0.0;
  PhysicalValueMatchBand band = PhysicalValueMatchBand::None;
};

// Parses a string like "100nF", "0.1uF", "10k Ohm", "4R7", "1MHz" into a
// normalized physical value. Returns nullopt if the string cannot be parsed.
std::optional<PhysicalValue> parsePhysicalValue(const std::string& text);

// Compares two parameter value strings using the built-in search bands. The
// first argument is the candidate value and the second is the requested value.
// Returns normalized values, signed-free relative distance, and the band.
std::optional<PhysicalValueComparison> comparePhysicalValues(const std::string& candidate,
                                                             const std::string& target);

// Returns the human-readable label used by the Stock result groups.
const char* physicalValueMatchBandName(PhysicalValueMatchBand band);

// Maps a parameter name to its physical value type.
// Recognizes common aliases: "res", "cap", "ind", "freq", etc.
PhysicalValueType parameterNameToType(const std::string& name);

// Heuristics on free-form parameter text, shared by the DigiKey importer and the label printer.
namespace value_text {

bool looksLikeFrequencyValue(const std::string& value);
bool looksLikeInductanceValue(const std::string& value);
// Finds a value such as "4R7 uH" or "10 mH" in running text and returns it as "4.7uH" or "10mH".
std::optional<std::string> extractInductance(const std::string& text);

}  // namespace value_text

}  // namespace inventatory
