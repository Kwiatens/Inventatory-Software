// Inventatory - Hardware Inventory Management System
// Stock browser page rendering and keyboard handling.

#include "App.h"

#include "core/parts/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

// Fits the Modified and reset rows at 100 columns; the panel sits over the list's right edge.
constexpr int kStockFilterPanelWidth = 53;

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
  const int detailOuterWidth = stockDetailOuterWidth(screenWidth);
  const int listOuterWidth = stockListOuterWidth(screenWidth);
  const int listInnerWidth = max(20, listOuterWidth - 2);
  const int detailInnerWidth = max(20, detailOuterWidth - 2);
  size_t longestQuantity = string("Qty").size();
  size_t longestPartName = string("Part").size();
  size_t longestCategory = string("Category").size();
  for (const auto& item : store_.items()) {
    longestPartName = max(longestPartName, displayWidth(item.partName));
    longestQuantity = max(longestQuantity, to_string(item.quantity).size());
    if (stocktakeActive_) {
      longestQuantity = max(longestQuantity, to_string(stocktakeCountFor(item)).size());
    }
    longestCategory = max(longestCategory, displayWidth(displayCategory(item.category)));
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
  const bool filtersEnabled = !stocktakeActive_ && !closestSearchActive_;
  const auto filterSummary = stockFilterSummary(stockSortOrder_, stockDateFilter_);
  // The ranked header shortens its wording, and drops the summary, before the
  // Filter button would be pushed out of the list panel.
  const bool rankedMatchesHeader = rankedView && filtersEnabled;
  const auto rankedHeader = stockRankedHeaderFor(filtered.size(), filterSummary, listOuterWidth);
  const auto stockHeader = stocktakeActive_
                               ? "Stocktake  " + to_string(stocktakeCountedItems()) + "/" +
                                     to_string(store_.items().size()) + " counted"
                               : closestSearchActive_
                                     ? "Closest to  " + (closestSearchQuery_.empty() ? string("...") : closestSearchQuery_) +
                                           ", " + to_string(filtered.size()) + " candidates"
                                     : rankedView ? rankedHeader.label
                                                  : groupByCategory ? string("Component Category") : string();
  // The summary states the sort and date filter in use, so they show while the panel is closed.
  const bool showFilterSummary = filtersEnabled && (!rankedMatchesHeader || rankedHeader.showSummary);
  const auto filterSummaryText = showFilterSummary ? styledText(filterSummary, uiMutedText()) : ftxui::text("");
  const auto filterButtonBg = filtersEnabled ? uiRaisedSurfaceBg() : uiSurfaceBg();
  auto filterButton = target(ftxui::hbox({
                                 styledText(" Filter ", filtersEnabled ? uiInteractiveColor() : uiMutedText(),
                                            filterButtonBg),
                                 styledText("f ", uiMutedText(), filterButtonBg),
                             }),
                             "stock.filters.header", UiTargetKind::Button,
                             [self] { self->openStockFilterPanel(); }, filtersEnabled);
  ftxui::Element listHeader;
  if (groupByCategory) {
    const auto qtyHeaderCell = ftxui::hbox({
                                      ftxui::filler(),
                                      styledText(stocktakeActive_ ? "Count" : "Qty", uiMutedColor()),
                                      ftxui::text(" "),
                                  }) |
                               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth);
    const auto groupedHeaderLabel = ftxui::hbox({
                                         styledText(stockHeader, stocktakeActive_ ? uiAccentColor() : uiSecondaryText()),
                                         ftxui::filler(),
                                         filterSummaryText,
                                         ftxui::text(" "),
                                         filterButton,
                                     }) |
                                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth);
    listHeader = ftxui::hbox({
        groupedHeaderLabel,
        ftxui::separator() | ftxui::color(uiDimColor()),
        qtyHeaderCell,
    }) | ftxui::bgcolor(uiSurfaceBg());
  } else {
    listHeader = ftxui::hbox({
        styledText(stockHeader, stocktakeActive_ ? uiAccentColor() : uiSecondaryText()),
        ftxui::filler(),
        filterSummaryText,
        ftxui::text(" "),
        filterButton,
    }) | ftxui::bgcolor(uiSurfaceBg());
  }

  auto listBody = ftxui::vbox(move(listRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                  ftxui::bgcolor(uiSurfaceBg()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listOuterWidth) |
                  ftxui::flex;
  ftxui::Element listPanel = ftxui::vbox({move(listHeader), move(listBody)}) |
                             ftxui::bgcolor(uiSurfaceBg()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listOuterWidth) |
                             ftxui::flex;
  const bool filterOpen = inputMode_ == InputMode::StockFilter && !closestSearchActive_;
  if (filterOpen) {
    // Clear only the panel footprint, then draw it over the list. This keeps
    // the stock rows in place while preventing their text from bleeding
    // through the floating panel.
    const int panelWidth = min(kStockFilterPanelWidth, listOuterWidth);
    auto filterOverlay = ftxui::vbox({
        ftxui::text(""),
        ftxui::hbox({ftxui::filler(),
                     renderStockFilterPanel(panelWidth, static_cast<int>(filtered.size())) | ftxui::clear_under}),
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
    detailFooter = styledText(" Up/Down field   Enter edit   s save   Esc cancel", uiMutedColor());
  } else if (closestSearchActive_) {
    detailFooter = styledText(inputMode_ == InputMode::ClosestSearch
                                  ? " type target   Up/Down select   Enter keep   Esc close"
                                  : " Up/Down select   Enter details   F edit target   Esc close",
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
    ftxui::Elements editRow = {
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
                   self->inputReplaceOnType_ = true;
                   self->inputMode_ = App::InputMode::QuantityAdjust;
                   self->setMessage("Enter the total quantity on hand", 3);
                 }
               }, hasItem),
    };
    ftxui::Elements outputRow = {
        // Peer actions share the ordinary raised-button role rather than
        // competing for a single primary; Delete alone carries the danger
        // role because it removes database records.
        target(uiSecondaryButton("Print", nullopt, hasItem), "stock.print", UiTargetKind::Button,
               [self] { self->printSelectedLabel(); }, hasItem),
        ftxui::text(" "),
        // Arms the same confirmation modal as Ctrl+Backspace.
        target(uiSecondaryButton("Delete", uiDangerColor(), hasItem), "stock.delete", UiTargetKind::Button,
               [self] { self->armDeleteConfirmation(); }, hasItem),
        ftxui::filler(),
    };
    // The full button strip needs 46 columns. Narrower panels (the 100x30 and 120x30 terminals) wrap it onto two
    // rows rather than clipping the Delete button.
    const bool wrap = detailOuterWidth < 47;
    if (wrap) {
      editRow.push_back(ftxui::filler());
      detailFooter = ftxui::vbox({ftxui::hbox(move(editRow)), ftxui::hbox(move(outputRow))});
    } else {
      editRow.push_back(ftxui::text(" "));
      for (auto& element : outputRow) editRow.push_back(move(element));
      detailFooter = ftxui::hbox(move(editRow));
    }
  }
  auto detailPanel = ftxui::vbox({
                         fullLine(editing ? "Edit item" : "Item detail", uiSecondaryText(), uiSurfaceBg()),
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

// The Sort / Filter panel is borderless and sits on the raised surface. Each choice
// row is a segmented control: the applied option is primary text, the others
// are muted, and the keyboard-focused option takes the selection colour. Every
// choice applies at once and the panel stays open.
ftxui::Element App::renderStockFilterPanel(int width, int shownCount) const {
  auto self = const_cast<App*>(this);
  const auto rowBg = uiRaisedSurfaceBg();
  const int totalCount = static_cast<int>(store_.items().size());

  vector<string> sortLabels;
  for (int option = 0; option < kStockSortOptionCount; ++option) {
    sortLabels.push_back(stockSortOrderPanelName(stockSortOrderAt(option)));
  }
  vector<string> dateLabels;
  for (int option = 0; option < kStockDateFilterOptionCount; ++option) {
    dateLabels.push_back(stockDateFilterPanelName(stockDateFilterAt(option)));
  }

  const auto choiceRow = [&](int row, const string& name, const vector<string>& options, int applied,
                             const string& idPrefix, const auto& activate) {
    const bool rowFocused = stockFilterRow_ == row;
    ftxui::Elements cells = {
        styledText(rowFocused ? "> " : "  ", uiPrimaryText(), rowBg),
        fixedCell(name, 11, uiSecondaryText()),
    };
    for (int option = 0; option < static_cast<int>(options.size()); ++option) {
      if (option > 0) cells.push_back(styledText("  ", uiMutedText(), rowBg));
      const bool focused = rowFocused && stockFilterOption_ == option;
      const bool isApplied = option == applied;
      auto cell = styledText(options[option],
                             focused ? uiFocusColor() : isApplied ? uiPrimaryText() : uiMutedText(),
                             focused ? uiSelectionBg() : rowBg);
      cells.push_back(target(cell, idPrefix + to_string(option), UiTargetKind::Button,
                             [activate, row, option] { activate(row, option); }));
    }
    cells.push_back(ftxui::filler());
    return ftxui::hbox(move(cells)) | ftxui::bgcolor(rowBg);
  };

  const auto activateOption = [self](int row, int option) {
    self->focusStockFilterOption(row, option);
    self->activateStockFilterOption(row, option);
  };
  const bool resetFocused = stockFilterRow_ == kStockFilterRowCount - 1;
  auto resetRow = ftxui::hbox({
                      styledText(resetFocused ? "> " : "  ", uiPrimaryText(), rowBg),
                      target(uiSecondaryButton("Reset filters", resetFocused ? optional(uiFocusColor()) : nullopt),
                             "stock.filter.reset", UiTargetKind::Button,
                             [activateOption] { activateOption(kStockFilterRowCount - 1, 0); }),
                      ftxui::filler(),
                      styledText(to_string(shownCount) + " of " + to_string(totalCount) +
                                     (totalCount == 1 ? " part  " : " parts  "),
                                 uiMutedText(), rowBg),
                  }) |
                  ftxui::bgcolor(rowBg);

  return ftxui::vbox({
             choiceRow(0, "Sort", sortLabels, stockSortOrderIndex(stockSortOrder_), "stock.filter.sort.",
                       activateOption),
             choiceRow(1, "Modified", dateLabels, stockDateFilterIndex(stockDateFilter_), "stock.filter.date.",
                       activateOption),
             uiDivider(),
             resetRow,
         }) |
         ftxui::bgcolor(rowBg) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

}  // namespace inventatory
