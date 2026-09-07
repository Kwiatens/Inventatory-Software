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

namespace {

enum class CellAlign { Left, Center, Right };

}  // namespace

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
                           fixedCell("Part", partWidth, uiMutedColor()),
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
          listRows.push_back(ruledRow(uiHeaderText(" " + toUpper(category), uiSecondaryText()) |
                                          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth),
                                      uiRaisedSurfaceBg()));
        }
      }
      const bool selected = index == activeSelection;
      const auto bg = selected ? uiSelectionBg() : (index % 2 == 0 ? uiCanvasBg() : uiSurfaceBg());
      ftxui::Elements cells = {fixedCell(groupByCategory ? "   " + item.partName : " " + item.partName,
                                         partWidth, uiPrimaryText()),
                               ftxui::separator() | ftxui::color(uiDimColor())};
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

  // Editing renders inline here instead of a separate blocking field-name menu:
  // the Detail panel becomes the form, current field highlighted, live typing
  // shown in place. Save ('s') and cancel (Esc) act on the whole form.
  const bool editing = inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue;
  const InventorySearchMatch* selectedMatch = nullptr;
  if (rankedView && !searchMatches.empty()) {
    const auto selectedPosition = min(activeSelection, searchMatches.size() - 1);
    selectedMatch = &searchMatches[selectedPosition];
  }

  ftxui::Elements detailRows;
  auto self = const_cast<App*>(this);
  if (editing) {
    const auto titleText = workingCopy_.isNew ? string("New part") : workingCopy_.item.partName;
    detailRows.push_back(fullLine(titleText, uiTitleColor(), uiRowSelectedBg()));
    detailRows.push_back(uiDivider());
    const int labelWidth = clamp(detailInnerWidth / 3, 14, 20);
    for (size_t index = 0; index < menuOptions_.size(); ++index) {
      const auto& option = menuOptions_[index];
      const bool selected = static_cast<int>(index) == fieldMenuIndex_;
      const bool typing = selected && inputMode_ == InputMode::EditValue;
      const auto value = typing ? inputBuffer_ + "_" : currentFieldValue(option.field);
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto labelColor = selected ? uiTitleColor() : uiMutedColor();
      const auto valueColor = typing ? uiLinkColor() : (selected ? uiTitleColor() : uiLabelColor());
      auto row = ftxui::hbox({
                     fixedCell(option.label, labelWidth, labelColor),
                     styledText(ellipsize(value, static_cast<size_t>(max(detailInnerWidth - labelWidth, 4))),
                                valueColor),
                     ftxui::filler(),
                 }) |
                 ftxui::bgcolor(bg);
      if (selected) {
        row = row | ftxui::select;
      }
      detailRows.push_back(target(row, "stock.field." + to_string(index), UiTargetKind::Field,
                                  [self, index] {
                                    self->fieldMenuIndex_ = static_cast<int>(index);
                                    self->inputBuffer_ = self->currentFieldValue(self->menuOptions_[index].field);
                                    self->inputMode_ = InputMode::EditValue;
                                    self->dirty_ = true;
                                  }));
    }
  } else if (const auto* item = selectedItem()) {
    const auto electricalFields = electricalFieldsForItem(*item);
    const bool passive = categoryContains(*item, {"resistor", "capacitor", "inductor", "diode", "fuse",
                                                   "thermistor", "varistor", "crystal", "resonator"});
    const auto manufacturer = trim(item->manufacturer).empty() ? string("UNKNOWN MANUFACTURER") : item->manufacturer;
    detailRows.push_back(uiBodyText(manufacturer, uiPrimaryText()));
    if (!passive) {
      detailRows.push_back(styledText(item->partName, uiSecondaryText()));
    } else {
      const auto primary = electricalFields.empty() ? item->partName : electricalFields.front().value;
      ftxui::Elements summary = {uiBodyText(primary, uiPrimaryText())};
      for (size_t index = 1; index < electricalFields.size() && index < 3; ++index) {
        if (trim(electricalFields[index].value).empty()) continue;
        summary.push_back(styledText("  " + electricalFields[index].value, uiSecondaryText()));
      }
      detailRows.push_back(ftxui::hbox(move(summary)));
    }
    if (!trim(partShortDescription(*item)).empty() && partShortDescription(*item) != item->partName) {
      detailRows.push_back(styledText(partShortDescription(*item), uiMutedColor()));
    }

    // Essentials: the two facts the footer buttons act on, kept directly under
    // the summary instead of at the bottom of a details list.
    const auto rack = rackLocation(*item, store_.racks());
    const auto quantityColor = item->quantity <= 0 ? uiDangerColor()
                               : isLowStock(*item, settings_.lowStockThreshold) ? uiWarnColor()
                                                  : uiPrimaryText();
    detailRows.push_back(uiDivider());
    detailRows.push_back(ftxui::hbox({
        styledText(" Qty ", uiSecondaryText()),
        uiBodyText(to_string(item->quantity), quantityColor),
        styledText("     Rack ", uiSecondaryText()),
        uiBodyText(rack.empty() ? "NOT ASSIGNED" : rack, rack.empty() ? uiWarnColor() : uiFocusColor()),
        ftxui::filler(),
    }));

    if (selectedMatch != nullptr) {
      const auto targetText = closestSearchActive_ ? closestSearchQuery_ : searchQuery_;
      const auto matchText = selectedMatch->band == PhysicalValueMatchBand::Exact
                                 ? string("Exact normalized value")
                                 : selectedMatch->hasPhysicalComparison
                                       ? string(physicalValueMatchBandName(selectedMatch->band))
                                       : string("Text match");
      ostringstream distance;
      if (selectedMatch->hasPhysicalComparison && selectedMatch->band != PhysicalValueMatchBand::Exact &&
          isfinite(selectedMatch->signedRelativeDifference)) {
        distance << showpos << fixed << setprecision(1) << selectedMatch->signedRelativeDifference * 100.0 << "%";
      }
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("MATCH", bandColor(selectedMatch->band), uiSurfaceBg()));
      detailRows.push_back(detailFieldLine({"Fit: ", matchText, uiSecondaryText(), bandColor(selectedMatch->band)},
                                            detailInnerWidth));
      if (!distance.str().empty()) {
        detailRows.push_back(detailFieldLine({"Delta: ", distance.str(), uiSecondaryText(), uiPrimaryText()},
                                              detailInnerWidth));
      }
      if (!targetText.empty()) {
        detailRows.push_back(detailFieldLine({"Target: ", targetText, uiSecondaryText(), uiPrimaryText()},
                                              detailInnerWidth));
      }
    }

    if (stocktakeActive_) {
      const auto counted = stocktakeCounts_.find(item->id);
      const bool alreadyCounted = counted != stocktakeCounts_.end();
      const auto physicalCount = stocktakeCountFor(*item);
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("STOCKTAKE COUNT", uiAccentColor(), uiSurfaceBg()));
      detailRows.push_back(ftxui::hbox({
          styledText(" System ", uiSecondaryText()),
          uiBodyText(to_string(item->quantity), uiPrimaryText()),
          styledText("     Count ", uiSecondaryText()),
          uiBodyText(to_string(physicalCount), alreadyCounted ? uiFocusColor() : uiWarnColor()),
          ftxui::filler(),
      }));
      detailRows.push_back(fullLine(inputMode_ == InputMode::StocktakeCount
                                        ? "Enter the physical count, then press Enter"
                                        : "Press Enter to count this part · s finish · q cancel",
                                    uiMutedColor(), uiSurfaceBg()));
      if (inputMode_ == InputMode::StocktakeCount) {
        detailRows.push_back(fullLine(" Count: " + inputBuffer_ + "_", uiLinkColor(), uiSelectionBg()));
      }
    }

    vector<const InventoryMovement*> recentMovements;
    for (const auto& movement : inventoryMovements_) {
      if (movement.itemId == item->id) {
        recentMovements.push_back(&movement);
        if (recentMovements.size() == 5) break;
      }
    }
    if (!recentMovements.empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("RECENT MOVEMENTS", uiSecondaryText(), uiSurfaceBg()));
      for (const auto* movement : recentMovements) {
        const auto delta = movement->delta > 0 ? "+" + to_string(movement->delta) : to_string(movement->delta);
        auto reference = movement->reference.empty() ? string() : " · " + movement->reference;
        const auto summary = delta + " · " + movement->source + reference + " · " +
                             to_string(movement->quantityBefore) + " → " + to_string(movement->quantityAfter) +
                             " · " + nowTimestampString(movement->occurredAt);
        detailRows.push_back(fullLine(ellipsize(summary, static_cast<size_t>(detailInnerWidth)),
                                      movement->delta < 0 ? uiWarnColor() : uiFocusColor(), uiSurfaceBg()));
      }
    }

    if (!electricalFields.empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("ELECTRICAL PARAMETERS", uiSecondaryText(), uiSurfaceBg()));
      for (const auto& field : electricalFields) {
        // The passive summary line above already prints the package value, and
        // "0603 (1608 Metric)" needs no label to be understood.
        if (passive && field.label == "Package: ") continue;
        detailRows.push_back(detailFieldLine(field, detailInnerWidth));
      }
    }

    // Detail fields carry no value when empty, so an absent one is dropped
    // rather than rendered as a dash.
    ftxui::Elements detailRowsExpanded;
    const auto detailField = [&](const string& label, const string& value, ftxui::Color valueColor) {
      if (trim(value).empty()) return;
      detailRowsExpanded.push_back(detailFieldLine({label + ": ", value, uiSecondaryText(), valueColor}, detailInnerWidth));
    };
    detailField("DigiKey", item->digikeyPartNumber, uiPrimaryText());
    detailField("SKU", item->sku, uiPrimaryText());
    detailField("Inventatory ID", item->inventatoryId, uiPrimaryText());
    detailField("Location", item->location, uiPrimaryText());
    detailField("Tags", renderTags(item->tags), uiPrimaryText());
    // Collapsible: the heading always states how many fields are hidden so the
    // block never looks like missing data.
    detailRows.push_back(uiDivider());
    detailRows.push_back(target(ftxui::hbox({
                                    styledText(stockDetailsExpanded_ ? " \xE2\x96\xBE DETAILS" : " \xE2\x96\xB8 DETAILS",
                                               uiSecondaryText()),
                                    ftxui::filler(),
                                    styledText(stockDetailsExpanded_
                                                   ? string()
                                                   : to_string(detailRowsExpanded.size()) + " fields ",
                                               uiMutedText()),
                                }) | ftxui::bgcolor(uiSurfaceBg()),
                                "stock.details.toggle", UiTargetKind::Button, [self] {
                                  self->stockDetailsExpanded_ = !self->stockDetailsExpanded_;
                                  self->dirty_ = true;
                                }));
    if (stockDetailsExpanded_) {
      for (auto& row : detailRowsExpanded) detailRows.push_back(move(row));
    }

    ftxui::Elements linkRows;
    const auto link = [&](const string& id, const string& label, const string& value, function<void()> activate) {
      if (trim(value).empty()) return;
      linkRows.push_back(target(detailFieldLine({label + ": ", renderUrl(value), uiSecondaryText(), uiInteractiveColor()},
                                                detailInnerWidth),
                                id, UiTargetKind::Link, move(activate)));
    };
    link("stock.link.datasheet", "Datasheet", item->datasheetUrl,
         [self] { if (const auto* value = self->selectedItem()) self->openCurrentUrl(value->datasheetUrl, "datasheet"); });
    link("stock.link.product", "Product", item->productUrl,
         [self] { if (const auto* value = self->selectedItem()) self->openCurrentUrl(value->productUrl, "product"); });
    if (!linkRows.empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("LINKS", uiSecondaryText(), uiSurfaceBg()));
      for (auto& row : linkRows) detailRows.push_back(move(row));
    }

    if (!trim(item->notes).empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("NOTES", uiSecondaryText(), uiSurfaceBg()));
      detailRows.push_back(ftxui::paragraphAlignLeft(item->notes) | ftxui::color(uiPrimaryText()));
    }
  } else {
    detailRows.push_back(fullLine("No item selected.", uiMutedColor(), uiPanelRightBg()));
  }

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

void App::handleClosestSearchKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    inputBuffer_.push_back(key.ch);
    closestSearchQuery_ = inputBuffer_;
    closestSelectedPosition_ = 0;
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      closestSearchQuery_ = inputBuffer_;
      closestSelectedPosition_ = 0;
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    if (!parsePhysicalValue(closestSearchQuery_).has_value()) {
      setMessage("Enter a physical value such as 4.7k, 100nF, or 1MHz", 4);
      return;
    }
    inputMode_ = InputMode::None;
    inputBuffer_.clear();
    setMessage("Closest-value results kept; select a part or press F to edit the target", 3);
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Escape) {
    clearClosestSearch();
    setMessage("Closest-value search closed", 2);
    return;
  }

  if (key.type == KeyType::Up || (key.type == KeyType::Character && key.ch == 'k')) {
    moveSelection(-1);
  } else if (key.type == KeyType::Down || (key.type == KeyType::Character && key.ch == 'j')) {
    moveSelection(1);
  } else if (key.type == KeyType::PageUp) {
    moveSelection(-10);
  } else if (key.type == KeyType::PageDown) {
    moveSelection(10);
  }
}

void App::handleStockKey(const KeyEvent& key) {
  if (closestSearchActive_ && inputMode_ == InputMode::None && key.type == KeyType::Enter) {
    openSelectedDetail();
    return;
  }
  if (stocktakeActive_) {
    if (key.type == KeyType::Escape ||
        (key.type == KeyType::Character && (key.ch == 'q' || key.ch == 'Q'))) {
      cancelStocktake();
      return;
    }
    if (key.type == KeyType::Enter) {
      beginStocktakeCount();
      return;
    }
    if (key.type == KeyType::Character && (key.ch == 's' || key.ch == 'S')) {
      finishStocktake();
      return;
    }
    if (key.type == KeyType::Character && (key.ch == 'j' || key.ch == 'J')) {
      moveSelection(1);
      return;
    }
    if (key.type == KeyType::Character && (key.ch == 'k' || key.ch == 'K')) {
      moveSelection(-1);
      return;
    }
    if (key.type == KeyType::Down) {
      moveSelection(1);
      return;
    }
    if (key.type == KeyType::Up) {
      moveSelection(-1);
      return;
    }
    if (key.type == KeyType::PageDown) {
      moveSelection(10);
      return;
    }
    if (key.type == KeyType::PageUp) {
      moveSelection(-10);
      return;
    }
    return;
  }

  if (deleteConfirmationActive()) {
    if (key.type == KeyType::Enter) {
      confirmDeleteSelectedItem();
      return;
    }
    if (key.type == KeyType::Escape) {
      cancelDeleteConfirmation();
      return;
    }

    cancelDeleteConfirmation();
    return;
  }

  if (key.type == KeyType::CtrlZ) {
    undoLastInventoryChange();
    return;
  }
  if (key.type == KeyType::Character) {
    const auto ch = tolower(static_cast<unsigned char>(key.ch));
    if (ch == 'u') {
      undoLastInventoryChange();
      return;
    }
    if (ch == 'q') {
      requestUserExit();
      return;
    }
  }

  if (key.type == KeyType::Backspace) {
    return;
  }

  if (key.type == KeyType::CtrlBackspace) {
    armDeleteConfirmation();
    return;
  }

  if (key.type == KeyType::Tab) {
    changePage(Page::Home);
    return;
  }

  if (key.type == KeyType::Character) {
    const auto ch = tolower(static_cast<unsigned char>(key.ch));
    switch (ch) {
      case 'j':
        moveSelection(1);
        break;
      case 'k':
        moveSelection(-1);
        break;
      case '/':
        startSearch();
        break;
      case 'e':
        beginEditCurrentItem(false);
        break;
      case 'n':
        beginEditCurrentItem(true);
        break;
      case 'm':
        openRackManagement();
        break;
      case '+':
        adjustQuantity(1);
        break;
      case '-':
        adjustQuantity(-1);
        break;
      case 'd':
        if (const auto* item = selectedItem()) {
          openCurrentUrl(item->datasheetUrl, "datasheet");
        }
        break;
      case 'o':
        if (const auto* item = selectedItem()) {
          openCurrentUrl(item->productUrl, "product");
        }
        break;
      case 'g':
        if (const auto* item = selectedItem()) {
          const auto digiKeySearch = item->digikeyPartNumber.empty()
                                         ? string()
                                         : "https://www.digikey.com/en/products/result?keywords=" + item->digikeyPartNumber;
          openCurrentUrl(digiKeySearch, "DigiKey");
        }
        break;
      case 'r':
        store_.load(inventoryPath_);
        loadInventoryHistory(inventoryPath_, inventoryHistory_);
        if (inventoryHistory_.empty()) {
          appendInventoryHistory(inventoryHistory_,
                                 makeInventoryHistoryPoint(store_.items(), settings_.lowStockThreshold));
        }
        saveInventoryHistory(inventoryPath_, inventoryHistory_);
        refreshInventoryMovements();
        syncSelectionToFilter();
        setMessage("Inventory refreshed", 2);
        break;
      case 'p':
        printSelectedLabel();
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Up) {
    moveSelection(-1);
  } else if (key.type == KeyType::Down) {
    moveSelection(1);
  } else if (key.type == KeyType::PageUp) {
    moveSelection(-10);
  } else if (key.type == KeyType::PageDown) {
    moveSelection(10);
  } else if (key.type == KeyType::Home) {
    if (closestSearchActive_) closestSelectedPosition_ = 0;
    else selectedPosition_ = 0;
    syncSelectionToFilter();
  } else if (key.type == KeyType::End) {
    const auto filtered = filteredIndices();
    if (!filtered.empty()) {
      if (closestSearchActive_) closestSelectedPosition_ = filtered.size() - 1;
      else selectedPosition_ = filtered.size() - 1;
      syncSelectionToFilter();
    }
  } else if (key.type == KeyType::Enter) {
    openSelectedDetail();
  } else if (key.type == KeyType::Escape) {
    changePage(Page::Home);
  }
}

void App::beginStocktake() {
  if (stocktakeActive_) {
    setMessage("Stocktake is already active", 2);
    return;
  }
  if (store_.items().empty()) {
    setMessage("Add inventory parts before starting a stocktake", 4);
    return;
  }
  closestSearchActive_ = false;
  closestSearchQuery_.clear();
  closestSelectedPosition_ = 0;
  searchQuery_.clear();
  stockDateFilter_ = StockDateFilter::All;
  stockSortOrder_ = StockSortOrder::Az;
  stocktakeCounts_.clear();
  stocktakeSessionId_ = makeId();
  stocktakeActive_ = true;
  stocktakeCommitPending_ = false;
  selectedPosition_ = 0;
  inputMode_ = InputMode::None;
  focusedTarget_ = -1;
  syncSelectionToFilter();
  setMessage("Stocktake started · count every part, then press S to finish", 5);
  dirty_ = true;
}

void App::beginStocktakeCount() {
  if (!stocktakeActive_) return;
  const auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No part selected for counting", 2);
    return;
  }
  inputBuffer_.clear();
  inputMode_ = InputMode::StocktakeCount;
  setMessage("Enter the physical count for " + item->partName, 4);
  dirty_ = true;
}

void App::handleStocktakeCountKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (isdigit(static_cast<unsigned char>(key.ch)) != 0 && inputBuffer_.size() < 10U) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) inputBuffer_.pop_back();
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Physical count cancelled", 2);
    return;
  }
  if (key.type != KeyType::Enter) return;

  if (inputBuffer_.empty()) {
    setMessage("Enter a whole number from 0 to 2147483647", 4);
    return;
  }
  long long parsed = -1;
  try {
    size_t consumed = 0;
    parsed = stoll(inputBuffer_, &consumed);
    if (consumed != inputBuffer_.size() || parsed < 0 || parsed > numeric_limits<int>::max()) parsed = -1;
  } catch (...) {
    parsed = -1;
  }
  if (parsed < 0) {
    setMessage("Count must be a whole number from 0 to 2147483647", 4);
    return;
  }

  auto* item = selectedItem();
  if (item == nullptr) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("The selected part is no longer available", 3);
    return;
  }
  stocktakeCounts_[item->id] = static_cast<int>(parsed);
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  const auto itemName = item->partName;
  moveSelection(1);
  setMessage(itemName + " counted as " + to_string(parsed), 3);
  dirty_ = true;
}

void App::cancelStocktake() {
  if (!stocktakeActive_) return;
  const bool hadPendingCommit = stocktakeCommitPending_;
  bool revertedUnsavedChanges = false;
  bool activitySaveFailed = false;
  if (stocktakeCommitPending_ && !pendingMovementSource_.empty() && undoSnapshot_.valid) {
    store_.items() = undoSnapshot_.items;
    store_.racks() = undoSnapshot_.racks;
    activities_ = undoSnapshot_.activities;
    const bool activitiesSaved = saveActivitiesChecked(false);
    activitySaveFailed = !activitiesSaved;
    undoSnapshot_.valid = false;
    pendingMovementSource_.clear();
    pendingMovementReference_.clear();
    if (activitiesSaved) persistenceError_.clear();
    revertedUnsavedChanges = true;
  }
  stocktakeActive_ = false;
  stocktakeCommitPending_ = false;
  stocktakeSessionId_.clear();
  stocktakeCounts_.clear();
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  setMessage(activitySaveFailed
                 ? "Stocktake cancelled; inventory changes were discarded, but activity history was not saved; press R to retry"
                 : revertedUnsavedChanges ? "Stocktake cancelled; unsaved inventory changes were discarded"
                                          : hadPendingCommit ? "Stocktake closed; inventory changes were already saved"
                                                             : "Stocktake cancelled; inventory was not changed",
             activitySaveFailed ? 6 : 4);
  dirty_ = true;
}

void App::finishStocktake() {
  if (!stocktakeActive_) return;
  if (stocktakeCountedItems() != store_.items().size()) {
    setMessage("Count every part before finishing the stocktake", 4);
    return;
  }

  if (!stocktakeCommitPending_) {
    captureUndoSnapshot();
    const auto now = time(nullptr);
    int changed = 0;
    for (auto& item : store_.items()) {
      const auto count = stocktakeCounts_.find(item.id);
      if (count == stocktakeCounts_.end() || count->second == item.quantity) continue;
      item.quantity = count->second;
      item.lastUpdated = now;
      reconcileRackAssignment(store_, item);
      ++changed;
    }
    logActivity("stocktake", "Physical count completed · " + to_string(changed) + " parts adjusted");
  }

  if (!saveState("stocktake", stocktakeSessionId_)) {
    stocktakeCommitPending_ = true;
    setMessage("Stocktake changes are in memory; press R to retry saving or Q to discard", 6);
    dirty_ = true;
    return;
  }
  const auto countedParts = stocktakeCountedItems();
  stocktakeActive_ = false;
  stocktakeCommitPending_ = false;
  stocktakeSessionId_.clear();
  stocktakeCounts_.clear();
  inputMode_ = InputMode::None;
  setMessage("Stocktake saved; " + to_string(countedParts) + " parts counted", 5);
  dirty_ = true;
}

int App::stocktakeCountFor(const InventoryItem& item) const {
  const auto it = stocktakeCounts_.find(item.id);
  return it == stocktakeCounts_.end() ? item.quantity : it->second;
}

size_t App::stocktakeCountedItems() const {
  return stocktakeCounts_.size();
}

}  // namespace inventatory

