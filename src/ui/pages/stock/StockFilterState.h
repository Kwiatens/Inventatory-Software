#pragma once

namespace inventatory {

enum class StockDateFilter { All, Today, Last7Days, Last30Days, OlderThan30Days };
enum class StockSortOrder { Az, Quantity, Za };
enum class StockFilterMenuItem { Date, Quantity, Az, Za, Reset };

constexpr int kStockFilterMenuOptionCount = 5;
constexpr int kStockDateFilterOptionCount = 5;

inline StockFilterMenuItem stockFilterMenuItemAt(int selection) {
  switch (selection) {
    case 0: return StockFilterMenuItem::Date;
    case 1: return StockFilterMenuItem::Quantity;
    case 2: return StockFilterMenuItem::Az;
    case 3: return StockFilterMenuItem::Za;
    default: return StockFilterMenuItem::Reset;
  }
}

inline int stockFilterMenuSelection(StockDateFilter dateFilter, StockSortOrder sortOrder) {
  if (dateFilter != StockDateFilter::All) return 0;
  switch (sortOrder) {
    case StockSortOrder::Quantity: return 1;
    case StockSortOrder::Az: return 2;
    case StockSortOrder::Za: return 3;
  }
  return 2;
}

inline StockDateFilter stockDateFilterAt(int selection) {
  switch (selection) {
    case 0: return StockDateFilter::All;
    case 1: return StockDateFilter::Today;
    case 2: return StockDateFilter::Last7Days;
    case 3: return StockDateFilter::Last30Days;
    case 4: return StockDateFilter::OlderThan30Days;
    default: return StockDateFilter::All;
  }
}

inline void resetStockFilterState(StockDateFilter& dateFilter, StockSortOrder& sortOrder,
                                  int& selection, bool& dateSubmenuOpen) {
  dateFilter = StockDateFilter::All;
  sortOrder = StockSortOrder::Az;
  dateSubmenuOpen = false;
  selection = stockFilterMenuSelection(dateFilter, sortOrder);
}

}  // namespace inventatory
