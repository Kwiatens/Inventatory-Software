// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - pure layout rules for the bottom action sheet.
//
// Keeps the cursor row visible and every action reachable on short terminals. No FTXUI or
// App state, so these helpers can be unit tested.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace inventatory::action_sheet {

// Indices grouped by the first appearance of each group, keeping registry order within a group.
inline std::vector<std::size_t> groupedOrder(const std::vector<std::string>& groups) {
  std::vector<std::string> seen;
  for (const auto& group : groups) {
    if (std::find(seen.begin(), seen.end(), group) == seen.end()) seen.push_back(group);
  }
  std::vector<std::size_t> order;
  order.reserve(groups.size());
  for (const auto& group : seen) {
    for (std::size_t index = 0; index < groups.size(); ++index) {
      if (groups[index] == group) order.push_back(index);
    }
  }
  return order;
}

struct Row {
  bool header = false;
  std::size_t action = 0;  // for a header: the first action of its group
};

// Rows for groups already in display order; a header starts each run.
inline std::vector<Row> rowsFor(const std::vector<std::string>& orderedGroups) {
  std::vector<Row> rows;
  rows.reserve(orderedGroups.size() * 2);
  for (std::size_t index = 0; index < orderedGroups.size(); ++index) {
    if (index == 0 || orderedGroups[index] != orderedGroups[index - 1]) rows.push_back({true, index});
    rows.push_back({false, index});
  }
  return rows;
}

inline std::size_t rowCountFor(const std::vector<std::string>& orderedGroups) {
  return rowsFor(orderedGroups).size();
}

inline std::size_t rowOfAction(const std::vector<Row>& rows, std::size_t action) {
  for (std::size_t index = 0; index < rows.size(); ++index) {
    if (!rows[index].header && rows[index].action == action) return index;
  }
  return 0;
}

// Frame rows around the sheet (header, dividers, message, sheet title), and page rows
// kept visible, so it reads as a sheet.
inline constexpr int kChromeRows = 4;
inline constexpr int kTitleRows = 2;
inline constexpr int kMinPageRows = 6;
inline constexpr int kMinBodyRows = 5;

// Rows available for action and group-header rows at this terminal height.
inline int maxBodyRows(int terminalRows) {
  return std::max(kMinBodyRows, terminalRows - kChromeRows - kTitleRows - kMinPageRows);
}

struct Window {
  std::size_t first = 0;
  std::size_t count = 0;
};

// The slice of `totalRows` to draw: at most `maxRows` rows, always containing
// `cursorRow`, centred on it when the list does not fit.
inline Window windowFor(std::size_t totalRows, std::size_t cursorRow, std::size_t maxRows) {
  if (maxRows == 0 || totalRows == 0) return {0, 0};
  if (totalRows <= maxRows) return {0, totalRows};
  cursorRow = std::min(cursorRow, totalRows - 1);
  const std::size_t half = maxRows / 2;
  std::size_t first = cursorRow > half ? cursorRow - half : 0;
  first = std::min(first, totalRows - maxRows);
  return {first, maxRows};
}

}  // namespace inventatory::action_sheet
