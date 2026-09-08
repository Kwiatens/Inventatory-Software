// Inventatory - Inventory query token matching and evaluation.

#include "core/InventoryQueryPrivate.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <unordered_set>

namespace inventatory::query_detail {

using namespace std;

optional<PhysicalValueComparison> bestPhysicalComparison(const optional<PhysicalValueComparison>& current,
                                                         const optional<PhysicalValueComparison>& candidate) {
  if (!candidate.has_value()) return current;
  if (!current.has_value() || candidate->relativeDifference < current->relativeDifference) return candidate;
  return current;
}

namespace {


vector<string> splitTokensRespectingQuotes(const string& query) {
  vector<string> tokens;
  string current;
  bool inQuotes = false;

  for (char ch : query) {
    if (ch == '"') {
      inQuotes = !inQuotes;
      continue;
    }

    if (!inQuotes && isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }

    current.push_back(ch);
  }

  if (!current.empty()) {
    tokens.push_back(current);
  }

  return tokens;
}

struct TokenMatchResult {
  bool matched = false;
  optional<PhysicalValueComparison> physical;
};

TokenMatchResult tokenMatchesParameterList(const vector<Parameter>& parameters, const string& value) {
  const auto equalsPos = value.find('=');
  const auto needleKey = toLower(equalsPos == string::npos ? value : value.substr(0, equalsPos));
  const auto needleValue = equalsPos == string::npos ? string() : trim(value.substr(equalsPos + 1));
  const auto loweredNeedleValue = toLower(needleValue);
  TokenMatchResult result;

  for (const auto& parameter : parameters) {
    const auto key = toLower(parameter.name);
    const auto parameterValue = toLower(parameter.value);

    if (!needleKey.empty() && key.find(needleKey) == string::npos) {
      continue;
    }

    if (needleValue.empty()) {
      continue;
    }

    // Try physical value matching first when the value looks like a physical quantity
    auto parsedNeedle = parsePhysicalValue(needleValue);
    if (parsedNeedle.has_value() && parsedNeedle->type != PhysicalValueType::Unknown) {
      // If a key was specified (e.g. "param:Capacitance=0.1uF"), only match
      // parameters whose name maps to the same physical type.
      if (!needleKey.empty()) {
        auto paramType = parameterNameToType(parameter.name);
        if (paramType != PhysicalValueType::Unknown && paramType != parsedNeedle->type) {
          continue;
        }
      }
      const auto comparison = comparePhysicalValues(parameter.value, needleValue);
      if (comparison.has_value() && comparison->band != PhysicalValueMatchBand::None) {
        result.matched = true;
        result.physical = bestPhysicalComparison(result.physical, comparison);
      }
    }

    // Fall back to substring match
    if (parameterValue.find(loweredNeedleValue) != string::npos) {
      result.matched = true;
    }
  }

  return result;
}

TokenMatchResult tokenMatchesParameter(const InventoryItem& item, const string& value) {
  const auto primary = tokenMatchesParameterList(item.parameters, value);
  const auto vendor = tokenMatchesParameterList(item.vendorMetadata.parameters, value);
  return {primary.matched || vendor.matched, bestPhysicalComparison(primary.physical, vendor.physical)};
}

optional<PhysicalValueComparison> tokenMatchesParameterPhysicallyList(const vector<Parameter>& parameters,
                                                                      const string& token,
                                                                      bool allowOutsideBands = false) {
  const auto parsed = parsePhysicalValue(token);
  if (!parsed.has_value() || parsed->type == PhysicalValueType::Unknown) {
    return nullopt;
  }

  optional<PhysicalValueComparison> best;

  for (const auto& parameter : parameters) {
    auto paramType = parameterNameToType(parameter.name);
    // Only match if the parameter type matches the parsed needle type
    if (paramType != PhysicalValueType::Unknown && paramType != parsed->type) {
      continue;
    }
    const auto comparison = comparePhysicalValues(parameter.value, token);
    if (comparison.has_value() &&
        (allowOutsideBands || comparison->band != PhysicalValueMatchBand::None)) {
      best = bestPhysicalComparison(best, comparison);
    }
  }

  return best;
}

optional<PhysicalValueComparison> tokenMatchesParameterPhysically(const InventoryItem& item, const string& token,
                                                                  bool allowOutsideBands = false) {
  return bestPhysicalComparison(tokenMatchesParameterPhysicallyList(item.parameters, token, allowOutsideBands),
                                tokenMatchesParameterPhysicallyList(item.vendorMetadata.parameters, token,
                                                                    allowOutsideBands));
}

bool tokenMatchesQuantity(const InventoryItem& item, const string& token) {
  try {
    if (token.rfind("qty>=", 0) == 0) {
      return item.quantity >= stoi(token.substr(5));
    }
    if (token.rfind("qty<=", 0) == 0) {
      return item.quantity <= stoi(token.substr(5));
    }
    if (token.rfind("qty>", 0) == 0) {
      return item.quantity > stoi(token.substr(4));
    }
    if (token.rfind("qty<", 0) == 0) {
      return item.quantity < stoi(token.substr(4));
    }
    if (token.rfind("qty=", 0) == 0) {
      return item.quantity == stoi(token.substr(4));
    }
  } catch (const invalid_argument&) {
    return false;
  } catch (const out_of_range&) {
    return false;
  }
  return false;
}

bool tokenMatchesStatus(const InventoryItem& item, const string& value, int lowStockThreshold) {
  const auto lowerValue = toLower(value);
  if (lowerValue == "low") {
    return isLowStock(item, lowStockThreshold);
  }
  if (lowerValue == "missing") {
    return item.hasMissingMetadata();
  }
  if (lowerValue == "synced") {
    return toLower(item.syncStatus) == "synced";
  }
  if (lowerValue == "unsynced") {
    return toLower(item.syncStatus) != "synced";
  }
  return containsInsensitive(item.syncStatus, lowerValue);
}

bool tokenMatchesCategory(const InventoryItem& item, const string& value) {
  return containsInsensitive(item.category, value);
}

bool tokenMatchesField(const string& field, const string& value) {
  return containsInsensitive(field, value);
}

}  // namespace

QueryMatchResult evaluateQueryWithRack(const InventoryItem& item, const string& query, const string& itemRackLocation,
                                       int lowStockThreshold) {
  const auto tokens = tokenizeQuery(query);
  if (tokens.empty()) return {true, nullopt};

  optional<PhysicalValueComparison> bestPhysical;
  for (const auto& rawToken : tokens) {
    const auto token = toLower(rawToken);
    if (tokenMatchesQuantity(item, token)) continue;

    if (token.rfind("cat:", 0) == 0 || token.rfind("category:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesCategory(item, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("mfg:", 0) == 0 || token.rfind("manufacturer:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.manufacturer, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("name:", 0) == 0 || token.rfind("part:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.partName, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("loc:", 0) == 0 || token.rfind("location:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.location, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("rack:", 0) == 0) {
      const auto value = token.substr(5);
      if (tokenMatchesField(itemRackLocation, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("sku:", 0) == 0) {
      const auto value = token.substr(4);
      if (tokenMatchesField(item.sku, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("inventatory:", 0) == 0 || token.rfind("inventatoryid:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.inventatoryId, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("dg:", 0) == 0 || token.rfind("digikey:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.digikeyPartNumber, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("tag:", 0) == 0) {
      const auto value = token.substr(4);
      const auto matched = any_of(item.tags.begin(), item.tags.end(), [&](const string& tag) {
        return containsInsensitive(tag, value);
      });
      if (matched) continue;
      return {false, nullopt};
    }
    if (token.rfind("param:", 0) == 0) {
      const auto parameterMatch = tokenMatchesParameter(item, rawToken.substr(6));
      if (parameterMatch.matched) {
        bestPhysical = bestPhysicalComparison(bestPhysical, parameterMatch.physical);
        continue;
      }
      return {false, nullopt};
    }
    if (token.rfind("status:", 0) == 0) {
      if (tokenMatchesStatus(item, token.substr(7), lowStockThreshold)) continue;
      return {false, nullopt};
    }

    // Prefer physical comparison for parseable values so normalized matches
    // receive a rank even when the raw spelling also appears in the item text.
    const auto physicalMatch = tokenMatchesParameterPhysically(item, rawToken);
    if (physicalMatch.has_value()) {
      bestPhysical = bestPhysicalComparison(bestPhysical, physicalMatch);
      continue;
    }
    if (containsInsensitive(item.searchableText(), token)) continue;
    if (containsInsensitive(itemRackLocation, token)) continue;
    return {false, nullopt};
  }

  return {true, bestPhysical};
}

}  // namespace inventatory::query_detail
