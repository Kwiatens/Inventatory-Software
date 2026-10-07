// Inventatory - Hardware Inventory Management System
// Physical value parsing and comparison for component parameters.

#include "core/parts/PhysicalValue.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale>
#include <regex>
#include <sstream>
#include <string>

namespace inventatory {

namespace {

using std::string;

struct PhysicalValueBandLimits {
  double workable;
  double possible;
};

constexpr std::array<PhysicalValueBandLimits, 5> kPhysicalValueBandLimits = {{
    {0.10, 0.33},  // Resistance
    {0.10, 0.33},  // Capacitance
    {0.10, 0.33},  // Inductance
    {0.10, 0.33},  // Frequency
    {0.0, 0.0},    // Unknown
}};

constexpr PhysicalValueBandLimits physicalValueBandLimits(PhysicalValueType type) {
  const auto index = static_cast<size_t>(type);
  return index < kPhysicalValueBandLimits.size() ? kPhysicalValueBandLimits[index]
                                                 : PhysicalValueBandLimits{0.0, 0.0};
}

// Converts a plain decimal literal without consulting the process locale. The
// application calls setlocale(LC_ALL, ""), and strtod would then read "0.1" as
// "0" under locales that use a comma as the decimal separator.
//
// A literal that overflows (or is not a number at all) has no value; returning 0 there would make
// "1e999 k" compare as an exact 0-ohm match.
std::optional<double> parseClassicDouble(const string& literal) {
  std::istringstream stream(literal);
  stream.imbue(std::locale::classic());
  double value = 0.0;
  stream >> value;
  if (stream.fail() || !std::isfinite(value)) return std::nullopt;
  return value;
}

// Scans a leading signed decimal number (digits, optional fraction, optional
// exponent) and returns the number of characters consumed, or 0 if there is none.
size_t scanLeadingNumber(const string& text, double& value) {
  size_t index = 0;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) ++index;
  const auto digitsFrom = [&](size_t from) {
    size_t end = from;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
    return end;
  };
  const auto integerEnd = digitsFrom(index);
  size_t end = integerEnd;
  size_t fractionDigits = 0;
  if (end < text.size() && text[end] == '.') {
    const auto fractionEnd = digitsFrom(end + 1);
    fractionDigits = fractionEnd - (end + 1);
    if (fractionDigits > 0 || integerEnd > index) end = fractionEnd;
  }
  if (integerEnd == index && fractionDigits == 0) return 0;
  if (end < text.size() && (text[end] == 'e' || text[end] == 'E')) {
    size_t exponent = end + 1;
    if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) ++exponent;
    const auto exponentEnd = digitsFrom(exponent);
    if (exponentEnd > exponent) end = exponentEnd;
  }
  const auto parsed = parseClassicDouble(text.substr(0, end));
  if (!parsed.has_value()) return 0;
  value = *parsed;
  return end;
}

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

// The unit must be a whole word ("ohm", "F", "Hz"), not just start with the right letter: "10 hours"
// is not an inductance and "5 ft" is not a capacitance. Anything after the word (a tolerance such
// as "1%", a rating) is ignored.
PhysicalValueType unitType(const string& unit) {
  if (unit.empty()) {
    return PhysicalValueType::Unknown;
  }

  static constexpr char kOmega[] = "\xCE\xA9";
  if (unit.rfind(kOmega, 0) == 0) {
    const bool wordEnds = unit.size() == 2 || isalpha(static_cast<unsigned char>(unit[2])) == 0;
    return wordEnds ? PhysicalValueType::Resistance : PhysicalValueType::Unknown;
  }

  size_t length = 0;
  while (length < unit.size() && isalpha(static_cast<unsigned char>(unit[length])) != 0) ++length;
  string word = unit.substr(0, length);
  std::transform(word.begin(), word.end(), word.begin(),
                 [](unsigned char character) { return static_cast<char>(tolower(character)); });
  if (word == "ohm" || word == "ohms" || word == "r") return PhysicalValueType::Resistance;
  if (word == "hz" || word == "hertz") return PhysicalValueType::Frequency;
  if (word == "f" || word == "farad" || word == "farads") return PhysicalValueType::Capacitance;
  if (word == "h" || word == "henry" || word == "henrys" || word == "henries") {
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
  // Only k/K/M/G/g are resistance markers in RKM position. p/n/u/m are
  // ambiguous (4u7 may be 4.7 uF or 4.7 uH) and are left to the caller.
  const bool resistancePrefixMarker = marker == 'k' || marker == 'K' || marker == 'M' || marker == 'g' ||
                                      marker == 'G';
  if (!resistanceMarker && !resistancePrefixMarker) {
    return std::nullopt;
  }
  if (resistancePrefixMarker) {
    parsePrefix(marker, multiplier);
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
  // Only R may omit the head (R280); M3, K4, G1 are not resistor values.
  if (head.empty() && !resistanceMarker) {
    return std::nullopt;
  }

  double integerPart = 0.0;
  double fractionalPart = 0.0;
  if (!head.empty()) {
    const auto parsedHead = parseClassicDouble(head);
    if (!parsedHead.has_value()) return std::nullopt;
    integerPart = *parsedHead;
  }
  if (!tail.empty()) {
    const auto parsedTail = parseClassicDouble(tail);
    if (!parsedTail.has_value()) return std::nullopt;
    fractionalPart = *parsedTail;
    for (size_t index = 0; index < tail.size(); ++index) {
      fractionalPart /= 10.0;
    }
  }

  const auto value = integerPart + fractionalPart;
  const auto scaled = resistanceMarker ? value : value * multiplier;
  if (!std::isfinite(scaled)) return std::nullopt;
  return PhysicalValue{scaled, PhysicalValueType::Resistance};
}

}  // namespace

std::optional<PhysicalValue> parsePhysicalValue(const std::string& text) {
  auto trimmed = trimValue(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  // Datasheets often carry U+2126 OHM SIGN, which is canonically the same letter as the Greek
  // capital omega the unit parser knows.
  static constexpr char kOhmSign[] = "\xE2\x84\xA6";
  static constexpr char kGreekOmega[] = "\xCE\xA9";
  for (auto at = trimmed.find(kOhmSign); at != string::npos; at = trimmed.find(kOhmSign, at + 2)) {
    trimmed.replace(at, 3, kGreekOmega);
  }

  if (const auto rkm = parseRkmValue(trimmed); rkm.has_value()) {
    return rkm;
  }

  // Handles signs, .5, and scientific notation while leaving the unit suffix
  // for the component-specific parser below.
  double number = 0.0;
  const auto numberLength = scanLeadingNumber(trimmed, number);
  if (numberLength == 0) {
    return std::nullopt;
  }

  const auto suffix = trimValue(trimmed.substr(numberLength));
  const auto parsedUnit = parseUnit(suffix);
  if (parsedUnit.type == PhysicalValueType::Unknown) {
    return std::nullopt;
  }

  const auto scaled = number * parsedUnit.multiplier;
  if (!std::isfinite(scaled)) return std::nullopt;
  return PhysicalValue{scaled, parsedUnit.type};
}

std::optional<PhysicalValueComparison> comparePhysicalValues(const std::string& candidate,
                                                             const std::string& target) {
  const auto parsedCandidate = parsePhysicalValue(candidate);
  const auto parsedTarget = parsePhysicalValue(target);
  if (!parsedCandidate.has_value() || !parsedTarget.has_value() ||
      parsedCandidate->type != parsedTarget->type || parsedCandidate->type == PhysicalValueType::Unknown) {
    return std::nullopt;
  }

  const double targetMagnitude = std::abs(parsedTarget->value);
  const double difference = std::abs(parsedCandidate->value - parsedTarget->value);
  const double relativeDifference = targetMagnitude == 0.0
                                        ? (difference == 0.0 ? 0.0 : std::numeric_limits<double>::infinity())
                                        : difference / targetMagnitude;
  const double signedRelativeDifference = targetMagnitude == 0.0
                                              ? (difference == 0.0 ? 0.0 : std::numeric_limits<double>::infinity())
                                              : (parsedCandidate->value - parsedTarget->value) / targetMagnitude;
  const auto limits = physicalValueBandLimits(parsedTarget->type);
  constexpr double kComparisonEpsilon = 1e-9;

  PhysicalValueMatchBand band = PhysicalValueMatchBand::None;
  if (relativeDifference <= kComparisonEpsilon) {
    band = PhysicalValueMatchBand::Exact;
  } else if (relativeDifference <= limits.workable + kComparisonEpsilon) {
    band = PhysicalValueMatchBand::Workable;
  } else if (relativeDifference <= limits.possible + kComparisonEpsilon) {
    band = PhysicalValueMatchBand::Possible;
  }

  return PhysicalValueComparison{parsedTarget->type, parsedTarget->value, parsedCandidate->value,
                                 relativeDifference, signedRelativeDifference, band};
}

const char* physicalValueMatchBandName(PhysicalValueMatchBand band) {
  switch (band) {
    case PhysicalValueMatchBand::None: return "Other";
    case PhysicalValueMatchBand::Exact: return "Exact";
    case PhysicalValueMatchBand::Workable: return "Workable";
    case PhysicalValueMatchBand::Possible: return "Possible";
  }
  return "Other";
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

namespace value_text {

namespace {

string lowercaseAlphanumerics(const string& value) {
  string normalized;
  normalized.reserve(value.size());
  for (const unsigned char ch : value) {
    if (isalnum(ch)) normalized.push_back(static_cast<char>(tolower(ch)));
  }
  return normalized;
}

string canonicalInductanceUnit(string unit) {
  std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  if (unit == "uh") return "uH";
  if (unit == "nh") return "nH";
  if (unit == "mh") return "mH";
  if (unit == "ph") return "pH";
  return "H";
}

}  // namespace

bool looksLikeFrequencyValue(const string& value) {
  return lowercaseAlphanumerics(value).find("hz") != string::npos;
}

bool looksLikeInductanceValue(const string& value) {
  const auto normalized = lowercaseAlphanumerics(value);
  if (normalized.empty() || normalized.find("hz") != string::npos) return false;
  if (normalized.find("uh") != string::npos || normalized.find("nh") != string::npos ||
      normalized.find("ph") != string::npos || normalized.find("henry") != string::npos) {
    return true;
  }
  return normalized.find_first_of("0123456789") != string::npos && normalized.back() == 'h';
}

std::optional<string> extractInductance(const string& text) {
  static const std::regex valuePattern(R"(\b(\d+(?:\.\d+)?|\d+[rR]\d+)\s*([munp]?h)\b)",
                                       std::regex_constants::icase);
  std::smatch match;
  if (!std::regex_search(text, match, valuePattern) || match.size() <= 2) return std::nullopt;
  auto number = match[1].str();
  std::replace(number.begin(), number.end(), 'R', '.');
  std::replace(number.begin(), number.end(), 'r', '.');
  return number + canonicalInductanceUnit(match[2].str());
}

}  // namespace value_text

}  // namespace inventatory
