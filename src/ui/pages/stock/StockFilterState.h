// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace inventatory {

enum class StockDateFilter { All, Today, Last7Days, Last30Days, OlderThan30Days };
enum class StockSortOrder { Az, Quantity, Za };

// Panel rows and options are addressed by index, so keyboard focus and mouse share one model.
constexpr int kStockFilterRowCount = 3;
constexpr int kStockSortOptionCount = 3;
constexpr int kStockDateFilterOptionCount = 5;

inline int stockFilterOptionCount(int row) {
  switch (row) {
    case 0: return kStockSortOptionCount;
    case 1: return kStockDateFilterOptionCount;
    default: return 1;  // Reset filters
  }
}

inline int clampStockFilterOption(int row, int option) {
  return std::clamp(option, 0, stockFilterOptionCount(row) - 1);
}

inline StockSortOrder stockSortOrderAt(int option) {
  switch (option) {
    case 0: return StockSortOrder::Az;
    case 1: return StockSortOrder::Za;
    default: return StockSortOrder::Quantity;
  }
}

inline int stockSortOrderIndex(StockSortOrder order) {
  switch (order) {
    case StockSortOrder::Az: return 0;
    case StockSortOrder::Za: return 1;
    case StockSortOrder::Quantity: return 2;
  }
  return 0;
}

// The enum follows the panel order, so the option index is the enum value.
inline StockDateFilter stockDateFilterAt(int option) {
  switch (option) {
    case 0: return StockDateFilter::All;
    case 1: return StockDateFilter::Today;
    case 2: return StockDateFilter::Last7Days;
    case 3: return StockDateFilter::Last30Days;
    case 4: return StockDateFilter::OlderThan30Days;
    default: return StockDateFilter::All;
  }
}

inline int stockDateFilterIndex(StockDateFilter filter) {
  return static_cast<int>(filter);
}

inline std::string stockSortOrderPanelName(StockSortOrder order) {
  switch (order) {
    case StockSortOrder::Az: return "A-Z";
    case StockSortOrder::Za: return "Z-A";
    case StockSortOrder::Quantity: return "Quantity";
  }
  return "A-Z";
}

inline std::string stockDateFilterPanelName(StockDateFilter filter) {
  switch (filter) {
    case StockDateFilter::All: return "All";
    case StockDateFilter::Today: return "Today";
    case StockDateFilter::Last7Days: return "7 days";
    case StockDateFilter::Last30Days: return "30 days";
    case StockDateFilter::OlderThan30Days: return "Over 30";
  }
  return "All";
}

// Header summary name. "A-Z, <this>" or "Z-A, <this>" must fit in 17 columns at 100 columns wide.
inline std::string stockDateFilterSummaryName(StockDateFilter filter) {
  switch (filter) {
    case StockDateFilter::All: return "all dates";
    case StockDateFilter::Today: return "today";
    case StockDateFilter::Last7Days: return "last 7 days";
    case StockDateFilter::Last30Days: return "last 30 days";
    case StockDateFilter::OlderThan30Days: return "over 30 days";
  }
  return "all dates";
}

// Shows the applied sort and date filter while the panel is closed.
inline std::string stockFilterSummary(StockSortOrder sortOrder, StockDateFilter dateFilter) {
  const std::string sortName = sortOrder == StockSortOrder::Quantity ? "Quantity"
                               : sortOrder == StockSortOrder::Za   ? "Z-A"
                                                                   : "A-Z";
  return sortName + ", " + stockDateFilterSummaryName(dateFilter);
}

inline void resetStockFilterState(StockDateFilter& dateFilter, StockSortOrder& sortOrder) {
  dateFilter = StockDateFilter::All;
  sortOrder = StockSortOrder::Az;
}

// Width of the Filter button: " Filter " plus the "f " key hint.
constexpr std::size_t kStockFilterButtonCells = 10;

// Outer widths of the stock list and detail panels for a terminal of screenWidth columns.
inline int stockDetailOuterWidth(int screenWidth) {
  return std::clamp(screenWidth / 3, 42, 60);
}

inline int stockListOuterWidth(int screenWidth) {
  return std::max(42, screenWidth - stockDetailOuterWidth(screenWidth) - 1);
}

// Ranked-view header: the full label, then the short one. The summary is dropped only
// when even the short label does not fit beside it and the Filter button.
struct StockRankedHeader {
  std::string label;
  bool showSummary = false;
  std::size_t cells = 0;  // Width of label, summary (when shown), gap, and Filter button.
};

inline StockRankedHeader stockRankedHeaderFor(std::size_t matchCount, const std::string& filterSummary,
                                              int availableWidth) {
  const std::string matches = std::to_string(matchCount) + " matches";
  const std::string fullLabel = "Stock  " + matches + ", ranked by value";
  const std::string shortLabel = "Ranked  " + matches;
  const auto cellsFor = [&](const std::string& label, bool withSummary) {
    return displayWidth(label) + (withSummary ? displayWidth(filterSummary) : 0) + 1 + kStockFilterButtonCells;
  };
  const auto available = static_cast<std::size_t>(std::max(availableWidth, 0));
  if (cellsFor(fullLabel, true) <= available) return {fullLabel, true, cellsFor(fullLabel, true)};
  if (cellsFor(shortLabel, true) <= available) return {shortLabel, true, cellsFor(shortLabel, true)};
  return {shortLabel, false, cellsFor(shortLabel, false)};
}

}  // namespace inventatory
