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

ftxui::Element App::renderStockUi() const {
  const auto filtered = filteredIndices();
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int detailOuterWidth = clamp(screenWidth / 3, 42, 60);
  const int listOuterWidth = max(42, screenWidth - detailOuterWidth - 1);
  const int listInnerWidth = max(20, listOuterWidth - 2);
  const int detailInnerWidth = max(20, detailOuterWidth - 2);
  const int qtyWidth = 10;
  int partWidth = clamp(listInnerWidth / 3, 22, 34);
  int categoryWidth = listInnerWidth - partWidth - qtyWidth - 2;
  if (categoryWidth < 12) {
    categoryWidth = 12;
    partWidth = max(18, listInnerWidth - categoryWidth - qtyWidth - 2);
  }
  if (partWidth < 18) {
    partWidth = 18;
  }

  auto fixedCell = [](const string& text, int width, ftxui::Color color) {
    return ftxui::hbox({
               styledText(ellipsize(text, static_cast<size_t>(max(width, 0))), color),
               ftxui::filler(),
           }) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
  };

  ftxui::Elements listRows;
  listRows.push_back(ftxui::hbox({
                         fixedCell("Part", partWidth, uiMutedColor()),
                         ftxui::separator() | ftxui::color(uiDimColor()),
                         fixedCell("Category", categoryWidth, uiMutedColor()),
                         ftxui::filler(),
                         ftxui::hbox({
                             ftxui::filler(),
                             styledText("Qty", uiMutedColor()),
                         }) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth),
                     }) |
                     ftxui::bgcolor(uiPanelLeftBg()));

  if (filtered.empty()) {
    listRows.push_back(fullLine("No items match \"" + searchQuery_ + "\".", uiMutedColor(), uiPanelLeftBg()));
  } else {
    for (size_t index = 0; index < filtered.size(); ++index) {
      const auto& item = store_.items()[filtered[index]];
      const bool selected = index == selectedPosition_;
      const bool lowStock = item.lowStock();
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto fg = selected ? uiFocusColor() : (lowStock ? uiWarnColor() : uiPrimaryText());
      const auto category = displayCategory(item.category);
      auto row = ftxui::hbox({
                     fixedCell("  " + item.partName, partWidth, fg),
                     ftxui::separator() | ftxui::color(uiDimColor()),
                     fixedCell(category, categoryWidth, selected ? uiTitleColor() : uiLabelColor()),
                     ftxui::filler(),
                     ftxui::hbox({
                         ftxui::filler(),
                         quantityBadge(item.quantity, selected),
                     }) |
                         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, qtyWidth),
                 }) |
                 ftxui::bgcolor(bg);
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
  if (!editing) {
    const bool hasItem = selectedItem() != nullptr;
    detailRows.push_back(ftxui::hbox({
        target(styledText(" New ", uiInteractiveColor(), uiRaisedSurfaceBg()), "stock.new", UiTargetKind::Button,
               [self] { self->beginEditCurrentItem(true); }),
        ftxui::text(" "),
        target(styledText(" Edit ", hasItem ? uiInteractiveColor() : uiMutedText(), uiRaisedSurfaceBg()),
               "stock.edit", UiTargetKind::Button, [self] { self->beginEditCurrentItem(false); }, hasItem),
        ftxui::text(" "),
        target(styledText(" - ", hasItem ? uiInteractiveColor() : uiMutedText(), uiRaisedSurfaceBg()),
               "stock.decrement", UiTargetKind::Button, [self] { self->adjustQuantity(-1); }, hasItem),
        target(styledText(" + ", hasItem ? uiInteractiveColor() : uiMutedText(), uiRaisedSurfaceBg()),
               "stock.increment", UiTargetKind::Button, [self] { self->adjustQuantity(1); }, hasItem),
        ftxui::text(" "),
        target(styledText(" Print ", hasItem ? uiInteractiveColor() : uiMutedText(), uiRaisedSurfaceBg()),
               "stock.print", UiTargetKind::Button, [self] { self->printSelectedLabel(); }, hasItem),
        ftxui::filler(),
      }));
    detailRows.push_back(ftxui::separator() | ftxui::color(uiDividerColor()));
  }
  if (editing) {
    const auto titleText = workingCopy_.isNew ? string("New part") : workingCopy_.item.partName;
    detailRows.push_back(fullLine(titleText, uiTitleColor(), uiRowSelectedBg()));
    detailRows.push_back(
        styledText(" \xE2\x86\x91\xE2\x86\x93 field  \xE2\x8F\x8E edit  s save  esc cancel", uiMutedColor()));
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
    detailRows.push_back(fullLine(item->partName, uiPrimaryText(), uiSurfaceBg()) | ftxui::bold);
    detailRows.push_back(styledText(partShortDescription(*item), uiSecondaryText()));
    detailRows.push_back(ftxui::separator() | ftxui::color(uiDividerColor()));
    const auto electricalFields = electricalFieldsForItem(*item);
    const auto coreFields = detailCoreFields(*item, rackLocation(*item, store_.racks()));
    for (size_t index = 0; index < coreFields.size(); ++index) {
      const auto& field = coreFields[index];
      if (index == 0) {
        detailRows.push_back(fullLine(field.label + field.value, uiFocusColor(), uiSelectionBg()));
        continue;
      }
      detailRows.push_back(detailFieldLine(field, detailInnerWidth));
    }
    if (!electricalFields.empty()) {
      detailRows.push_back(ftxui::separator() | ftxui::color(uiDividerColor()));
      detailRows.push_back(fullLine("Electrical parameters", uiSecondaryText(), uiSurfaceBg()));
      for (const auto& field : electricalFields) detailRows.push_back(detailFieldLine(field, detailInnerWidth));
    }
    detailRows.push_back(ftxui::separator() | ftxui::color(uiDividerColor()));
    const auto link = [&](const string& id, const string& label, const string& value, function<void()> activate) {
      return target(detailFieldLine({label + ": ", renderUrl(value), uiSecondaryText(), uiInteractiveColor()},
                                    detailInnerWidth),
                    id, UiTargetKind::Link, move(activate), !trim(value).empty());
    };
    detailRows.push_back(link("stock.link.datasheet", "Datasheet", item->datasheetUrl,
                              [self] { if (const auto* value = self->selectedItem()) self->openCurrentUrl(value->datasheetUrl, "datasheet"); }));
    detailRows.push_back(link("stock.link.product", "Product", item->productUrl,
                              [self] { if (const auto* value = self->selectedItem()) self->openCurrentUrl(value->productUrl, "product"); }));
    detailRows.push_back(detailFieldLine({"DigiKey: ", item->digikeyPartNumber, uiSecondaryText(), uiPrimaryText()},
                                         detailInnerWidth));
    detailRows.push_back(detailFieldLine({"SKU: ", item->sku, uiSecondaryText(), uiPrimaryText()}, detailInnerWidth));
    detailRows.push_back(detailFieldLine({"Tags: ", renderTags(item->tags), uiSecondaryText(), uiPrimaryText()}, detailInnerWidth));
    if (!trim(item->notes).empty()) {
      detailRows.push_back(ftxui::separator() | ftxui::color(uiDividerColor()));
      detailRows.push_back(styledText("Notes", uiSecondaryText()));
      detailRows.push_back(ftxui::paragraphAlignLeft(item->notes) | ftxui::color(uiPrimaryText()));
    }
  } else {
    detailRows.push_back(fullLine("No item selected.", uiMutedColor(), uiPanelRightBg()));
  }

  auto filterButton = target(styledText(" Filter ", uiInteractiveColor(), uiRaisedSurfaceBg()),
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
      filterRows.push_back(fullLine("< Date of modification", uiSecondaryText(), uiPanelLeftBg()));
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
      const vector<string> labels = {"Date of modification", "Quantity", "A-Z", "Z-A"};
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
    filterMenu = ftxui::window(styledText(" Filters: ", uiAccentColor()),
                               ftxui::vbox(move(filterRows)) | ftxui::bgcolor(uiPanelLeftBg()) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 34)) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 34) |
                 ftxui::color(uiAccentColor()) | ftxui::bgcolor(uiPanelLeftBg()) |
                 ftxui::clear_under;
  }

  ftxui::Element listPanel = ftxui::vbox(move(listRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                             ftxui::bgcolor(uiSurfaceBg()) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listOuterWidth);
  if (inputMode_ == InputMode::StockFilter) {
    auto filterOverlay = ftxui::vbox({
        ftxui::text(" "),
        ftxui::hbox({ftxui::filler(), filterMenu}),
        ftxui::filler(),
    });
    listPanel = ftxui::dbox({listPanel, filterOverlay}) |
                ftxui::bgcolor(uiSurfaceBg()) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listOuterWidth);
  }
  detailRows.insert(detailRows.begin(), fullLine(editing ? "EDIT ITEM" : "ITEM DETAIL", uiSecondaryText(), uiSurfaceBg()));
  auto detailPanel = ftxui::vbox(move(detailRows)) | ftxui::yframe | ftxui::vscroll_indicator |
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
          appendInventoryHistory(inventoryHistory_, makeInventoryHistoryPoint(store_.items()));
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

