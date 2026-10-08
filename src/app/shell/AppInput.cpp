// Inventatory - Application keyboard and mouse input routing.

#include "App.h"

#include "app/shell/AppNavigation.h"
#include "ui/shared/UiFocus.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/StartupRegistration.h"
#include "platform/system/UpdateService.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <utility>

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
  // Below the supported size only the resize notice is drawn; keys must not act on the hidden page.
  if (workspaceBlockedByTerminalSize()) return;
  // A Tab-set focus only survives Tab, Shift+Tab and Enter. Any other key (arrows,
  // j/k, shortcuts, typing) or an open prompt drops it, so Enter cannot re-run a
  // control the user is no longer looking at.
  const bool focusKey = key.type == KeyType::Tab || key.type == KeyType::TabReverse || key.type == KeyType::Enter;
  if ((!focusKey || inputMode_ != InputMode::None) && !focusedTargetId_.empty()) {
    focusedTargetId_.clear();
    dirty_ = true;
  }
  if (page_ == Page::Update) {
    handleUpdateKey(key);
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
    case InputMode::HistorySearch:
      handleHistorySearchKey(key);
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

  if (page_ == Page::Racks && rackDeleteConfirmationActive()) {
    handleRackManagementKey(key);
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
    if (const auto* entry = app_navigation::primaryNavigationEntryForShortcut(key.ch)) {
      changePage(pageFor(entry->page));
      return;
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
    case Page::Update:
      handleUpdateKey(key);
      break;
  }
}

App::Page App::pageFor(app_navigation::PrimaryPage page) {
  switch (page) {
    case app_navigation::PrimaryPage::Stock: return Page::Stock;
    case app_navigation::PrimaryPage::Racks: return Page::Racks;
    case app_navigation::PrimaryPage::Import: return Page::Import;
    case app_navigation::PrimaryPage::Projects: return Page::Projects;
    case app_navigation::PrimaryPage::History: return Page::History;
    case app_navigation::PrimaryPage::Settings: return Page::Settings;
  }
  return Page::Stock;
}

ftxui::Element App::target(ftxui::Element element, string id, UiTargetKind kind,
                           function<void()> activate, bool enabled, bool focusable) const {
  const bool hovered = enabled && hoveredTargetId_ == id;
  const bool focused = enabled && focusable && !focusedTargetId_.empty() && id == focusedTargetId_;

  if (!enabled) {
    // Disabled controls remain visible, but never inherit a hover/focus role
    // from a stale target index or pointer location.
    element = element | ftxui::color(uiMutedText());
  } else {
    switch (kind) {
      case UiTargetKind::Button:
        // Button helpers already own their raised/filled surfaces. Preserve a
        // filled primary button's cyan identity while making both button
        // roles readable under hover and keyboard focus.
        if (hovered || focused) element = element | ftxui::color(uiFocusColor());
        if (focused) element = element | ftxui::bold;
        break;
      case UiTargetKind::Link:
        if (hovered || focused) {
          element = element | ftxui::color(uiFocusColor()) | ftxui::underlined;
        }
        break;
      case UiTargetKind::Field:
        if (hovered) element = element | ftxui::bgcolor(uiHoverBg());
        if (focused) {
          element = element | ftxui::color(uiFocusColor()) | ftxui::bgcolor(uiActiveSoftBg()) | ftxui::bold;
        }
        break;
      case UiTargetKind::Row:
      case UiTargetKind::Cell:
      case UiTargetKind::Navigation:
      case UiTargetKind::Action:
      case UiTargetKind::Category:
        if (hovered) element = element | ftxui::bgcolor(uiHoverBg());
        if (focused) element = element | ftxui::color(uiFocusColor()) | ftxui::bgcolor(uiSelectionBg()) | ftxui::bold;
        break;
    }
  }
  uiTargets_.push_back(UiTarget{move(id), kind, {}, enabled, focusable, move(activate)});
  return element | ftxui::reflect(uiTargets_.back().bounds);
}

bool App::handleMouse(const ftxui::Mouse& mouse) {
  if (workspaceBlockedByTerminalSize()) return false;
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
    focusedTargetId_.clear();
    if (inputMode_ == InputMode::ActionSheet) {
      // The open sheet is the topmost surface: the wheel scrolls it, not the page behind it.
      const int count = static_cast<int>(sheetActions_.size());
      sheetIndex_ = max(0, min(count - 1, sheetIndex_ + delta));
      dirty_ = true;
    }
    else if (page_ == Page::Stock) moveSelection(delta);
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
        if (uiBoxContains(bomTableBounds_, mouse.x, mouse.y)) moveBomSelection(delta);
      } else {
        moveBomSelection(delta);
      }
    }
    else if (page_ == Page::Racks) {
      if (uiBoxContains(rackListPanelBounds_, mouse.x, mouse.y)) moveRackPage(delta);
    }
    else if (page_ == Page::Settings && !settingsEditingField_ && !appearancePickerOpen_) {
      // The wheel moves the row cursor; it never changes a setting.
      moveSettingsRow(delta);
    }
    else if (page_ == Page::Update) {
      if (uiBoxContains(updateNotesBounds_, mouse.x, mouse.y)) {
        if (delta < 0) updateNotesScroll_ = updateNotesScroll_ > 0 ? updateNotesScroll_ - 1U : 0U;
        else ++updateNotesScroll_;
        dirty_ = true;
      }
    }
    return true;
  }

  if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
    const bool sheetOpen = inputMode_ == InputMode::ActionSheet;
    for (size_t reverse = uiTargets_.size(); reverse > 0; --reverse) {
      auto& hit = uiTargets_[reverse - 1];
      if (hit.enabled && contains(hit)) {
        // While the action sheet is open only the sheet and its header control
        // take clicks; a click on the page behind it just closes the sheet.
        if (sheetOpen && hit.id != "header.actions" && hit.id.rfind("action.", 0) != 0) {
          inputMode_ = InputMode::None;
          dirty_ = true;
          return true;
        }
        // A click selects/activates what is under the pointer without taking
        // keyboard focus, so a later Enter still reaches the page's own action.
        focusedTargetId_.clear();
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
  vector<ui_focus::Candidate> candidates;
  candidates.reserve(uiTargets_.size());
  for (const auto& target : uiTargets_) candidates.push_back({target.id, target.enabled, target.focusable});
  string next;
  if (focusedTargetId_.empty() && page_ == Page::Import && ui_focus::isActivatable(candidates, "import.choose")) {
    next = "import.choose";
  } else {
    next = ui_focus::next(candidates, focusedTargetId_, delta);
  }
  if (next.empty() || next == focusedTargetId_) return;
  focusedTargetId_ = next;
  dirty_ = true;
}

bool App::activateFocusedTarget() {
  if (focusedTargetId_.empty()) return false;
  for (auto it = uiTargets_.rbegin(); it != uiTargets_.rend(); ++it) {
    if (it->id != focusedTargetId_) continue;
    if (!it->enabled || !it->focusable || !it->activate) return false;
    // The callback may rebuild the target list on the next frame; run a copy.
    const auto activate = it->activate;
    activate();
    dirty_ = true;
    return true;
  }
  return false;
}

}  // namespace inventatory
