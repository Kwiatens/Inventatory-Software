// Inventatory - Private inventory query evaluation contract.

#pragma once

#include "core/inventory/Inventory.h"
#include "core/parts/PhysicalValue.h"

#include <optional>
#include <string>

namespace inventatory::query_detail {

struct QueryMatchResult {
  bool matched = false;
  std::optional<PhysicalValueComparison> physical;
};

std::optional<PhysicalValueComparison> bestPhysicalComparison(
    const std::optional<PhysicalValueComparison>& current,
    const std::optional<PhysicalValueComparison>& candidate);

// Compares a physical-value query against value-like words in the part name
// (for example "0.1uF Capacitor"), so items that only carry the value in their
// name still match equivalent spellings such as "100nF".
std::optional<PhysicalValueComparison> partNamePhysicalComparison(const InventoryItem& item,
                                                                  const std::string& target);

QueryMatchResult evaluateQueryWithRack(const InventoryItem& item, const std::string& query,
                                       const std::string& itemRackLocation, int lowStockThreshold);

}  // namespace inventatory::query_detail
