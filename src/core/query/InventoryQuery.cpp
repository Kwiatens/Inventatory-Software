// Inventatory - Hardware Inventory Management System
// Core inventory search, filtering, and summary logic.

#include "core/inventory/Inventory.h"
#include "core/query/InventoryQueryPrivate.h"
#include "core/parts/PhysicalValue.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <unordered_set>

namespace inventatory {

using namespace std;

using namespace query_detail;

namespace {
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
                          int lowStockThreshold) {
  return evaluateQueryWithRack(item, query, itemRackLocation, lowStockThreshold).matched;
}

bool matchesQuery(const InventoryItem& item, const string& query, int lowStockThreshold) {
  return matchesQueryWithRack(item, query, {}, lowStockThreshold);
}

bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks,
                  int lowStockThreshold) {
  return matchesQueryWithRack(item, query, rackLocation(item, racks), lowStockThreshold);
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, int lowStockThreshold) {
  vector<size_t> indices;
  const vector<InventatoryRack> racks;
  for (const auto& match : rankedFilterItems(items, query, racks, lowStockThreshold)) indices.push_back(match.itemIndex);
  return indices;
}

vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks, int lowStockThreshold) {
  vector<size_t> indices;
  for (const auto& match : rankedFilterItems(items, query, racks, lowStockThreshold)) indices.push_back(match.itemIndex);
  return indices;
}

vector<InventorySearchMatch> rankedFilterItems(const vector<InventoryItem>& items, const string& query,
                                               const vector<InventatoryRack>& racks, int lowStockThreshold) {
  vector<InventorySearchMatch> matches;
  matches.reserve(items.size());
  for (size_t index = 0; index < items.size(); ++index) {
    const auto result = evaluateQueryWithRack(items[index], query, rackLocation(items[index], racks), lowStockThreshold);
    if (!result.matched) continue;
    InventorySearchMatch match;
    match.itemIndex = index;
    if (result.physical.has_value()) {
      match.band = result.physical->band;
      match.relativeDifference = result.physical->relativeDifference;
      match.signedRelativeDifference = result.physical->signedRelativeDifference;
      match.hasPhysicalComparison = true;
    }
    matches.push_back(match);
  }
  return matches;
}

vector<InventorySearchMatch> findClosestPhysicalValues(const vector<InventoryItem>& items, const string& target) {
  const auto parsedTarget = parsePhysicalValue(target);
  if (!parsedTarget.has_value() || parsedTarget->type == PhysicalValueType::Unknown) return {};

  vector<InventorySearchMatch> matches;
  matches.reserve(items.size());
  for (size_t index = 0; index < items.size(); ++index) {
    optional<PhysicalValueComparison> best;
    const auto consider = [&](const vector<Parameter>& parameters) {
      for (const auto& parameter : parameters) {
        const auto parameterType = parameterNameToType(parameter.name);
        if (parameterType != PhysicalValueType::Unknown && parameterType != parsedTarget->type) continue;
        best = bestPhysicalComparison(best, comparePhysicalValues(parameter.value, target));
      }
    };
    consider(items[index].parameters);
    consider(items[index].vendorMetadata.parameters);
    if (!best.has_value()) continue;

    matches.push_back({index, best->band, best->relativeDifference, best->signedRelativeDifference, true});
  }

  sort(matches.begin(), matches.end(), [&](const InventorySearchMatch& lhs, const InventorySearchMatch& rhs) {
    if (lhs.relativeDifference != rhs.relativeDifference) return lhs.relativeDifference < rhs.relativeDifference;
    if (items[lhs.itemIndex].quantity != items[rhs.itemIndex].quantity) {
      return items[lhs.itemIndex].quantity > items[rhs.itemIndex].quantity;
    }
    return items[lhs.itemIndex].id < items[rhs.itemIndex].id;
  });
  return matches;
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
