// Inventatory - pure layout rules for the bottom action sheet.
//
// The sheet replaces the context line at the bottom of the frame, so on a
// 30-row terminal it can only be a fraction of the screen tall. These helpers
// group the actions, count the rows and choose the visible window so the
// cursor row is always drawn and every action stays reachable by keyboard and
// mouse. They do not touch FTXUI or App state so they can be unit tested.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace inventatory::action_sheet {

// Order that lists each group once, in order of first appearance, keeping the
// registry order inside a group. Returns indices into the original list.
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

// Rows for groups already in display order: one header per run of equal groups.
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

// Frame chrome that stays on screen around the sheet: header, two dividers and
// the message row, plus the sheet's own title and divider. A few page rows are
// kept visible above the sheet so it reads as a sheet, not a screen takeover.
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
