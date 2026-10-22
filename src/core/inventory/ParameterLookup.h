// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/inventory/Inventory.h"

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

// Name-based lookup of part parameters, shared by BOM matching and the item-detail view.
std::string normalizeKey(std::string value);
bool parameterLabelMatches(const std::string& lhs, const std::string& rhs);
bool looksLikePackagingValue(const std::string& value);
const Parameter* findParameter(const std::vector<Parameter>& parameters, std::initializer_list<const char*> names);
std::optional<std::string> parameterValue(const InventoryItem& item, std::initializer_list<const char*> names);

}  // namespace inventatory
