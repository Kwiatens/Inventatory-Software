// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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

// Compares value-like words in the part name, so "0.1uF Capacitor" matches "100nF".
std::optional<PhysicalValueComparison> partNamePhysicalComparison(const InventoryItem& item,
                                                                  const std::string& target);

// Only value parameters and name words count, and a different component family never matches.
// Comparisons outside the match bands are dropped unless allowOutsideBands is set.
std::optional<PhysicalValueComparison> itemPhysicalComparison(const InventoryItem& item,
                                                              const std::string& target,
                                                              bool allowOutsideBands = false);

QueryMatchResult evaluateQueryWithRack(const InventoryItem& item, const std::string& query,
                                       const std::string& itemRackLocation, int lowStockThreshold);

}  // namespace inventatory::query_detail
