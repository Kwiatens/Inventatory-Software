// Inventatory - Modal input handlers.

#include "App.h"

#include "platform/DigiKeyApi.h"
#include "platform/CredentialStore.h"
#include "platform/StartupRegistration.h"
#include "platform/UpdateService.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <thread>
#include <future>

namespace inventatory {

using namespace std;

void App::handleSearchKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    inputBuffer_.push_back(key.ch);
    searchQuery_ = inputBuffer_;
    syncSelectionToFilter();
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      searchQuery_ = inputBuffer_;
      syncSelectionToFilter();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    inputMode_ = InputMode::None;
    syncSelectionToFilter();
    setMessage(searchQuery_.empty() ? "Filter cleared" : "Filter kept", 2);
    return;
  }

  if (key.type == KeyType::Escape) {
    searchQuery_ = searchQueryBeforeEdit_;
    inputBuffer_ = searchQuery_;
    inputMode_ = InputMode::None;
    syncSelectionToFilter();
    setMessage("Search cancelled", 2);
  }
}

void App::handleEditMenuKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (key.ch == 's' || key.ch == 'S') {
      saveWorkingCopy();
    }
    return;
  }

  if (key.type == KeyType::Up) {
    fieldMenuIndex_ = max(0, fieldMenuIndex_ - 1);
    dirty_ = true;
  } else if (key.type == KeyType::Down) {
    fieldMenuIndex_ = min(fieldMenuIndex_ + 1, static_cast<int>(menuOptions_.size()) - 1);
    dirty_ = true;
  } else if (key.type == KeyType::Enter) {
    if (fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
      inputBuffer_ = currentFieldValue(menuOptions_[fieldMenuIndex_].field);
      inputMode_ = InputMode::EditValue;
      setMessage("Editing " + fieldLabel(menuOptions_[fieldMenuIndex_].field), 2);
    }
  } else if (key.type == KeyType::Escape) {
    cancelInput();
  }
}

void App::handleEditValueKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    inputBuffer_.push_back(key.ch);
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    if (fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
      commitEditField(menuOptions_[fieldMenuIndex_].field, inputBuffer_);
    }
    inputBuffer_.clear();
    inputMode_ = InputMode::EditFieldMenu;
    return;
  }

  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::EditFieldMenu;
    setMessage("Edit cancelled", 2);
  }
}

void App::handleRackValueKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    inputBuffer_.push_back(key.ch);
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    const auto value = inputBuffer_;
    const auto mode = inputMode_;
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    switch (mode) {
      case InputMode::RackRename:
        renameSelectedRack(value);
        break;
      case InputMode::RackType:
        changeSelectedRackType(value);
        break;
      case InputMode::RackCreate:
        createRackWithType(value);
        break;
      case InputMode::RackJump:
        jumpToRack(value);
        break;
      case InputMode::RackFilter:
        rackFilter_ = trim(value);
        syncRackSelection();
        setMessage(rackFilter_.empty() ? "Rack filter cleared" : "Rack filter applied", 2);
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Rack input cancelled", 2);
  }
}

void App::handleQuantityAdjustKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (isdigit(static_cast<unsigned char>(key.ch)) != 0 && inputBuffer_.size() < 9U) {
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
  if (key.type == KeyType::Enter) {
    const auto value = inputBuffer_;
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setSelectedQuantityFromInput(value);
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Quantity change cancelled", 2);
  }
}

void App::handleBomRestockKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (isdigit(static_cast<unsigned char>(key.ch)) != 0 && inputBuffer_.size() < 9U) {
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
    bomRestockItemId_.clear();
    inputMode_ = InputMode::None;
    setMessage("Stock receipt cancelled", 2);
    return;
  }
  if (key.type != KeyType::Enter) return;

  const auto text = inputBuffer_;
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  auto* item = store_.findById(bomRestockItemId_);
  bomRestockItemId_.clear();
  if (item == nullptr) {
    setMessage("The matched stock item no longer exists; add it manually from Stock", 5);
    return;
  }
  int received = 0;
  try {
    size_t parsed = 0;
    received = stoi(text, &parsed);
    if (parsed != text.size() || received <= 0) throw invalid_argument("quantity");
  } catch (...) {
    setMessage("Enter a positive received quantity", 3);
    return;
  }

  captureUndoSnapshot();
  const auto before = item->quantity;
  item->quantity = before > numeric_limits<int>::max() - received ? numeric_limits<int>::max() : before + received;
  item->lastUpdated = time(nullptr);
  reconcileRackAssignment(store_, *item);
  logActivity("receipt", item->partName + " received " + to_string(received) + " (now " +
                              to_string(item->quantity) + ")");
  const bool saved = saveState("stock_receipt", activeBomProjectId_);
  refreshBomAnalysis();
  setMessage(saved ? item->partName + " receipt saved; BOM re-analyzed"
                   : "Receipt is in memory; press R to retry saving, then recheck the BOM",
             6);
  dirty_ = true;
}

void App::handleExitConfirmationKey(const KeyEvent& key) {
  if (key.type == KeyType::Escape) {
    pendingPageAfterSettings_.reset();
    inputMode_ = InputMode::None;
    setMessage("Stayed on the current screen", 2);
    return;
  }
  if (key.type != KeyType::Character) return;
  const auto ch = static_cast<char>(tolower(static_cast<unsigned char>(key.ch)));
  if (exitSavePending_) {
    if (ch == 's') {
      if (saveState()) {
        exitSavePending_ = false;
        inputMode_ = InputMode::None;
        requestUserExit();
      }
    } else if (ch == 'd') {
      exitSavePending_ = false;
      inputMode_ = InputMode::None;
      setMessage("Exiting with unsaved data; retry persistence from the previous session", 8);
      running_ = false;
    }
    return;
  }
  if (ch == 's') {
    completeSettingsExit(true);
  } else if (ch == 'd') {
    completeSettingsExit(false);
  }
}

}  // namespace inventatory
