// Inventatory - Private inventory query evaluation contract.

#pragma once

#include "core/Inventory.h"
#include "core/PhysicalValue.h"

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

QueryMatchResult evaluateQueryWithRack(const InventoryItem& item, const std::string& query,
                                       const std::string& itemRackLocation, int lowStockThreshold);

}  // namespace inventatory::query_detail
