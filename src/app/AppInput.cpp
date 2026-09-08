// Inventatory - Application keyboard and mouse input routing.

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

void App::handleKey(const KeyEvent& key) {
  if (inventoryRecoveryRequired_) {
    if (key.type == KeyType::Escape) running_ = false;
    if (key.type == KeyType::Character && (key.ch == 'd' || key.ch == 'D')) chooseInventatoryFolder();
    dirty_ = true;
    return;
  }
  if (wizardTransition_.phase != WizardTransitionPhase::None) {
    if (!bufferedWizardKey_.has_value()) bufferedWizardKey_ = key;
    return;
  }
  switch (inputMode_) {
    case InputMode::Search:
      handleSearchKey(key);
      return;
    case InputMode::ClosestSearch:
      handleClosestSearchKey(key);
      return;
    case InputMode::EditFieldMenu:
      handleEditMenuKey(key);
      return;
    case InputMode::EditValue:
      handleEditValueKey(key);
      return;
    case InputMode::RackRename:
    case InputMode::RackType:
    case InputMode::RackCreate:
    case InputMode::RackJump:
    case InputMode::RackFilter:
      handleRackValueKey(key);
      return;
    case InputMode::StockFilter:
      handleStockFilterKey(key);
      return;
    case InputMode::StocktakeCount:
      handleStocktakeCountKey(key);
      return;
    case InputMode::QuantityAdjust:
      handleQuantityAdjustKey(key);
      return;
    case InputMode::BomRestock:
      handleBomRestockKey(key);
      return;
    case InputMode::HistoryCheckpoint:
    case InputMode::HistoryConfirm:
      handleHistoryKey(key);
      return;
    case InputMode::ExitConfirmation:
      handleExitConfirmationKey(key);
      return;
    case InputMode::ActionSheet:
      handleActionSheetKey(key);
      return;
    case InputMode::None:
      break;
  }

  // Settings fields use their own staged editor rather than a shared
  // InputMode. While it is active, all characters (including 1-5) and Enter
  // belong to the field and must not trigger global navigation or focus.
  if (page_ == Page::Settings && (settingsEditingField_ || appearancePickerOpen_)) {
    handleSettingsKey(key);
    return;
  }

  // The setup wizard owns every key while open: its Wi-Fi password and the
  // six-digit verification code must never be interpreted as global shortcuts.
  if (page_ == Page::ScanSetup) {
    handleInventatoryScanSetupKey(key);
    return;
  }

  if (page_ == Page::DigiKeySetup) {
    handleDigiKeySetupKey(key);
    return;
  }

  if (page_ == Page::Onboarding) {
    handleOnboardingKey(key);
    return;
  }

  if (page_ == Page::Import && importCommitPending_ && key.type == KeyType::Character && key.ch == 'R') {
    finishImportReview();
    return;
  }
  if (key.type == KeyType::Character && key.ch == 'R' && !persistenceError_.empty()) {
    retrySaveState();
    return;
  }

  // Delete confirmation is modal. Route Enter/Escape to the confirmation
  // handler before the normal focus system can activate an underlying target.
  if (page_ == Page::Stock && deleteConfirmationActive()) {
    handleStockKey(key);
    return;
  }

  if (key.type == KeyType::Tab) {
    moveUiFocus(1);
    return;
  }
  if (key.type == KeyType::TabReverse) {
    moveUiFocus(-1);
    return;
  }
  if (key.type == KeyType::Enter && activateFocusedTarget()) {
    return;
  }

  if (key.type == KeyType::Character) {
    switch (key.ch) {
      case '1': changePage(Page::Home); return;
      case '2': changePage(Page::Stock); return;
      case '3': changePage(Page::Racks); return;
      case '4': changePage(Page::Import); return;
      case '5': changePage(Page::Projects); return;
      case '6': changePage(Page::History); return;
      case '7': changePage(Page::Settings); return;
      default: break;
    }
  }

  // Open the action sheet, or run a contextual action by its accelerator.
  // Registry actions win over per-page handlers, keeping one source of truth.
  if (key.type == KeyType::Character && key.ch == ' ') {
    openActionSheet();
    return;
  }
  if (dispatchAction(key)) {
    return;
  }

  switch (page_) {
    case Page::Home:
      handleDashboardKey(key);
      break;
    case Page::Stock:
      handleStockKey(key);
      break;
    case Page::Racks:
      handleRackManagementKey(key);
      break;
    case Page::Import:
      handleImportCsvKey(key);
      break;
    case Page::Projects:
      handleBomProjectKey(key);
      break;
    case Page::History:
      handleHistoryKey(key);
      break;
    case Page::ScanSetup:
      handleInventatoryScanSetupKey(key);
      break;
    case Page::DigiKeySetup:
      handleDigiKeySetupKey(key);
      break;
    case Page::Settings:
      handleSettingsKey(key);
      break;
  }
}

ftxui::Element App::target(ftxui::Element element, string id, UiTargetKind kind,
                           function<void()> activate, bool enabled, bool focusable) const {
  const auto index = uiTargets_.size();
  const bool hovered = hoveredTargetId_ == id;
  const bool focused = focusable && static_cast<int>(index) == focusedTarget_;
  if (hovered) element = element | ftxui::bgcolor(uiHoverBg());
  if (focused) element = element | ftxui::color(uiFocusColor()) | ftxui::bgcolor(uiSelectionBg()) | ftxui::bold;
  uiTargets_.push_back(UiTarget{move(id), kind, {}, enabled, focusable, move(activate)});
  return element | ftxui::reflect(uiTargets_.back().bounds);
}

bool App::handleMouse(const ftxui::Mouse& mouse) {
  const auto contains = [&](const UiTarget& target) {
    return uiBoxContains(target.bounds, mouse.x, mouse.y);
  };

  string hovered;
  for (auto it = uiTargets_.rbegin(); it != uiTargets_.rend(); ++it) {
    if (it->enabled && contains(*it)) {
      hovered = it->id;
      break;
    }
  }
  if (hoveredTargetId_ != hovered) {
    hoveredTargetId_ = hovered;
    dirty_ = true;
  }

  if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
    const int delta = mouse.button == ftxui::Mouse::WheelUp ? -1 : 1;
    if (page_ == Page::Stock) moveSelection(delta);
    else if (page_ == Page::Home && inputMode_ == InputMode::None) {
      if (uiBoxContains(dashboardWarningPanelBounds_, mouse.x, mouse.y)) {
        moveDashboardSelection(DashboardList::Warnings, delta);
      } else if (uiBoxContains(dashboardActivityPanelBounds_, mouse.x, mouse.y)) {
        moveDashboardSelection(DashboardList::Commits, delta);
      }
    }
    else if (page_ == Page::Import) moveImportSelection(delta);
    else if (page_ == Page::History) {
      if (uiBoxContains(historyDetailPanelBounds_, mouse.x, mouse.y)) {
        moveHistoryRecordSelection(delta);
      } else if (uiBoxContains(historyListPanelBounds_, mouse.x, mouse.y)) {
        moveHistorySelection(delta);
      }
    }
    else if (page_ == Page::Projects) {
      if (bomView_ == BomView::Split) {
        if (uiBoxContains(bomReadyPanelBounds_, mouse.x, mouse.y)) {
          bomSplitShortFocused_ = false;
          moveBomSelection(delta);
        } else if (uiBoxContains(bomShortPanelBounds_, mouse.x, mouse.y)) {
          bomSplitShortFocused_ = true;
          moveBomSelection(delta);
        }
      } else {
        moveBomSelection(delta);
      }
    }
    else if (page_ == Page::Settings && settingsCategory_ == SettingsCategory::Printer) {
      const auto previousSelection = printerSelection_;
      if (delta < 0 && printerSelection_ > 0) --printerSelection_;
      if (delta > 0 && printerSelection_ + 1 < printerQueues_.size()) ++printerSelection_;
      if (printerSelection_ != previousSelection) stageSelectedPrinterQueue();
    }
    return true;
  }

  if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
    for (size_t reverse = uiTargets_.size(); reverse > 0; --reverse) {
      auto& hit = uiTargets_[reverse - 1];
      if (hit.enabled && contains(hit)) {
        focusedTarget_ = static_cast<int>(reverse - 1);
        hit.activate();
        dirty_ = true;
        return true;
      }
    }
  }
  return !hovered.empty();
}

void App::moveUiFocus(int delta) {
  if (uiTargets_.empty()) return;
  if (focusedTarget_ < 0 && page_ == Page::Import) {
    const auto primary = find_if(uiTargets_.begin(), uiTargets_.end(), [](const UiTarget& target) {
      return target.enabled && target.focusable && target.id == "import.choose";
    });
    if (primary != uiTargets_.end()) {
      focusedTarget_ = static_cast<int>(distance(uiTargets_.begin(), primary));
      dirty_ = true;
      return;
    }
  }
  int next = focusedTarget_;
  for (size_t count = 0; count < uiTargets_.size(); ++count) {
    next = (next + delta + static_cast<int>(uiTargets_.size())) % static_cast<int>(uiTargets_.size());
    if (uiTargets_[static_cast<size_t>(next)].enabled && uiTargets_[static_cast<size_t>(next)].focusable) {
      focusedTarget_ = next;
      dirty_ = true;
      return;
    }
  }
}

bool App::activateFocusedTarget() {
  if (focusedTarget_ < 0 || focusedTarget_ >= static_cast<int>(uiTargets_.size())) return false;
  auto& selected = uiTargets_[static_cast<size_t>(focusedTarget_)];
  if (!selected.enabled || !selected.focusable || !selected.activate) return false;
  selected.activate();
  dirty_ = true;
  return true;
}

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
