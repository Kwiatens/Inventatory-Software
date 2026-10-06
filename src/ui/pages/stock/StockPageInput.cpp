// Inventatory - Stock page-local input routing.

#include "App.h"

#include "core/parts/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

void App::handleClosestSearchKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    appendKeyText(inputBuffer_, key);
    closestSearchQuery_ = inputBuffer_;
    closestSelectedPosition_ = 0;
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      eraseLastCharacter(inputBuffer_);
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
    changePage(Page::Stock);
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
    changePage(Page::Stock);
  }
}

}  // namespace inventatory
