// HIMS - Hardware Inventory Management System
// Core inventory search, filtering, and summary logic.

#include "core/Inventory.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <unordered_set>

namespace hims {

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

bool tokenMatchesParameter(const InventoryItem& item, const string& value) {
  const auto equalsPos = value.find('=');
  const auto needleKey = toLower(equalsPos == string::npos ? value : value.substr(0, equalsPos));
  const auto needleValue = equalsPos == string::npos ? string() : toLower(value.substr(equalsPos + 1));

  for (const auto& parameter : item.parameters) {
    const auto key = toLower(parameter.name);
    const auto parameterValue = toLower(parameter.value);

    if (!needleKey.empty() && key.find(needleKey) == string::npos) {
      continue;
    }

    if (needleValue.empty() || parameterValue.find(needleValue) != string::npos) {
      return true;
    }
  }

  return false;
}

bool tokenMatchesQuantity(const InventoryItem& item, const string& token) {
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
  return false;
}

bool tokenMatchesStatus(const InventoryItem& item, const string& value) {
  const auto lowerValue = toLower(value);
  if (lowerValue == "low") {
    return item.lowStock();
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
  return duplicateId || item.quantity < 0 || item.reorderThreshold < 0;
}

struct ThresholdRule {
  const char* needle;
  int threshold;
};

constexpr ThresholdRule kCategoryThresholdRules[] = {
    {"resistor", 20},
    {"capacitor", 20},
    {"led", 20},
    {"indicator", 20},
    {"connector", 10},
    {"ic", 5},
    {"integrated circuit", 5},
    {"microcontroller", 3},
    {"mcu", 3},
    {"sensor", 3},
    {"module", 3},
    {"pcb", 2},
    {"mechanical", 10},
    {"switch", 10},
    {"relay", 5},
};

}  // namespace

bool matchesQueryWithRack(const InventoryItem& item, const string& query, const string& itemRackLocation) {
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

    if (token.rfind("hims:", 0) == 0 || token.rfind("himsid:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.himsId, value)) {
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
      const auto value = token.substr(6);
      if (tokenMatchesParameter(item, value)) {
        continue;
      }
      return false;
    }

    if (token.rfind("status:", 0) == 0) {
      const auto value = token.substr(7);
      if (tokenMatchesStatus(item, value)) {
        continue;
      }
      return false;
    }

    if (containsInsensitive(item.searchableText(), token)) {
      continue;
    }
    if (containsInsensitive(itemRackLocation, token)) continue;

    return false;
  }

  return true;
}

bool matchesQuery(const InventoryItem& item, const string& query) {
  return matchesQueryWithRack(item, query, {});
}

bool matchesQuery(const InventoryItem& item, const string& query, const vector<HimsRack>& racks) {
  return matchesQueryWithRack(item, query, rackLocation(item, racks));
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query) {
  vector<size_t> indices;
  for (size_t index = 0; index < items.size(); ++index) {
    if (matchesQuery(items[index], query)) {
      indices.push_back(index);
    }
  }
  return indices;
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, const vector<HimsRack>& racks) {
  vector<size_t> indices;
  for (size_t index = 0; index < items.size(); ++index) {
    if (matchesQuery(items[index], query, racks)) indices.push_back(index);
  }
  return indices;
}

Summary summarize(const vector<InventoryItem>& items) {
  Summary summary;
  summary.itemCount = items.size();

  for (const auto& item : items) {
    summary.totalUnits += static_cast<size_t>(max(item.quantity, 0));
    if (item.lowStock()) {
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

int categoryLowStockThreshold(const string& category) {
  const auto lowered = toLower(trim(category));
  for (const auto& rule : kCategoryThresholdRules) {
    if (containsInsensitive(lowered, rule.needle)) {
      return rule.threshold;
    }
  }
  return 5;
}

bool lowStockByCategory(const InventoryItem& item) {
  return item.quantity <= categoryLowStockThreshold(item.category);
}

InventoryHistoryPoint makeInventoryHistoryPoint(const vector<InventoryItem>& items, time_t timestamp) {
  InventoryHistoryPoint point;
  point.timestamp = timestamp == 0 ? time(nullptr) : timestamp;
  point.itemCount = items.size();

  unordered_set<string> seenIds;
  for (const auto& item : items) {
    point.totalUnits += static_cast<size_t>(max(item.quantity, 0));
    if (lowStockByCategory(item)) {
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

}  // namespace hims
