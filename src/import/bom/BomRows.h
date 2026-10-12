// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Row-level helpers shared by every BOM dialect: designator lists and count cells.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

using std::optional;
using std::size_t;
using std::string;
using std::vector;

// A single BOM row may not name more designators than this. Larger rows are rejected as malformed.
constexpr size_t kMaximumDesignatorsPerRow = 10000;

// Splits "C1, C15 C16" on commas, semicolons, spaces and tabs. False when the row exceeds the limit.
bool splitBomDesignators(const string& value, vector<string>& designators);

// A whole, non-negative count such as "6". Nullopt for blank, signed, fractional or overflowing text.
optional<int> parseBomCount(const string& value);

}  // namespace inventatory
