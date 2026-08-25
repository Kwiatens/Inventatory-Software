// Inventatory - Hardware Inventory Management System
// Physical value parsing and comparison for component parameters.

#include "core/PhysicalValue.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace inventatory {

namespace {

double parseSiPrefix(const std::string& text, size_t& consumed) {
  if (text.empty()) {
    consumed = 0;
    return 0.0;
  }

  char prefix = static_cast<char>(tolower(static_cast<unsigned char>(text[0])));
  double multiplier = 0.0;

  switch (prefix) {
    case 'p': multiplier = 1e-12; break;
    case 'n': multiplier = 1e-9; break;
    case 'u': multiplier = 1e-6; break;
    case 'm': multiplier = 1e-3; break;
    case 'k': multiplier = 1e3; break;
    case 'M': multiplier = 1e6; break;
    case 'g': multiplier = 1e9; break;
    default: consumed = 0; return 0.0;
  }

  consumed = 1;
  return multiplier;
}

PhysicalValueType guessTypeFromUnit(const std::string& unit) {
  if (unit.empty()) return PhysicalValueType::Unknown;
  char lower = static_cast<char>(tolower(static_cast<unsigned char>(unit[0])));
  switch (lower) {
    case 'o': return PhysicalValueType::Resistance;  // Ohm
    case 'f': return PhysicalValueType::Capacitance;  // Farad
    case 'h': return PhysicalValueType::Inductance;   // Henry
    case 'z': return PhysicalValueType::Resistance;   // Z (alias for Ohm in some contexts)
    default: break;
  }
  return PhysicalValueType::Unknown;
}

PhysicalValueType guessTypeFromSuffix(const std::string& text) {
  // Look at the last character to determine type
  if (text.empty()) return PhysicalValueType::Unknown;
  char last = static_cast<char>(tolower(static_cast<unsigned char>(text.back())));
  switch (last) {
    case 'o': return PhysicalValueType::Resistance;
    case 'f': return PhysicalValueType::Capacitance;
    case 'h': return PhysicalValueType::Inductance;
    case 'z': return PhysicalValueType::Resistance;
    default: break;
  }
  return PhysicalValueType::Unknown;
}

// Try to parse "4R7" style notation (e.g., 4R7 = 4.7, 10R0 = 10.0)
// Only valid for resistance values.
bool tryParseRNotation(const std::string& text, double& value) {
  if (text.empty()) return false;

  size_t rPos = text.find('r');
  if (rPos == std::string::npos) {
    rPos = text.find('R');
  }
  if (rPos == 0 || rPos == text.size() - 1) return false;

  // Check that everything before 'R' is a digit
  for (size_t i = 0; i < rPos; ++i) {
    if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
  }

  // Check that everything after 'R' is a digit
  for (size_t i = rPos + 1; i < text.size(); ++i) {
    if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
  }

  std::string before = text.substr(0, rPos);
  std::string after = text.substr(rPos + 1);

  double intPart = 0.0;
  double fracPart = 0.0;
  double fracDivisor = 1.0;

  try {
    intPart = std::stod(before);
  } catch (...) {
    return false;
  }

  for (char c : after) {
    fracPart = fracPart * 10.0 + (c - '0');
    fracDivisor *= 10.0;
  }

  value = intPart + (fracPart / fracDivisor);
  return true;
}

}  // namespace

std::optional<PhysicalValue> parsePhysicalValue(const std::string& text) {
  if (text.empty()) return std::nullopt;

  // Trim whitespace
  std::string trimmed = text;
  trimmed.erase(0, trimmed.find_first_not_of(" \t"));
  trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
  if (trimmed.empty()) return std::nullopt;

  // Try "4R7" notation first (resistance only)
  double rValue = 0.0;
  if (tryParseRNotation(trimmed, rValue)) {
    return PhysicalValue{rValue, PhysicalValueType::Resistance};
  }

  // Try to find the numeric part and the unit part
  // Pattern: [number][SI prefix][unit] or [number][unit]
  // Examples: "100nF", "0.1uF", "10k Ohm", "1MHz", "4.7uH", "100 Ohm"

  // Find where the number ends
  size_t numEnd = 0;
  bool hasDot = false;
  while (numEnd < trimmed.size()) {
    char c = trimmed[numEnd];
    if (isdigit(static_cast<unsigned char>(c))) {
      numEnd++;
    } else if (c == '.' && !hasDot) {
      hasDot = true;
      numEnd++;
    } else {
      break;
    }
  }

  if (numEnd == 0 || numEnd == trimmed.size()) return std::nullopt;

  // Parse the numeric value
  std::string numStr = trimmed.substr(0, numEnd);
  double number = 0.0;
  try {
    number = std::stod(numStr);
  } catch (...) {
    return std::nullopt;
  }

  // Parse the remaining string for SI prefix and unit
  std::string remaining = trimmed.substr(numEnd);
  size_t consumed = 0;
  double multiplier = parseSiPrefix(remaining, consumed);

  // If no SI prefix found, check if remaining starts with a unit
  if (multiplier == 0.0) {
    // Check for "Ohm" or "ohm"
    if (remaining.size() >= 3) {
      std::string unit3 = remaining.substr(0, 3);
      std::string unit3Lower;
      for (char c : unit3) unit3Lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (unit3Lower == "ohm") {
        return PhysicalValue{number, PhysicalValueType::Resistance};
      }
    }
    // Check for "Hz" or "HZ"
    if (remaining.size() >= 2) {
      std::string unit2 = remaining.substr(0, 2);
      std::string unit2Lower;
      for (char c : unit2) unit2Lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (unit2Lower == "hz") {
        return PhysicalValue{number, PhysicalValueType::Frequency};
      }
    }
    // Check for single char unit
    if (remaining.size() >= 1) {
      char c = static_cast<char>(tolower(static_cast<unsigned char>(remaining[0])));
      if (c == 'o') return PhysicalValue{number, PhysicalValueType::Resistance};
      if (c == 'f') return PhysicalValue{number, PhysicalValueType::Capacitance};
      if (c == 'h') return PhysicalValue{number, PhysicalValueType::Inductance};
      if (c == 'z') return PhysicalValue{number, PhysicalValueType::Resistance};
      if (c == 'e') {
        // "e" could be part of scientific notation or "e" suffix (rare)
        // If followed by a digit, it's scientific notation
        if (remaining.size() >= 2 && isdigit(static_cast<unsigned char>(remaining[1]))) {
          // Try parsing the whole thing as a plain number with scientific notation
          try {
            number = std::stod(trimmed);
            // Can't determine type from plain scientific notation, return Unknown
            return PhysicalValue{number, PhysicalValueType::Unknown};
          } catch (...) {
            return std::nullopt;
          }
        }
      }
    }
    return std::nullopt;
  }

  // SI prefix found, now look for unit after the prefix
  std::string afterPrefix = remaining.substr(consumed);
  PhysicalValueType type = PhysicalValueType::Unknown;

  if (!afterPrefix.empty()) {
    char c = static_cast<char>(tolower(static_cast<unsigned char>(afterPrefix[0])));
    switch (c) {
      case 'o': type = PhysicalValueType::Resistance; break;
      case 'f': type = PhysicalValueType::Capacitance; break;
      case 'h': type = PhysicalValueType::Inductance; break;
      case 'z': type = PhysicalValueType::Resistance; break;
      case 'e':
      case 's':
      case 'v':
      case 'a':
      case 'w':
        // These could be unit suffixes (e.g., "uF" where 'F' is the unit)
        // If the prefix was 'u' and next char is 'F', it's capacitance
        if (afterPrefix.size() >= 2) {
          char c2 = static_cast<char>(tolower(static_cast<unsigned char>(afterPrefix[1])));
          if (c2 == 'f') type = PhysicalValueType::Capacitance;
          else if (c2 == 'h') type = PhysicalValueType::Inductance;
          else if (c2 == 'o') type = PhysicalValueType::Resistance;
          else if (c2 == 'z') type = PhysicalValueType::Resistance;
        }
        break;
      default: break;
    }
  }

  // If no unit found after prefix, try to guess from context
  if (type == PhysicalValueType::Unknown) {
    // Check if the original text has any unit indicators
    type = guessTypeFromSuffix(trimmed);
  }

  if (type == PhysicalValueType::Unknown) return std::nullopt;

  return PhysicalValue{number * multiplier, type};
}

bool physicalValueMatches(const std::string& a, const std::string& b, double tolerance) {
  auto parsedA = parsePhysicalValue(a);
  auto parsedB = parsePhysicalValue(b);

  if (!parsedA.has_value() || !parsedB.has_value()) return false;
  if (parsedA->type != parsedB->type) return false;
  if (parsedA->type == PhysicalValueType::Unknown) return false;

  // Avoid division by zero
  double maxVal = std::max(std::abs(parsedA->value), std::abs(parsedB->value));
  if (maxVal < 1e-15) {
    // Both are essentially zero
    return std::abs(parsedA->value - parsedB->value) < tolerance * 1e-15;
  }

  double diff = std::abs(parsedA->value - parsedB->value);
  return (diff / maxVal) <= tolerance;
}

double defaultTolerance(PhysicalValueType type) {
  switch (type) {
    case PhysicalValueType::Resistance: return 0.01;
    case PhysicalValueType::Capacitance: return 0.01;
    case PhysicalValueType::Inductance: return 0.05;
    case PhysicalValueType::Frequency: return 0.01;
    case PhysicalValueType::Unknown: return 0.01;
  }
  return 0.01;
}

double toleranceForType(const PhysicalValueTolerances& tolerances, PhysicalValueType type) {
  switch (type) {
    case PhysicalValueType::Resistance: return tolerances.resistance;
    case PhysicalValueType::Capacitance: return tolerances.capacitance;
    case PhysicalValueType::Inductance: return tolerances.inductance;
    case PhysicalValueType::Frequency: return tolerances.frequency;
    case PhysicalValueType::Unknown: return tolerances.resistance;
  }
  return 0.01;
}

PhysicalValueType parameterNameToType(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  // Remove common suffixes like " (ohm)", " (f)", etc.
  auto paren = lower.find('(');
  if (paren != std::string::npos) {
    lower = lower.substr(0, paren);
  }
  // Trim whitespace
  lower.erase(lower.find_last_not_of(" \t") + 1);

  static const struct { const char* name; PhysicalValueType type; } table[] = {
    {"resistance", PhysicalValueType::Resistance},
    {"res", PhysicalValueType::Resistance},
    {"r", PhysicalValueType::Resistance},
    {"impedance", PhysicalValueType::Resistance},
    {"imp", PhysicalValueType::Resistance},
    {"capacitance", PhysicalValueType::Capacitance},
    {"cap", PhysicalValueType::Capacitance},
    {"c", PhysicalValueType::Capacitance},
    {"inductance", PhysicalValueType::Inductance},
    {"ind", PhysicalValueType::Inductance},
    {"l", PhysicalValueType::Inductance},
    {"frequency", PhysicalValueType::Frequency},
    {"freq", PhysicalValueType::Frequency},
    {"f", PhysicalValueType::Frequency},
    {"voltage", PhysicalValueType::Unknown},
    {"v", PhysicalValueType::Unknown},
    {"current", PhysicalValueType::Unknown},
    {"wavelength", PhysicalValueType::Unknown},
    {"wl", PhysicalValueType::Unknown},
    {"dc resistance", PhysicalValueType::Resistance},
    {"dcr", PhysicalValueType::Resistance},
    {"esr", PhysicalValueType::Resistance},
    {"q factor", PhysicalValueType::Unknown},
    {"q", PhysicalValueType::Unknown},
    {"dl", PhysicalValueType::Unknown},
    {"dissipation factor", PhysicalValueType::Unknown},
    {"df", PhysicalValueType::Unknown},
    {"loss tangent", PhysicalValueType::Unknown},
    {"leakage", PhysicalValueType::Unknown},
    {"tolerance", PhysicalValueType::Unknown},
    {"nominal resistance", PhysicalValueType::Resistance},
    {"nominal capacitance", PhysicalValueType::Capacitance},
    {"nominal inductance", PhysicalValueType::Inductance},
    {"nominal frequency", PhysicalValueType::Frequency},
  };

  for (const auto& entry : table) {
    if (lower == entry.name) {
      return entry.type;
    }
  }
  return PhysicalValueType::Unknown;
}

}  // namespace inventatory
