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
  const auto quantityCell = [&](int quantity) {
    return ftxui::hbox({
        ftxui::filler(),
        uiBodyText(to_string(quantity), uiPrimaryText()),
        ftxui::text(" "),
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth);
  };
  const auto treeCell = [](const string& value, int width, ftxui::Color color) {
    return ftxui::hbox({styledText(value, color), ftxui::filler()}) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
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
                           fixedCell("Part", partWidth, uiMutedColor()),
                           ftxui::separator() | ftxui::color(uiDimColor()),
                           fixedCell("Fit", matchWidth, uiMutedColor(), CellAlign::Center),
                           ftxui::separator() | ftxui::color(uiDimColor()),
                           qtyHeaderCell,
                       }) |
                       ftxui::bgcolor(uiPanelLeftBg()));
  } else if (groupByCategory) {
    listRows.push_back(ftxui::hbox({
                           treeCell("", 8, uiMutedColor()),
                           fixedCell("Part", max(1, partWidth - 8), uiMutedColor()),
                           ftxui::separator() | ftxui::color(uiDimColor()),
                           qtyHeaderCell,
                       }) |
                       ftxui::bgcolor(uiPanelLeftBg()));
  } else {
    listRows.push_back(ftxui::hbox({
                           fixedCell("Part", partWidth, uiMutedColor()),
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
      case PhysicalValueMatchBand::Exact: return string(" EXACT MATCHES");
      case PhysicalValueMatchBand::Workable: return string(" WORKABLE MATCHES");
      case PhysicalValueMatchBand::Possible: return string(" POSSIBLE MATCHES");
      case PhysicalValueMatchBand::None:
        return closest ? string(" OTHER SAME-TYPE MATCHES") : string(" OTHER MATCHES");
    }
    return string(" OTHER MATCHES");
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
  const auto categoryIsLast = [&](size_t index, const string& category) {
    for (size_t next = index + 1; next < filtered.size(); ++next) {
      const auto& nextMatch = searchMatches[next];
      const auto& nextItem = store_.items()[nextMatch.itemIndex];
      if (displayCategory(nextItem.category) != category) return false;
    }
    return true;
  };
  const auto partIsLast = [&](size_t index, const string& category) {
    if (index + 1 >= filtered.size()) return true;
    const auto& nextMatch = searchMatches[index + 1];
    const auto& nextItem = store_.items()[nextMatch.itemIndex];
    return displayCategory(nextItem.category) != category;
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
          // A blank spacer sets each group apart; the first needs none because
          // the column header sits directly above it.
          if (index != 0) {
            listRows.push_back(
                ruledRow(ftxui::text("") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth), uiSurfaceBg()));
          }
          const auto categoryBranch = treeCell(categoryIsLast(index, category) ? "└──" : "├──",
                                               4, uiDividerColor());
          const auto categoryTitle =
              ftxui::hbox({uiHeaderText(" " + toUpper(category), uiSecondaryText()), ftxui::filler()}) |
              ftxui::size(ftxui::WIDTH, ftxui::EQUAL, max(1, partWidth - 4));
          listRows.push_back(ruledRow(ftxui::hbox({categoryBranch, categoryTitle}), uiRaisedSurfaceBg()));
        }
      }
      const bool selected = index == activeSelection;
      const auto bg = selected ? uiSelectionBg() : (index % 2 == 0 ? uiCanvasBg() : uiSurfaceBg());
      ftxui::Element partCell;
      if (groupByCategory) {
        const auto categoryLast = categoryIsLast(index, category);
        const auto partLast = partIsLast(index, category);
        partCell = ftxui::hbox({
            treeCell(categoryLast ? "    " : "│   ", 4, uiDividerColor()),
            treeCell(partLast ? "└──" : "├──", 4, uiDividerColor()),
            fixedCell(item.partName, max(1, partWidth - 8), uiPrimaryText()),
        });
      } else {
        partCell = fixedCell(" " + item.partName, partWidth, uiPrimaryText());
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
      cells.push_back(quantityCell(stocktakeActive_ ? stocktakeCountFor(item) : item.quantity));
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
