// Inventatory - BOM matching helpers.

#include "BomMatchPrivate.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace inventatory::bom_match_detail {

using namespace std;

vector<string> splitOnUnderscore(const string& value) {
  vector<string> tokens;
  string current;
  for (const char ch : value) {
    if (ch == '_') {
      if (!current.empty()) {
        tokens.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

bool startsWithInsensitive(const string& value, const string& prefix) {
  return value.size() >= prefix.size() && toLower(value.substr(0, prefix.size())) == toLower(prefix);
}

// Pulls "5.0" out of a "D5.0mm" diameter token.
string diameterToken(const vector<string>& tokens) {
  for (const auto& token : tokens) {
    if (token.size() > 2 && (token[0] == 'D' || token[0] == 'd') &&
        toLower(token).substr(token.size() - 2) == "mm") {
      return token.substr(1, token.size() - 3);  // drop the leading D and trailing mm
    }
  }
  return {};
}

// Pulls the pin count out of a "1x03" grid token.
string pinCountToken(const vector<string>& tokens) {
  for (const auto& token : tokens) {
    const auto lowered = toLower(token);
    const auto cross = lowered.find('x');
    if (cross == string::npos || cross == 0 || cross + 1 >= lowered.size()) {
      continue;
    }
    const auto rows = lowered.substr(0, cross);
    const auto columns = lowered.substr(cross + 1);
    if (!all_of(rows.begin(), rows.end(), [](unsigned char ch) { return isdigit(ch) != 0; }) ||
        !all_of(columns.begin(), columns.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
      continue;
    }
    unsigned long long rowCount = 0;
    unsigned long long columnCount = 0;
    for (const unsigned char ch : rows) {
      const auto digit = static_cast<unsigned long long>(ch - '0');
      if (rowCount > (numeric_limits<unsigned long long>::max() - digit) / 10U) return {};
      rowCount = rowCount * 10U + digit;
    }
    for (const unsigned char ch : columns) {
      const auto digit = static_cast<unsigned long long>(ch - '0');
      if (columnCount > (numeric_limits<unsigned long long>::max() - digit) / 10U) return {};
      columnCount = columnCount * 10U + digit;
    }
    if (rowCount == 0 || columnCount == 0 || rowCount > numeric_limits<unsigned long long>::max() / columnCount) {
      return {};
    }
    const auto pinCount = rowCount * columnCount;
    if (pinCount > static_cast<unsigned long long>(numeric_limits<int>::max())) return {};
    return to_string(pinCount);
  }
  return {};
}

// Splits a multiplier letter off the numeric body, honouring RKM notation where
// the letter also stands in for the decimal point ("4R7", "1u5").
optional<double> parseNumberWithMultiplier(const string& body, bool resistanceLike) {
  if (body.empty()) {
    return nullopt;
  }

  size_t alpha = string::npos;
  for (size_t index = 0; index < body.size(); ++index) {
    if (isalpha(static_cast<unsigned char>(body[index])) != 0) {
      if (alpha != string::npos) {
        return nullopt;  // more than one letter is not a value
      }
      alpha = index;
    } else if (isdigit(static_cast<unsigned char>(body[index])) == 0 && body[index] != '.') {
      return nullopt;
    }
  }

  string numberText = body;
  double multiplier = 1.0;

  if (alpha != string::npos) {
    const auto letter = static_cast<char>(tolower(static_cast<unsigned char>(body[alpha])));
    const auto head = body.substr(0, alpha);
    const auto tail = body.substr(alpha + 1);

    switch (letter) {
      case 'p':
        multiplier = 1e-12;
        break;
      case 'n':
        multiplier = 1e-9;
        break;
      case 'u':
        multiplier = 1e-6;
        break;
      case 'm':
        // For resistors a bare M means megaohm; for reactive units it is milli.
        multiplier = resistanceLike ? 1e6 : 1e-3;
        break;
      case 'k':
        multiplier = 1e3;
        break;
      case 'g':
        multiplier = 1e9;
        break;
      case 'r':
      case 'e':
        multiplier = 1.0;
        break;
      default:
        return nullopt;
    }
    // An uppercase M is always mega, whatever the unit.
    if (body[alpha] == 'M') {
      multiplier = 1e6;
    }

    if (!tail.empty()) {
      numberText = (head.empty() ? string("0") : head) + "." + tail;
    } else {
      numberText = head;
    }
  }

  if (numberText.empty()) {
    return nullopt;
  }

  try {
    size_t consumed = 0;
    const double number = stod(numberText, &consumed);
    if (consumed != numberText.size()) {
      return nullopt;
    }
    const double result = number * multiplier;
    if (!isfinite(result) || result < 0.0) return nullopt;
    return result;
  } catch (...) {
    return nullopt;
  }
}

bool endsWith(const string& value, const string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

string compactKey(const string& value) {
  string compact;
  compact.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      compact.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return compact;
}

// The item-side value for a kind, taken from DigiKey parameters when present
// and otherwise recovered from the description text.
optional<double> itemValueFor(const InventoryItem& item, ValueKind kind) {
  // The initializer lists stay inline: binding one to a named variable would
  // leave the backing array dangling past the full expression.
  optional<string> text;
  switch (kind) {
    case ValueKind::Capacitance:
      text = parameterValue(item, {"Capacitance", "Value"});
      break;
    case ValueKind::Resistance:
      text = parameterValue(item, {"Resistance", "Value"});
      break;
    case ValueKind::Inductance:
      text = parameterValue(item, {"Inductance", "Value"});
      break;
    case ValueKind::Frequency:
      text = parameterValue(item, {"Frequency", "Value"});
      break;
    case ValueKind::None:
      return nullopt;
  }

  if (text) {
    ValueKind parsedKind = ValueKind::None;
    if (const auto value = parseElectricalValue(*text, parsedKind); value && parsedKind == kind) {
      return value;
    }
  }

  // DigiKey descriptions such as "CAP CER 1UF 25V X7R 0603" carry the value in
  // free text, which covers items that were never enriched.
  for (const auto& token : tokenizeQuery(item.partName + " " + item.notes)) {
    ValueKind parsedKind = ValueKind::None;
    if (const auto value = parseElectricalValue(token, parsedKind); value && parsedKind == kind) {
      return value;
    }
  }
  return nullopt;
}

optional<string> itemPackage(const InventoryItem& item) {
  if (const auto package = parameterValue(item, {"Package / Case", "Package Case", "Case / Package",
                                                 "Case Package", "Supplier Device Package", "Device Package",
                                                 "Package"})) {
    return package;
  }
  // Fall back to a chip code sitting in the description.
  const auto compact = compactKey(item.partName);
  for (const auto* code : kChipCodes) {
    if (compact.find(code) != string::npos) {
      return string(code);
    }
  }
  return nullopt;
}

bool sameText(const string& lhs, const string& rhs) {
  return !trim(lhs).empty() && toLower(trim(lhs)) == toLower(trim(rhs));
}

bool partNameHasToken(const InventoryItem& item, const string& designation) {
  const auto needle = compactKey(designation);
  if (needle.size() < 4) {
    return false;
  }
  return compactKey(item.partName + " " + item.notes + " " + item.sku).find(needle) != string::npos;
}

int scoreItem(const InventoryItem& item, const BomLine& line, const string& bomPackage,
              optional<double> bomValue, ValueKind bomKind) {
  if (sameText(line.designation, item.sku) || sameText(line.designation, item.digikeyPartNumber)) {
    return 100;
  }
  if (looksLikePartNumber(line.designation) && partNameHasToken(item, line.designation)) {
    return 85;
  }

  if (!bomValue) {
    return 0;
  }

  const auto candidateValue = itemValueFor(item, bomKind);
  if (!candidateValue) {
    return 0;
  }
  const double reference = max(fabs(*bomValue), 1e-18);
  if (fabs(*candidateValue - *bomValue) / reference > 0.01) {
    return 0;
  }

  const auto package = itemPackage(item);
  if (bomPackage.empty() || !package) {
    return 70;
  }
  return packageMatches(bomPackage, *package) ? 90 : 30;
}

int saturatingAdd(int lhs, int rhs) {
  if (lhs < 0 || rhs < 0) return numeric_limits<int>::min();
  if (lhs > numeric_limits<int>::max() - rhs) return numeric_limits<int>::max();
  return lhs + rhs;
}

int saturatingMultiplyNonNegative(int lhs, int rhs) {
  if (lhs <= 0 || rhs <= 0) return 0;
  if (lhs > numeric_limits<int>::max() / rhs) return numeric_limits<int>::max();
  return lhs * rhs;
}

}  // namespace inventatory::bom_match_detail
