// Inventatory - stock list and ranked-result rendering.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

namespace {

enum class CellAlign { Left, Center, Right };

}  // namespace

ftxui::Elements App::renderStockListRows(const vector<InventorySearchMatch>& searchMatches,
                                          const vector<size_t>& filtered, bool rankedView,
                                          size_t activeSelection, int qtyWidth,
                                          int partWidth, int categoryWidth, int matchWidth,
                                          bool groupByCategory) const {
  auto fixedCell = [](const string& text, int width, ftxui::Color color, CellAlign align = CellAlign::Left) {
    const auto content = styledText(ellipsize(text, static_cast<size_t>(max(width, 0))), color);
    ftxui::Elements parts;
    if (align != CellAlign::Left) parts.push_back(ftxui::filler());
    parts.push_back(content);
    if (align != CellAlign::Right) parts.push_back(ftxui::filler());
    return ftxui::hbox(move(parts)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
  };
  const auto quantityCell = [&](int quantity, ftxui::Color color) {
    return ftxui::hbox({
        ftxui::filler(),
        uiHeaderText(to_string(quantity), color),
        ftxui::text(" "),
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth);
  };

  const auto qtyHeaderCell = ftxui::hbox({
                                  ftxui::filler(),
                                  styledText(stocktakeActive_ ? "Count" : "Qty", uiMutedColor()),
                                  ftxui::text(" "),
                              }) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth);

  ftxui::Elements listRows;
  if (rankedView) {
    listRows.push_back(ftxui::hbox({
                           fixedCell("Component Category", partWidth, uiMutedColor()),
                           ftxui::separator() | ftxui::color(uiDimColor()),
                           fixedCell("Fit", matchWidth, uiMutedColor(), CellAlign::Center),
                           ftxui::separator() | ftxui::color(uiDimColor()),
                           qtyHeaderCell,
                       }) |
                       ftxui::bgcolor(uiPanelLeftBg()));
  } else if (!groupByCategory) {
    listRows.push_back(ftxui::hbox({
                           fixedCell("Component Category", partWidth, uiMutedColor()),
                           ftxui::separator() | ftxui::color(uiDimColor()),
                           fixedCell("Category", categoryWidth, uiMutedColor()),
                           ftxui::filler(),
                           ftxui::separator() | ftxui::color(uiDimColor()),
                           qtyHeaderCell,
                       }) |
                       ftxui::bgcolor(uiPanelLeftBg()));
  }

  // Every list row carries the part/quantity separator at the same column so the
  // vertical rule runs unbroken through group headers and spacers alike.
  const auto ruledRow = [&](ftxui::Element partCell, ftxui::Color background) {
    ftxui::Elements cells = {
        move(partCell),
        ftxui::separator() | ftxui::color(uiDimColor()),
    };
    if (rankedView) {
      cells.push_back(ftxui::text("") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, matchWidth));
      cells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
    }
    cells.push_back(ftxui::text("") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth));
    return ftxui::hbox(move(cells)) | ftxui::bgcolor(background);
  };

  const auto bandTitle = [](PhysicalValueMatchBand band, bool closest) {
    switch (band) {
      case PhysicalValueMatchBand::Exact: return string(" Exact matches");
      case PhysicalValueMatchBand::Workable: return string(" Workable matches");
      case PhysicalValueMatchBand::Possible: return string(" Possible matches");
      case PhysicalValueMatchBand::None:
        return closest ? string(" Other same-type matches") : string(" Other matches");
    }
    return string(" Other matches");
  };
  const auto bandColor = [](PhysicalValueMatchBand band) {
    switch (band) {
      case PhysicalValueMatchBand::Exact: return uiSuccessColor();
      case PhysicalValueMatchBand::Workable: return uiFocusColor();
      case PhysicalValueMatchBand::Possible: return uiWarnColor();
      case PhysicalValueMatchBand::None: return uiMutedColor();
    }
    return uiMutedColor();
  };
  const auto matchLabel = [](const InventorySearchMatch& match) {
    if (!match.hasPhysicalComparison) return string("TEXT");
    if (match.band == PhysicalValueMatchBand::Exact) return string("EXACT");
    if (!isfinite(match.signedRelativeDifference)) return string("OUTSIDE");
    ostringstream value;
    value << showpos << fixed << setprecision(1) << match.signedRelativeDifference * 100.0 << "%";
    return value.str();
  };

  if (filtered.empty()) {
    string message;
    if (closestSearchActive_) {
      if (trim(closestSearchQuery_).empty()) {
        message = "Enter a physical value to find the closest part.";
      } else if (!parsePhysicalValue(closestSearchQuery_).has_value()) {
        message = "Use a physical value such as 4.7k, 100nF, or 1MHz.";
      } else {
        message = "No inventory records have the same physical value type.";
      }
    } else {
      message = "No items match \"" + searchQuery_ + "\".";
    }
    listRows.push_back(fullLine(message, uiMutedColor(), uiPanelLeftBg()));
  } else {
    for (size_t index = 0; index < filtered.size(); ++index) {
      const auto& searchMatch = searchMatches[index];
      const auto& item = store_.items()[searchMatch.itemIndex];
      const auto category = displayCategory(item.category);
      if (rankedView) {
        const bool startsGroup = index == 0 || searchMatch.band != searchMatches[index - 1].band;
        if (startsGroup) {
          if (index != 0) {
            listRows.push_back(
                ruledRow(ftxui::text("") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth), uiSurfaceBg()));
          }
          listRows.push_back(ruledRow(uiHeaderText(bandTitle(searchMatch.band, closestSearchActive_),
                                                       bandColor(searchMatch.band)) |
                                          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth),
                                      uiRaisedSurfaceBg()));
        }
      } else if (groupByCategory) {
        const bool startsGroup =
            index == 0 || category != displayCategory(store_.items()[filtered[index - 1]].category);
        if (startsGroup) {
          size_t partCount = 0;
          size_t lowCount = 0;
          for (size_t next = index; next < filtered.size(); ++next) {
            const auto& nextItem = store_.items()[searchMatches[next].itemIndex];
            if (displayCategory(nextItem.category) != category) break;
            ++partCount;
            if (nextItem.quantity <= 0 || isLowStock(nextItem, settings_.lowStockThreshold)) ++lowCount;
          }
          const string countText = to_string(partCount) + (partCount == 1 ? " part" : " parts");
          ftxui::Elements headerCells = {ftxui::text(" "), uiHeaderText(category, uiPrimaryText()), ftxui::filler()};
          if (lowCount > 0) {
            headerCells.push_back(styledText(to_string(lowCount) + " low", uiWarnColor()));
            headerCells.push_back(ftxui::text("  "));
          }
          headerCells.push_back(styledText(countText + " ", uiMutedColor()));
          listRows.push_back(ruledRow(ftxui::hbox(move(headerCells)) |
                                          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth),
                                      uiRaisedSurfaceBg()));
        }
      }
      const bool selected = index == activeSelection;
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      ftxui::Element partCell;
      if (groupByCategory) {
        partCell = ftxui::hbox({
                       ftxui::text("   "),
                       uiPartName(item.partName, max(1, partWidth - 3), selected),
                       ftxui::filler(),
                   }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth);
      } else {
        partCell = ftxui::hbox({
                       ftxui::text(" "),
                       uiPartName(item.partName, max(1, partWidth - 1), selected),
                       ftxui::filler(),
                   }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth);
      }
      ftxui::Elements cells = {move(partCell), ftxui::separator() | ftxui::color(uiDimColor())};
      if (rankedView) {
        cells.push_back(fixedCell(matchLabel(searchMatch), matchWidth, bandColor(searchMatch.band), CellAlign::Center));
        cells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
      } else if (!groupByCategory) {
        cells.push_back(fixedCell(category, categoryWidth, selected ? uiTitleColor() : uiLabelColor()));
        cells.push_back(ftxui::filler());
        cells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
      }
      cells.push_back(quantityCell(stocktakeActive_ ? stocktakeCountFor(item) : item.quantity,
                                   uiQuantityColor(item, settings_.lowStockThreshold)));
      auto row = ftxui::hbox(move(cells)) | ftxui::bgcolor(bg);
      if (selected) {
        row = row | ftxui::select;
      }
      auto self = const_cast<App*>(this);
      const auto position = index;
      listRows.push_back(target(row, "stock.row." + item.id, UiTargetKind::Row, [self, position] {
        if (self->closestSearchActive_) self->closestSelectedPosition_ = position;
        else self->selectedPosition_ = position;
        self->syncSelectionToFilter();
        self->dirty_ = true;
      }));
    }
  }
  return listRows;
}

}  // namespace inventatory
