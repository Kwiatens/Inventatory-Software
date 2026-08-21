// Inventatory - Hardware Inventory Management System
// Stock browser page rendering and keyboard handling.

#include "App.h"

#include "core/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
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
  const auto filtered = filteredIndices();
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
  const bool groupByCategory = stockSortOrder_ != StockSortOrder::Quantity;
  const int minCategoryWidth = 12;
  const int maxPartWidth = groupByCategory ? max(18, listInnerWidth - qtyWidth - 1)
                                           : max(18, listInnerWidth - qtyWidth - minCategoryWidth - 2);
  const int partWidth = groupByCategory ? maxPartWidth : clamp(static_cast<int>(longestPartName) + 3, 18, maxPartWidth);
  // The fallback column is sized to its longest value and sits beside the part
  // name; the leftover width becomes one gutter before the right-anchored
  // quantity, rather than padding out the category cell.
  const int categoryWidth = clamp(static_cast<int>(longestCategory) + 2, minCategoryWidth,
                                  max(minCategoryWidth, listInnerWidth - partWidth - qtyWidth - 2));

  auto fixedCell = [](const string& text, int width, ftxui::Color color, CellAlign align = CellAlign::Left) {
    const auto content = styledText(ellipsize(text, static_cast<size_t>(max(width, 0))), color);
    ftxui::Elements parts;
    if (align != CellAlign::Left) parts.push_back(ftxui::filler());
    parts.push_back(content);
    if (align != CellAlign::Right) parts.push_back(ftxui::filler());
    return ftxui::hbox(move(parts)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
  };
  const auto quantityCell = [&](int quantity, bool selected) {
    InventoryItem quantityItem;
    quantityItem.quantity = quantity;
    const auto fg = quantity <= 0 ? uiDangerColor()
                                  : isLowStock(quantityItem, settings_.lowStockThreshold) ? uiWarnColor()
                                                                                           : uiSuccessColor();
    const auto bg = selected ? uiSelectionBg()
                    : quantity <= 0 ? uiDangerBg()
                    : isLowStock(quantityItem, settings_.lowStockThreshold) ? uiWarningBg()
                                    : uiRaisedSurfaceBg();
    return ftxui::hbox({
        ftxui::filler(),
        styledText(to_string(quantity), fg) | ftxui::bold,
        ftxui::text(" "),
    }) | ftxui::bgcolor(bg) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth);
  };

  const auto qtyHeaderCell = ftxui::hbox({
                                 ftxui::filler(),
                                 styledText("Qty", uiMutedColor()),
                                 ftxui::text(" "),
                             }) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth);

  ftxui::Elements listRows;
  if (groupByCategory) {
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
    return ftxui::hbox({
               move(partCell),
               ftxui::separator() | ftxui::color(uiDimColor()),
               ftxui::text("") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth),
           }) |
           ftxui::bgcolor(background);
  };

  if (filtered.empty()) {
    listRows.push_back(fullLine("No items match \"" + searchQuery_ + "\".", uiMutedColor(), uiPanelLeftBg()));
  } else {
    for (size_t index = 0; index < filtered.size(); ++index) {
      const auto& item = store_.items()[filtered[index]];
      const auto category = displayCategory(item.category);
      if (groupByCategory) {
        const bool startsGroup =
            index == 0 || category != displayCategory(store_.items()[filtered[index - 1]].category);
        if (startsGroup) {
          // A blank spacer sets each group apart; the first needs none because
          // the column header sits directly above it.
          if (index != 0) {
            listRows.push_back(
                ruledRow(ftxui::text("") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth), uiSurfaceBg()));
          }
          listRows.push_back(ruledRow(fixedCell(" " + toUpper(category), partWidth, uiSecondaryText()) | ftxui::bold,
                                      uiRaisedSurfaceBg()));
        }
      }
      const bool selected = index == selectedPosition_;
      const bool outOfStock = item.quantity <= 0;
      const bool lowStock = isLowStock(item, settings_.lowStockThreshold);
      const auto bg = selected ? uiSelectionBg()
                      : outOfStock ? uiDangerBg()
                                   : (index % 2 == 0 ? uiCanvasBg() : uiSurfaceBg());
      const auto fg = selected ? uiFocusColor()
                      : outOfStock ? uiDangerColor()
                                   : (lowStock ? uiWarnColor() : uiPrimaryText());
      ftxui::Elements cells = {fixedCell(groupByCategory ? "   " + item.partName : " " + item.partName, partWidth, fg),
                               ftxui::separator() | ftxui::color(uiDimColor())};
      if (!groupByCategory) {
        cells.push_back(fixedCell(category, categoryWidth, selected ? uiTitleColor() : uiLabelColor()));
        cells.push_back(ftxui::filler());
        cells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
      }
      cells.push_back(quantityCell(item.quantity, selected));
      auto row = ftxui::hbox(move(cells)) | ftxui::bgcolor(bg);
      if (selected) {
        row = row | ftxui::select;
      }
      auto self = const_cast<App*>(this);
      const auto position = index;
      listRows.push_back(target(row, "stock.row." + item.id, UiTargetKind::Row, [self, position] {
        self->selectedPosition_ = position;
        self->syncSelectionToFilter();
        self->dirty_ = true;
      }));
    }
  }

  // Editing renders inline here instead of a separate blocking field-name menu:
  // the Detail panel becomes the form, current field highlighted, live typing
  // shown in place. Save ('s') and cancel (Esc) act on the whole form.
  const bool editing = inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue;

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
    detailRows.push_back(styledText(manufacturer, uiPrimaryText()) | ftxui::bold);
    if (!passive) {
      detailRows.push_back(styledText(item->partName, uiSecondaryText()));
    } else {
      const auto primary = electricalFields.empty() ? item->partName : electricalFields.front().value;
      ftxui::Elements summary = {styledText(primary, uiPrimaryText()) | ftxui::bold};
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
    // the summary instead of at the bottom of an identity list.
    const auto rack = rackLocation(*item, store_.racks());
    const auto quantityColor = item->quantity <= 0 ? uiDangerColor()
                               : isLowStock(*item, settings_.lowStockThreshold) ? uiWarnColor()
                                                  : uiPrimaryText();
    detailRows.push_back(uiDivider());
    detailRows.push_back(ftxui::hbox({
        styledText(" Qty ", uiSecondaryText()),
        styledText(to_string(item->quantity), quantityColor) | ftxui::bold,
        styledText("     Rack ", uiSecondaryText()),
        styledText(rack.empty() ? "NOT ASSIGNED" : rack, rack.empty() ? uiWarnColor() : uiFocusColor()) | ftxui::bold,
        ftxui::filler(),
    }));

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

    // Identity fields carry no value when empty, so an absent one is dropped
    // rather than rendered as a dash.
    ftxui::Elements identityRows;
    const auto identityField = [&](const string& label, const string& value, ftxui::Color valueColor) {
      if (trim(value).empty()) return;
      identityRows.push_back(detailFieldLine({label + ": ", value, uiSecondaryText(), valueColor}, detailInnerWidth));
    };
    identityField("DigiKey", item->digikeyPartNumber, uiPrimaryText());
    identityField("SKU", item->sku, uiPrimaryText());
    identityField("Inventatory ID", item->inventatoryId, uiPrimaryText());
    identityField("Location", item->location, uiPrimaryText());
    identityField("Sync", item->syncStatus,
                  toLower(item->syncStatus) == "synced" ? uiSuccessColor() : uiWarnColor());
    identityField("Tags", renderTags(item->tags), uiPrimaryText());
    // Collapsible: the heading always states how many fields are hidden so the
    // block never looks like missing data.
    detailRows.push_back(uiDivider());
    detailRows.push_back(target(ftxui::hbox({
                                    styledText(stockIdentityExpanded_ ? " \xE2\x96\xBE IDENTITY" : " \xE2\x96\xB8 IDENTITY",
                                               uiSecondaryText()),
                                    ftxui::filler(),
                                    styledText(stockIdentityExpanded_
                                                   ? string()
                                                   : to_string(identityRows.size()) + " fields ",
                                               uiMutedText()),
                                }) | ftxui::bgcolor(uiSurfaceBg()),
                                "stock.identity.toggle", UiTargetKind::Button, [self] {
                                  self->stockIdentityExpanded_ = !self->stockIdentityExpanded_;
                                  self->dirty_ = true;
                                }));
    if (stockIdentityExpanded_) {
      for (auto& row : identityRows) detailRows.push_back(move(row));
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

  auto filterButton = target(styledText(" Sort / Filter ", uiInteractiveColor(), uiRaisedSurfaceBg()),
                             "stock.filters.header", UiTargetKind::Button,
                             [self] { self->openStockFilterPanel(); });
  listRows.insert(listRows.begin(), ftxui::hbox({
      styledText("STOCK  " + to_string(filtered.size()) + " items", uiSecondaryText()),
      ftxui::filler(),
      filterButton,
  }) | ftxui::bgcolor(uiSurfaceBg()));

  ftxui::Element filterMenu = ftxui::text("");
  if (inputMode_ == InputMode::StockFilter) {
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
  if (inputMode_ == InputMode::StockFilter) {
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

void App::handleStockKey(const KeyEvent& key) {
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
    selectedPosition_ = 0;
    syncSelectionToFilter();
  } else if (key.type == KeyType::End) {
    const auto filtered = filteredIndices();
    if (!filtered.empty()) {
      selectedPosition_ = filtered.size() - 1;
      syncSelectionToFilter();
    }
  } else if (key.type == KeyType::Enter) {
    openSelectedDetail();
  } else if (key.type == KeyType::Escape) {
    changePage(Page::Home);
  }
}

}  // namespace inventatory

