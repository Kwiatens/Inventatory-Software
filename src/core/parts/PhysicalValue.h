// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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

// Parses a value such as "100nF" or "4R7" into a normalized value; nullopt if it cannot be parsed.
std::optional<PhysicalValue> parsePhysicalValue(const std::string& text);

// Compares candidate with target using the built-in search bands. Returns the values, relative
// distance and band; nullopt if either fails to parse or the types differ or are unknown.
std::optional<PhysicalValueComparison> comparePhysicalValues(const std::string& candidate,
                                                             const std::string& target);

// Label for a band, as shown in the Stock result groups.
const char* physicalValueMatchBandName(PhysicalValueMatchBand band);

// Maps a parameter name, including common aliases such as "res" or "cap", to its value type.
PhysicalValueType parameterNameToType(const std::string& name);

// Heuristics on free-form parameter text, shared by the DigiKey importer and the label printer.
namespace value_text {

bool looksLikeFrequencyValue(const std::string& value);
bool looksLikeInductanceValue(const std::string& value);
// Finds a value such as "4R7 uH" or "10 mH" in running text and returns it as "4.7uH" or "10mH".
std::optional<std::string> extractInductance(const std::string& text);

}  // namespace value_text

}  // namespace inventatory
