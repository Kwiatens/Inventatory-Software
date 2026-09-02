// Inventatory - Hardware Inventory Management System
// Core inventory search, filtering, and summary logic.

#include "core/Inventory.h"
#include "core/PhysicalValue.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <unordered_set>

namespace inventatory {

using namespace std;

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

bool tokenMatchesParameterList(const vector<Parameter>& parameters, const string& value,
                               const PhysicalValueTolerances& tolerances) {
  const auto equalsPos = value.find('=');
  const auto needleKey = toLower(equalsPos == string::npos ? value : value.substr(0, equalsPos));
  const auto needleValue = equalsPos == string::npos ? string() : trim(value.substr(equalsPos + 1));
  const auto loweredNeedleValue = toLower(needleValue);

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
      const double tolerance = toleranceForType(tolerances, parsedNeedle->type);
      if (physicalValueMatches(parameter.value, needleValue, tolerance)) {
        return true;
      }
    }

    // Fall back to substring match
    if (parameterValue.find(loweredNeedleValue) != string::npos) {
      return true;
    }
  }

  return false;
}

bool tokenMatchesParameter(const InventoryItem& item, const string& value,
                           const PhysicalValueTolerances& tolerances) {
  return tokenMatchesParameterList(item.parameters, value, tolerances) ||
         tokenMatchesParameterList(item.vendorMetadata.parameters, value, tolerances);
}

bool tokenMatchesParameterPhysicallyList(const vector<Parameter>& parameters, const string& token,
                                         const PhysicalValueTolerances& tolerances) {
  auto parsed = parsePhysicalValue(token);
  if (!parsed.has_value() || parsed->type == PhysicalValueType::Unknown) {
    return false;
  }

  const double tolerance = toleranceForType(tolerances, parsed->type);

  for (const auto& parameter : parameters) {
    auto paramType = parameterNameToType(parameter.name);
    // Only match if the parameter type matches the parsed needle type
    if (paramType != PhysicalValueType::Unknown && paramType != parsed->type) {
      continue;
    }
    if (physicalValueMatches(parameter.value, token, tolerance)) {
      return true;
    }
  }

  return false;
}

bool tokenMatchesParameterPhysically(const InventoryItem& item, const string& token,
                                     const PhysicalValueTolerances& tolerances) {
  return tokenMatchesParameterPhysicallyList(item.parameters, token, tolerances) ||
         tokenMatchesParameterPhysicallyList(item.vendorMetadata.parameters, token, tolerances);
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

bool looksLikeDataError(const InventoryItem& item, bool duplicateId) {
  return duplicateId || item.quantity < 0;
}

}  // namespace

int effectiveReorderThreshold(const InventoryItem& item, int globalThreshold) {
  return item.reorderThreshold > 0 ? item.reorderThreshold : max(0, globalThreshold);
}

bool isLowStock(const InventoryItem& item, int threshold) {
  return effectiveReorderThreshold(item, threshold) > 0 && item.quantity > 0 &&
         item.quantity <= effectiveReorderThreshold(item, threshold);
}

bool matchesQueryWithRack(const InventoryItem& item, const string& query, const string& itemRackLocation,
                          int lowStockThreshold, const PhysicalValueTolerances& tolerances) {
  const auto tokens = tokenizeQuery(query);
  if (tokens.empty()) {
    return true;
  }

  for (const auto& rawToken : tokens) {
    const auto token = toLower(rawToken);

    if (tokenMatchesQuantity(item, token)) {
      continue;
    }

    if (token.rfind("cat:", 0) == 0 || token.rfind("category:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesCategory(item, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("mfg:", 0) == 0 || token.rfind("manufacturer:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.manufacturer, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("name:", 0) == 0 || token.rfind("part:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.partName, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("loc:", 0) == 0 || token.rfind("location:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.location, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("rack:", 0) == 0) {
      const auto value = token.substr(5);
      if (tokenMatchesField(itemRackLocation, value)) continue;
      return false;
    }

    if (token.rfind("sku:", 0) == 0) {
      const auto value = token.substr(4);
      if (tokenMatchesField(item.sku, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("inventatory:", 0) == 0 || token.rfind("inventatoryid:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.inventatoryId, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("dg:", 0) == 0 || token.rfind("digikey:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.digikeyPartNumber, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("tag:", 0) == 0) {
      const auto value = token.substr(4);
      const auto matched = any_of(item.tags.begin(), item.tags.end(), [&](const string& tag) {
        return containsInsensitive(tag, value);
      });
      if (matched) {
        continue;
      }
      return false;
    }

    if (token.rfind("param:", 0) == 0) {
      const auto value = rawToken.substr(6);
      if (tokenMatchesParameter(item, value, tolerances)) {
        continue;
      }
      return false;
    }

    if (token.rfind("status:", 0) == 0) {
      const auto value = token.substr(7);
      if (tokenMatchesStatus(item, value, lowStockThreshold)) {
        continue;
      }
      return false;
    }

    if (containsInsensitive(item.searchableText(), token)) {
      continue;
    }
    if (containsInsensitive(itemRackLocation, token)) continue;

    // Try physical value matching against parameter values
    if (tokenMatchesParameterPhysically(item, rawToken, tolerances)) {
      continue;
    }

    return false;
  }

  return true;
}

bool matchesQuery(const InventoryItem& item, const string& query, int lowStockThreshold) {
  return matchesQuery(item, query, lowStockThreshold, PhysicalValueTolerances{});
}

bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks,
                  int lowStockThreshold) {
  return matchesQuery(item, query, racks, lowStockThreshold, PhysicalValueTolerances{});
}

bool matchesQuery(const InventoryItem& item, const string& query, int lowStockThreshold,
                  const PhysicalValueTolerances& tolerances) {
  return matchesQueryWithRack(item, query, {}, lowStockThreshold, tolerances);
}

bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks,
                  int lowStockThreshold, const PhysicalValueTolerances& tolerances) {
  return matchesQueryWithRack(item, query, rackLocation(item, racks), lowStockThreshold, tolerances);
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, int lowStockThreshold) {
  return filterItems(items, query, lowStockThreshold, PhysicalValueTolerances{});
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, int lowStockThreshold,
                           const PhysicalValueTolerances& tolerances) {
  vector<size_t> indices;
  for (size_t index = 0; index < items.size(); ++index) {
    if (matchesQuery(items[index], query, lowStockThreshold, tolerances)) {
      indices.push_back(index);
    }
  }
  return indices;
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks, int lowStockThreshold) {
  return filterItems(items, query, racks, lowStockThreshold, PhysicalValueTolerances{});
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks, int lowStockThreshold,
                           const PhysicalValueTolerances& tolerances) {
  vector<size_t> indices;
  for (size_t index = 0; index < items.size(); ++index) {
    if (matchesQuery(items[index], query, racks, lowStockThreshold, tolerances)) indices.push_back(index);
  }
  return indices;
}

Summary summarize(const vector<InventoryItem>& items, int lowStockThreshold) {
  Summary summary;
  summary.itemCount = items.size();

  for (const auto& item : items) {
    summary.totalUnits += static_cast<size_t>(max(item.quantity, 0));
    if (isLowStock(item, lowStockThreshold)) {
      ++summary.lowStockCount;
    }
    if (item.hasMissingMetadata()) {
      ++summary.missingMetadataCount;
    }
    if (toLower(item.syncStatus) != "synced") {
      ++summary.unsyncedCount;
    }
  }

  return summary;
}

InventoryHistoryPoint makeInventoryHistoryPoint(const vector<InventoryItem>& items, int lowStockThreshold,
                                                time_t timestamp) {
  InventoryHistoryPoint point;
  point.timestamp = timestamp == 0 ? time(nullptr) : timestamp;
  point.itemCount = items.size();

  unordered_set<string> seenIds;
  for (const auto& item : items) {
    point.totalUnits += static_cast<size_t>(max(item.quantity, 0));
    if (isLowStock(item, lowStockThreshold)) {
      ++point.lowStockCount;
    }
    if (item.quantity <= 0) {
      ++point.outOfStockCount;
    }
    if (looksLikeDataError(item, !seenIds.insert(item.id).second)) {
      ++point.dataErrorCount;
    }
  }

  return point;
}

bool matchesQuery(const InventoryItem& item, const string& query) {
  return matchesQuery(item, query, 5);
}

bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks) {
  return matchesQuery(item, query, racks, 5);
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query) {
  return filterItems(items, query, 5);
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks) {
  return filterItems(items, query, racks, 5);
}

Summary summarize(const vector<InventoryItem>& items) {
  return summarize(items, 5);
}

InventoryHistoryPoint makeInventoryHistoryPoint(const vector<InventoryItem>& items, time_t timestamp) {
  return makeInventoryHistoryPoint(items, 5, timestamp);
}

void appendInventoryHistory(vector<InventoryHistoryPoint>& history, const InventoryHistoryPoint& point,
                            size_t maxEntries) {
  if (!history.empty()) {
    const auto& previous = history.back();
    if (previous.itemCount == point.itemCount && previous.totalUnits == point.totalUnits &&
        previous.lowStockCount == point.lowStockCount && previous.outOfStockCount == point.outOfStockCount &&
        previous.dataErrorCount == point.dataErrorCount) {
      return;
    }
  }

  history.push_back(point);
  if (history.size() > maxEntries) {
    history.erase(history.begin(), history.begin() + static_cast<ptrdiff_t>(history.size() - maxEntries));
  }
}

}  // namespace inventatory
