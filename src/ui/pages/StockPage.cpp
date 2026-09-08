// Inventatory - Hardware Inventory Management System
// Stock browser page rendering and keyboard handling.

#include "App.h"

#include "core/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;


ftxui::Element App::renderStockUi() const {
  const auto searchMatches = stockSearchMatches();
  vector<size_t> filtered;
  filtered.reserve(searchMatches.size());
  for (const auto& match : searchMatches) filtered.push_back(match.itemIndex);
  const bool rankedView = closestSearchActive_ || any_of(searchMatches.begin(), searchMatches.end(), [](const auto& match) {
    return match.hasPhysicalComparison;
  });
  const size_t activeSelection = closestSearchActive_ ? closestSelectedPosition_ : selectedPosition_;
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int detailOuterWidth = clamp(screenWidth / 3, 42, 60);
  const int listOuterWidth = max(42, screenWidth - detailOuterWidth - 1);
  const int listInnerWidth = max(20, listOuterWidth - 2);
  const int detailInnerWidth = max(20, detailOuterWidth - 2);
  size_t longestQuantity = string("Qty").size();
  size_t longestPartName = string("Part").size();
  size_t longestCategory = string("Category").size();
  for (const auto& item : store_.items()) {
    longestPartName = max(longestPartName, item.partName.size());
    longestQuantity = max(longestQuantity, to_string(item.quantity).size());
    longestCategory = max(longestCategory, displayCategory(item.category).size());
  }
  // The separators are positioned from the complete inventory, not just the
  // current filter result, so columns do not jump or truncate a later row.
  const int qtyWidth = max(7, static_cast<int>(longestQuantity) + 4);
  // Name sorting yields one contiguous run per category, so the category is
  // stated once in a group header and the part column takes the width the
  // repeated column used to waste. Quantity sorting interleaves categories, so
  // it keeps a per-row column instead.
  const bool groupByCategory = !rankedView && stockSortOrder_ != StockSortOrder::Quantity;
  const int matchWidth = rankedView ? 14 : 0;
  const int minCategoryWidth = 12;
  const int maxPartWidth = groupByCategory ? max(18, listInnerWidth - qtyWidth - 1)
                                           : max(18, listInnerWidth - qtyWidth - matchWidth - minCategoryWidth - 2);
  const int partWidth = groupByCategory ? maxPartWidth : clamp(static_cast<int>(longestPartName) + 3, 18, maxPartWidth);
  // The fallback column is sized to its longest value and sits beside the part
  // name; the leftover width becomes one gutter before the right-anchored
  // quantity, rather than padding out the category cell.
  const int categoryWidth = clamp(static_cast<int>(longestCategory) + 2, minCategoryWidth,
                                  max(minCategoryWidth, listInnerWidth - partWidth - qtyWidth - matchWidth - 2));

  auto listRows = renderStockListRows(searchMatches, filtered, rankedView, activeSelection, qtyWidth,
                                    partWidth, categoryWidth, matchWidth, groupByCategory);
  auto detailRows = renderStockDetailRows(searchMatches, rankedView, activeSelection, detailInnerWidth);
  const bool editing = inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue;
  auto self = const_cast<App*>(this);
  const auto stockHeader = stocktakeActive_
                               ? "STOCKTAKE  " + to_string(stocktakeCountedItems()) + "/" +
                                     to_string(store_.items().size()) + " counted"
                               : closestSearchActive_
                                     ? "CLOSEST TO  " + (closestSearchQuery_.empty() ? string("...") : closestSearchQuery_) +
                                           "  · " + to_string(filtered.size()) + " candidates"
                                     : rankedView ? "STOCK  " + to_string(filtered.size()) + " matches · ranked by value"
                                                  : "STOCK  " + to_string(filtered.size()) + " items";
  const bool filtersEnabled = !stocktakeActive_ && !closestSearchActive_;
  auto filterButton = target(styledText(" Sort / Filter ", filtersEnabled ? uiInteractiveColor() : uiMutedColor(),
                                       filtersEnabled ? uiRaisedSurfaceBg() : uiSurfaceBg()),
                             "stock.filters.header", UiTargetKind::Button,
                             [self] { self->openStockFilterPanel(); }, filtersEnabled);
  listRows.insert(listRows.begin(), ftxui::hbox({
      styledText(stockHeader, stocktakeActive_ ? uiAccentColor() : uiSecondaryText()),
      ftxui::filler(),
      filterButton,
  }) | ftxui::bgcolor(uiSurfaceBg()));

  ftxui::Element filterMenu = ftxui::text("");
  if (inputMode_ == InputMode::StockFilter && !closestSearchActive_) {
    ftxui::Elements filterRows;
    if (stockDateFilterSubmenuOpen_) {
      filterRows.push_back(fullLine("FILTER BY DATE OF MODIFICATION", uiSecondaryText(), uiPanelLeftBg()));
      const vector<StockDateFilter> dateFilters = {
          StockDateFilter::All,
          StockDateFilter::Today,
          StockDateFilter::Last7Days,
          StockDateFilter::Last30Days,
          StockDateFilter::OlderThan30Days,
      };
      for (size_t index = 0; index < dateFilters.size(); ++index) {
        const bool selected = static_cast<int>(index) == stockFilterSelection_;
        auto row = fullLine(string(selected ? "  > " : "    ") + stockDateFilterName(dateFilters[index]),
                            selected ? uiTitleColor() : uiMutedColor(),
                            selected ? uiSelectionBg() : uiPanelLeftBg());
        filterRows.push_back(target(row, "stock.filter.date." + to_string(index), UiTargetKind::Button,
                                    [self, filter = dateFilters[index]] { self->applyStockDateFilter(filter); }));
      }
    } else {
      filterRows.push_back(fullLine("FILTER", uiSecondaryText(), uiPanelLeftBg()));
      const vector<string> labels = {"Date of modification", "Sort: quantity", "Sort: A-Z", "Sort: Z-A"};
      for (size_t index = 0; index < labels.size(); ++index) {
        const bool selected = static_cast<int>(index) == stockFilterSelection_;
        auto row = fullLine(string(selected ? "  > " : "    ") + labels[index],
                            selected ? uiTitleColor() : uiMutedColor(),
                            selected ? uiSelectionBg() : uiPanelLeftBg());
        if (index == 0) {
          filterRows.push_back(target(row, "stock.filter.date", UiTargetKind::Button,
                                      [self] { self->openStockDateFilterSubmenu(); }));
        } else {
          const auto order = index == 1 ? StockSortOrder::Quantity
                            : index == 2 ? StockSortOrder::Az
                                         : StockSortOrder::Za;
          filterRows.push_back(target(row, "stock.filter.sort." + to_string(index), UiTargetKind::Button,
                                      [self, order] { self->applyStockSortOrder(order); }));
        }
      }
    }
    filterMenu = ftxui::window(styledText(" Sort / Filter ", uiAccentColor()),
                               ftxui::vbox(move(filterRows)) | ftxui::bgcolor(uiPanelLeftBg()) |
                                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 34)) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 34) |
                 ftxui::color(uiAccentColor()) | ftxui::bgcolor(uiPanelLeftBg());
  }

  ftxui::Element listPanel = ftxui::vbox(move(listRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                             ftxui::bgcolor(uiSurfaceBg()) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listOuterWidth);
  if (inputMode_ == InputMode::StockFilter && !closestSearchActive_) {
    // Clear only the popup footprint, then draw it over the list. This keeps
    // the stock rows in place while preventing their text from bleeding
    // through the floating menu.
    auto filterOverlay = ftxui::vbox({
        ftxui::text(""),
        ftxui::hbox({ftxui::filler(), filterMenu | ftxui::clear_under}),
        ftxui::filler(),
    });
    listPanel = ftxui::dbox({listPanel, filterOverlay}) |
                ftxui::bgcolor(uiSurfaceBg()) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listOuterWidth);
  }
  // The header and the action footer sit outside the scrolling frame so the
  // controls stay reachable no matter how long the selected part's detail is.
  ftxui::Element detailFooter;
  if (editing) {
    detailFooter = styledText(" \xE2\x86\x91\xE2\x86\x93 field  \xE2\x8F\x8E edit  s save  esc cancel", uiMutedColor());
  } else if (closestSearchActive_) {
    detailFooter = styledText(inputMode_ == InputMode::ClosestSearch
                                  ? " type target  \xE2\x86\x91\xE2\x86\x93 select  enter keep  esc close"
                                  : " \xE2\x86\x91\xE2\x86\x93 select  enter details  F edit target  esc close",
                              uiMutedColor());
  } else if (stocktakeActive_) {
    const bool ready = stocktakeCountedItems() == store_.items().size();
    detailFooter = ftxui::hbox({
        target(uiSecondaryButton("Count", nullopt, selectedItem() != nullptr), "stocktake.count", UiTargetKind::Button,
               [self] { self->beginStocktakeCount(); }, selectedItem() != nullptr),
        ftxui::text(" "),
        target(uiSecondaryButton("Finish", nullopt, ready), "stocktake.finish", UiTargetKind::Button,
               [self] { self->finishStocktake(); }, ready),
        ftxui::text(" "),
        target(uiSecondaryButton("Cancel"), "stocktake.cancel", UiTargetKind::Button,
               [self] { self->cancelStocktake(); }),
        ftxui::filler(),
    });
  } else {
    const bool hasItem = selectedItem() != nullptr;
    detailFooter = ftxui::hbox({
        target(uiSecondaryButton("New"), "stock.new", UiTargetKind::Button,
               [self] { self->beginEditCurrentItem(true); }),
        ftxui::text(" "),
        target(uiSecondaryButton("Edit", nullopt, hasItem), "stock.edit", UiTargetKind::Button,
               [self] { self->beginEditCurrentItem(false); }, hasItem),
        ftxui::text(" "),
        target(uiSecondaryButton("-", nullopt, hasItem), "stock.decrement", UiTargetKind::Button,
               [self] { self->adjustQuantity(-1); }, hasItem),
        target(uiSecondaryButton("+", nullopt, hasItem), "stock.increment", UiTargetKind::Button,
               [self] { self->adjustQuantity(1); }, hasItem),
        ftxui::text(" "),
        target(uiSecondaryButton("Set qty", nullopt, hasItem), "stock.set_quantity", UiTargetKind::Button,
               [self] {
                 if (const auto* item = self->selectedItem()) {
                   self->inputBuffer_ = to_string(item->quantity);
                   self->inputMode_ = App::InputMode::QuantityAdjust;
                   self->setMessage("Enter the total quantity on hand", 3);
                 }
               }, hasItem),
        ftxui::text(" "),
        // Five peer actions with no single primary among them, so they share
        // the ordinary raised-button role rather than competing for the eye.
        target(uiSecondaryButton("Print", nullopt, hasItem), "stock.print", UiTargetKind::Button,
               [self] { self->printSelectedLabel(); }, hasItem),
        ftxui::filler(),
    });
  }
  auto detailPanel = ftxui::vbox({
                         fullLine(editing ? "EDIT ITEM" : "ITEM DETAIL", uiSecondaryText(), uiSurfaceBg()),
                         ftxui::vbox(move(detailRows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::flex,
                         ftxui::separator() | ftxui::color(uiDividerColor()),
                         detailFooter,
                     }) |
                     ftxui::bgcolor(uiSurfaceBg()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, detailOuterWidth);

  auto page = ftxui::hbox({
      listPanel,
      ftxui::separator() | ftxui::color(uiDividerColor()),
      detailPanel,
  });

  if (!deleteConfirmationActive()) {
    return page;
  }

  const auto* item = store_.findById(deleteConfirmationItemId_);
  const auto itemLabel = item == nullptr ? string("this part") : item->partName;
  const auto secondsLeft = deleteConfirmationSecondsLeft();
  const int popupWidth = max(48, min(screenWidth - 12, 72));

  ftxui::Elements popupRows;
  popupRows.push_back(ftxui::paragraphAlignLeft("Are you sure you want to delete this part from the database?") |
                      ftxui::color(uiTitleColor()));
  popupRows.push_back(uiDivider());
  popupRows.push_back(ftxui::paragraphAlignLeft("Selected: " + itemLabel) | ftxui::color(uiWarnColor()));
  popupRows.push_back(ftxui::paragraphAlignLeft("Press Enter to confirm after the timer unlocks.") |
                      ftxui::color(secondsLeft == 0 ? uiTitleColor() : uiMutedColor()));
  popupRows.push_back(ftxui::paragraphAlignLeft("Press Esc to cancel.") | ftxui::color(uiMutedColor()));
  popupRows.push_back(ftxui::paragraphAlignLeft(to_string(secondsLeft) + " second" +
                                                (secondsLeft == 1 ? string() : string("s")) + " remaining") |
                      ftxui::color(uiWarnColor()));

  auto popup = ftxui::window(styledText("Delete item", uiDangerColor()),
                             ftxui::vbox(move(popupRows)) | ftxui::bgcolor(uiPanelRightBg()) |
                                 ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, popupWidth)) |
               ftxui::color(uiDangerColor());

  auto overlay = ftxui::vbox({
      ftxui::filler(),
      ftxui::hbox({
          ftxui::filler(),
          popup,
          ftxui::filler(),
      }),
      ftxui::filler(),
  });

  return ftxui::dbox({
      page,
      overlay,
  });
}

}  // namespace inventatory
