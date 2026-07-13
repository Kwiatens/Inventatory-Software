// HIMS - Hardware Inventory Management System
// Detail page rendering and keyboard handling.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace hims {

using namespace std;

ftxui::Element App::renderDetailUi() const {
  ftxui::Elements leftRows;
  ftxui::Elements rightRows;

  if (const auto* item = selectedItem()) {
    const auto electricalFields = electricalFieldsForItem(*item);
    leftRows.push_back(fullLine("Core details", uiAccentColor(), uiPanelLeftBg()));
    const auto coreFields = detailCoreFields(*item, rackLocation(*item, store_.racks()));
    for (size_t index = 0; index < coreFields.size(); ++index) {
      if (index == 0) {
        leftRows.push_back(fullLine(coreFields[index].label + coreFields[index].value, uiTitleColor(), uiRowSelectedBg()));
      } else {
        leftRows.push_back(detailFieldLine(coreFields[index], 40));
      }
    }
    if (!electricalFields.empty()) {
      leftRows.push_back(uiDivider());
      leftRows.push_back(fullLine("Electrical parameters", uiAccentColor(), uiPanelLeftBg()));
      for (const auto& field : electricalFields) {
        leftRows.push_back(detailFieldLine(field, 40));
      }
    }
    if (!item->tags.empty()) {
      leftRows.push_back(uiDivider());
      leftRows.push_back(detailFieldLine({"Tags: ", renderTags(item->tags), uiLabelColor(), uiTitleColor()}, 40));
    }

    rightRows.push_back(fullLine("Metadata", uiAccentColor(), uiPanelRightBg()));
    rightRows.push_back(bulletLine("DigiKey: ", renderUrl(item->digikeyPartNumber), uiLinkColor(), uiTitleColor()));
    rightRows.push_back(bulletLine("Datasheet: ", renderUrl(item->datasheetUrl), uiLinkColor(), uiTitleColor()));
    rightRows.push_back(bulletLine("Product: ", renderUrl(item->productUrl), uiLinkColor(), uiTitleColor()));
    rightRows.push_back(bulletLine("SKU: ", renderUrl(item->sku), uiLabelColor(), uiTitleColor()));
    rightRows.push_back(bulletLine("Status: ", item->syncStatus, uiSuccessColor(), uiTitleColor()));
    rightRows.push_back(bulletLine("Updated: ", nowTimestampString(item->lastUpdated), uiMutedColor(), uiTitleColor()));
    rightRows.push_back(uiDivider());
    rightRows.push_back(fullLine("Notes", uiWarnColor(), uiPanelRightBg()));
    rightRows.push_back(ftxui::paragraphAlignLeft(item->notes) | ftxui::color(uiMutedColor()));
  } else {
    leftRows.push_back(fullLine("No item selected.", uiMutedColor(), uiPanelLeftBg()));
    rightRows.push_back(fullLine("Press Esc to return to stock.", uiMutedColor(), uiPanelRightBg()));
  }

  auto leftPanel = panel("Item detail", move(leftRows), uiAccentColor()) | ftxui::bgcolor(uiPanelLeftBg()) | ftxui::flex;
  auto rightPanel = panel("Links and notes", move(rightRows), uiAccentColor()) | ftxui::bgcolor(uiPanelRightBg()) | ftxui::flex;

  return ftxui::hbox({
      leftPanel,
      uiDivider(),
      rightPanel,
  });
}

void App::handleDetailKey(const KeyEvent& key) {
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
      running_ = false;
      return;
    }
    switch (ch) {
      case 'e':
        beginEditCurrentItem(false);
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
      case 'p':
        printSelectedLabel();
        break;
      case 'j':
        moveSelection(1);
        break;
      case 'k':
        moveSelection(-1);
        break;
      case '/':
        startSearch();
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Escape) {
    changePage(Page::Stock);
  }
}

}  // namespace hims

