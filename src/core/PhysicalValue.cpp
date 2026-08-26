// Inventatory - Hardware Inventory Management System
// Physical value parsing and comparison for component parameters.

#include "core/PhysicalValue.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace inventatory {

namespace {

using std::string;

string trimValue(string value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

bool startsWithInsensitive(const string& value, const string& prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (size_t index = 0; index < prefix.size(); ++index) {
    const auto lhs = static_cast<char>(tolower(static_cast<unsigned char>(value[index])));
    const auto rhs = static_cast<char>(tolower(static_cast<unsigned char>(prefix[index])));
    if (lhs != rhs) {
      return false;
    }
  }
  return true;
}

struct ParsedUnit {
  PhysicalValueType type = PhysicalValueType::Unknown;
  double multiplier = 1.0;
};

bool parsePrefix(char prefix, double& multiplier) {
  // SI is case-insensitive except for m/M: milli and mega are different
  // prefixes. K is accepted as a common component-marking spelling of k.
  switch (prefix) {
    case 'p':
    case 'P': multiplier = 1e-12; return true;
    case 'n':
    case 'N': multiplier = 1e-9; return true;
    case 'u':
    case 'U': multiplier = 1e-6; return true;
    case 'm': multiplier = 1e-3; return true;
    case 'M': multiplier = 1e6; return true;
    case 'k':
    case 'K': multiplier = 1e3; return true;
    case 'g':
    case 'G': multiplier = 1e9; return true;
    default: return false;
  }
}

bool parseUnicodeMicroPrefix(const string& value, size_t& consumed, double& multiplier) {
  // DigiKey data commonly uses U+00B5 (micro sign), while some sources use
  // U+03BC (Greek small letter mu). Both are UTF-8 and mean the same SI prefix.
  static constexpr char kMicroSign[] = "\xC2\xB5";
  static constexpr char kGreekMu[] = "\xCE\xBC";
  if (value.rfind(kMicroSign, 0) == 0 || value.rfind(kGreekMu, 0) == 0) {
    consumed = 2;
    multiplier = 1e-6;
    return true;
  }
  consumed = 0;
  return false;
}

PhysicalValueType unitType(const string& unit) {
  if (unit.empty()) {
    return PhysicalValueType::Unknown;
  }

  if (unit == "\xCE\xA9" || startsWithInsensitive(unit, "ohm") || unit[0] == 'R' || unit[0] == 'r') {
    return PhysicalValueType::Resistance;
  }
  if (startsWithInsensitive(unit, "hz") || startsWithInsensitive(unit, "hertz")) {
    return PhysicalValueType::Frequency;
  }
  if (unit[0] == 'F' || unit[0] == 'f' || startsWithInsensitive(unit, "farad")) {
    return PhysicalValueType::Capacitance;
  }
  if (unit[0] == 'H' || unit[0] == 'h' || startsWithInsensitive(unit, "henry") ||
      startsWithInsensitive(unit, "henries")) {
    return PhysicalValueType::Inductance;
  }
  return PhysicalValueType::Unknown;
}

ParsedUnit parseUnit(const string& suffix) {
  const auto remaining = trimValue(suffix);
  if (remaining.empty()) {
    return {};
  }

  // A unit without an SI prefix.
  const auto directType = unitType(remaining);
  if (directType != PhysicalValueType::Unknown) {
    return {directType, 1.0};
  }

  double multiplier = 1.0;
  size_t prefixLength = 0;
  if (!parseUnicodeMicroPrefix(remaining, prefixLength, multiplier)) {
    prefixLength = 1;
    if (!parsePrefix(remaining.front(), multiplier)) {
      return {};
    }
  }

  const auto unit = trimValue(remaining.substr(prefixLength));
  if (unit.empty()) {
    // A bare prefixed component value such as 10k or 1M is conventionally a
    // resistor value. Values with n/u/p need a unit because they are
    // otherwise ambiguous between capacitance and inductance.
    if (prefixLength == 1 &&
        (remaining.front() == 'k' || remaining.front() == 'K' || remaining.front() == 'M' ||
         remaining.front() == 'm' || remaining.front() == 'g' || remaining.front() == 'G')) {
      return {PhysicalValueType::Resistance, multiplier};
    }
    return {};
  }

  const auto type = unitType(unit);
  if (type == PhysicalValueType::Unknown) {
    return {};
  }
  return {type, multiplier};
}

bool allDigits(const string& value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return isdigit(character) != 0;
  });
}

// Parse RKM notation: R280 = 0.28, 4R7 = 4.7, 280R = 280, and 4K7 = 4.7k.
std::optional<PhysicalValue> parseRkmValue(const string& text) {
  size_t alpha = string::npos;
  for (size_t index = 0; index < text.size(); ++index) {
    if (isalpha(static_cast<unsigned char>(text[index])) != 0) {
      if (alpha != string::npos) {
        return std::nullopt;
      }
      alpha = index;
    }
  }
  if (alpha == string::npos) {
    return std::nullopt;
  }

  const char marker = text[alpha];
  const bool resistanceMarker = marker == 'r' || marker == 'R';
  double multiplier = 1.0;
  const bool knownMultiplier = parsePrefix(marker, multiplier);
  if (!resistanceMarker && !knownMultiplier) {
    return std::nullopt;
  }

  const auto head = text.substr(0, alpha);
  const auto tail = text.substr(alpha + 1);
  if ((!head.empty() && !allDigits(head)) || (!tail.empty() && !allDigits(tail))) {
    return std::nullopt;
  }
  if (head.empty() && tail.empty()) {
    return std::nullopt;
  }

  // A marker followed by digits is decimal RKM notation. A marker at the end
  // is an ordinary unit/prefix notation and is handled by parseUnit instead.
  if (tail.empty() && !resistanceMarker) {
    return std::nullopt;
  }

  double integerPart = 0.0;
  double fractionalPart = 0.0;
  if (!head.empty()) {
    integerPart = std::strtod(head.c_str(), nullptr);
  }
  if (!tail.empty()) {
    fractionalPart = std::strtod(tail.c_str(), nullptr);
    for (size_t index = 0; index < tail.size(); ++index) {
      fractionalPart /= 10.0;
    }
  }

  const auto value = integerPart + fractionalPart;
  if (resistanceMarker) {
    return PhysicalValue{value, PhysicalValueType::Resistance};
  }
  return PhysicalValue{value * multiplier, PhysicalValueType::Resistance};
}

}  // namespace

std::optional<PhysicalValue> parsePhysicalValue(const std::string& text) {
  const auto trimmed = trimValue(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  if (const auto rkm = parseRkmValue(trimmed); rkm.has_value()) {
    return rkm;
  }

  // strtod handles signs, .5, and scientific notation while leaving the unit
  // suffix for the component-specific parser below.
  char* numberEnd = nullptr;
  const double number = std::strtod(trimmed.c_str(), &numberEnd);
  if (numberEnd == trimmed.c_str()) {
    return std::nullopt;
  }

  const auto suffix = trimValue(trimmed.substr(static_cast<size_t>(numberEnd - trimmed.c_str())));
  const auto parsedUnit = parseUnit(suffix);
  if (parsedUnit.type == PhysicalValueType::Unknown) {
    return std::nullopt;
  }

  return PhysicalValue{number * parsedUnit.multiplier, parsedUnit.type};
}

bool physicalValueMatches(const std::string& a, const std::string& b, double tolerance) {
  if (tolerance < 0.0) {
    return false;
  }

  const auto parsedA = parsePhysicalValue(a);
  const auto parsedB = parsePhysicalValue(b);
  if (!parsedA.has_value() || !parsedB.has_value() || parsedA->type != parsedB->type ||
      parsedA->type == PhysicalValueType::Unknown) {
    return false;
  }

  const double largest = std::max(std::abs(parsedA->value), std::abs(parsedB->value));
  const double difference = std::abs(parsedA->value - parsedB->value);
  if (largest == 0.0) {
    return true;
  }
  return difference / largest <= tolerance;
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
    case PhysicalValueType::Unknown: return 0.01;
  }
  return 0.01;
}

PhysicalValueType parameterNameToType(const std::string& name) {
  string lower = trimValue(name);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
    return static_cast<char>(tolower(character));
  });

  const auto paren = lower.find('(');
  if (paren != string::npos) {
    lower = trimValue(lower.substr(0, paren));
  }

  if (lower == "resistance" || lower == "res" || lower == "r" || lower == "impedance" ||
      lower == "imp" || lower == "dc resistance" || lower == "dcr" || lower == "esr" ||
      lower == "nominal resistance" || lower == "resistance value") {
    return PhysicalValueType::Resistance;
  }
  if (lower == "capacitance" || lower == "cap" || lower == "c" || lower == "nominal capacitance" ||
      lower == "capacitance value" || lower == "rated capacitance") {
    return PhysicalValueType::Capacitance;
  }
  if (lower == "inductance" || lower == "ind" || lower == "l" || lower == "nominal inductance" ||
      lower == "inductance value") {
    return PhysicalValueType::Inductance;
  }
  if (lower == "frequency" || lower == "freq" || lower == "nominal frequency") {
    return PhysicalValueType::Frequency;
  }
  return PhysicalValueType::Unknown;
}

}  // namespace inventatory
