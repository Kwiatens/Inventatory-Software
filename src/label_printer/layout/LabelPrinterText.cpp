// Inventatory - Label text and electrical-value formatting helpers.

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

bool looksLikeFrequencyValue(const string& value) {
  return normalizeKey(value).find("hz") != string::npos;
}

bool looksLikeInductanceValue(const string& value) {
  const auto normalized = normalizeKey(value);
  if (normalized.empty() || looksLikeFrequencyValue(value)) {
    return false;
  }
  if (normalized.find("uh") != string::npos || normalized.find("nh") != string::npos ||
      normalized.find("ph") != string::npos || normalized.find("henry") != string::npos) {
    return true;
  }
  return normalized.find_first_of("0123456789") != string::npos && normalized.back() == 'h';
}

string canonicalInductanceUnit(string unit) {
  transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  if (unit == "uh") {
    return "uH";
  }
  if (unit == "nh") {
    return "nH";
  }
  if (unit == "mh") {
    return "mH";
  }
  if (unit == "ph") {
    return "pH";
  }
  return "H";
}

optional<string> extractInductanceFromText(const string& text) {
  regex valuePattern(R"(\b(\d+(?:\.\d+)?|\d+[rR]\d+)\s*([munp]?h)\b)", regex_constants::icase);
  smatch match;
  if (regex_search(text, match, valuePattern) && match.size() > 2) {
    auto number = match[1].str();
    replace(number.begin(), number.end(), 'R', '.');
    replace(number.begin(), number.end(), 'r', '.');
    return number + canonicalInductanceUnit(match[2].str());
  }
  return nullopt;
}

optional<string> parameterValueMatching(const InventoryItem& item, initializer_list<const char*> names,
                                        bool (*predicate)(const string&)) {
  for (const auto* name : names) {
    for (const auto& parameter : item.parameters) {
      if (!parameterLabelMatches(parameter.name, name)) {
        continue;
      }
      const auto value = trim(parameter.value);
      if (!value.empty() && !looksLikePackagingValue(value) && predicate(value)) {
        return value;
      }
    }
  }
  return nullopt;
}

optional<string> firstParameter(const InventoryItem& item, initializer_list<const char*> names) {
  return parameterValue(item, names);
}

optional<string> firstInductanceParameter(const InventoryItem& item) {
  if (const auto value = parameterValueMatching(item, {"Inductance", "Value"}, looksLikeInductanceValue)) {
    return value;
  }
  return extractInductanceFromText(item.notes + " " + item.partName + " " + item.sku);
}

bool itemTextContains(const InventoryItem& item, initializer_list<const char*> needles) {
  const auto textMatches = [&](const string& text) {
    for (const auto* needle : needles) {
      if (containsInsensitive(text, needle)) {
        return true;
      }
    }
    return false;
  };

  if (textMatches(item.category) || textMatches(displayCategory(item.category)) || textMatches(item.partName) ||
      textMatches(item.manufacturer) || textMatches(item.location) || textMatches(item.notes) ||
      textMatches(item.digikeyPartNumber) || textMatches(item.sku)) {
    return true;
  }

  for (const auto& tag : item.tags) {
    if (textMatches(tag)) {
      return true;
    }
  }

  for (const auto& parameter : item.parameters) {
    if (textMatches(parameter.name) || textMatches(parameter.value)) {
      return true;
    }
  }

  return false;
}

bool hasParameter(const InventoryItem& item, initializer_list<const char*> names) {
  return findParameter(item.parameters, names) != nullptr;
}

}  // namespace label_printer_detail
}  // namespace inventatory
