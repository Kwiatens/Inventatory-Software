// Inventatory - Hardware Inventory Management System
// Contextual action registry: the single source of truth for each screen's
// commands. The action bar and the bottom sheet are both generated from
// currentActions(), and key accelerators dispatch through the same list.

#include "App.h"

#include "platform/DigiKeyApi.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

namespace {
KeyEvent chr(char c) { return {KeyType::Character, c}; }
KeyEvent special(KeyType type) { return {type, '\0'}; }
KeyEvent none() { return {KeyType::Unknown, '\0'}; }
}  // namespace

vector<App::Action> App::currentActions() const {
  App* self = const_cast<App*>(this);
  vector<Action> actions;
  auto add = [&](string label, string group, string keyHint, KeyEvent trigger, function<void()> run) {
    string id = pageName() + "." + group + "." + keyHint;
    transform(id.begin(), id.end(), id.begin(), [](unsigned char ch) {
      return isalnum(ch) ? static_cast<char>(tolower(ch)) : '.';
    });
    id.erase(unique(id.begin(), id.end(), [](char lhs, char rhs) { return lhs == '.' && rhs == '.'; }), id.end());
    actions.push_back(Action{move(id), move(label), move(group), move(keyHint), trigger, move(run)});
  };

  const bool hasItem = selectedItem() != nullptr;

  auto reloadInventory = [self] {
    if (!self->store_.load(self->inventoryPath_)) {
      self->persistenceError_ = "Unable to reload the inventory database; the in-memory data was kept.";
      self->setMessage(self->persistenceError_, 5);
      return false;
    }
    loadInventoryHistory(self->inventoryPath_, self->inventoryHistory_);
    self->refreshInventoryMovements();
    self->persistedStore_ = self->store_;
    self->persistedStoreValid_ = true;
    self->refreshInventoryCommits();
    if (self->inventoryHistory_.empty()) {
      appendInventoryHistory(self->inventoryHistory_,
                             makeInventoryHistoryPoint(self->store_.items(), self->settings_.lowStockThreshold));
    }
    saveInventoryHistory(self->inventoryPath_, self->inventoryHistory_);
    self->persistenceError_.clear();
    return true;
  };
  auto openDatasheet = [self] {
    if (const auto* item = self->selectedItem()) self->openCurrentUrl(item->datasheetUrl, "datasheet");
  };
  auto openProduct = [self] {
    if (const auto* item = self->selectedItem()) self->openCurrentUrl(item->productUrl, "product");
  };
  auto openDigiKey = [self] {
    if (const auto* item = self->selectedItem()) {
      const auto url = item->digikeyPartNumber.empty()
                           ? string()
                           : "https://www.digikey.com/en/products/result?keywords=" + item->digikeyPartNumber;
      self->openCurrentUrl(url, "DigiKey");
    }
  };
  switch (page_) {
    case Page::Home:
      add("stock", "Go", "2", chr('2'), [self] { self->changePage(Page::Stock); });
      add("racks", "Go", "3", chr('3'), [self] { self->openRackManagement(); });
      add("projects", "Go", "5", chr('5'), [self] { self->openBomProjects(); });
      add("history", "Go", "6", chr('6'), [self] { self->changePage(Page::History); });
      add("settings", "Go", "7", chr('7'), [self] { self->openSettings(); });
      add("add part", "Create", "n", chr('n'), [self] { self->beginEditCurrentItem(true); });
      add("import CSV", "Create", "i", chr('i'), [self] { self->beginCsvImport(); });
      add("search", "System", "/", chr('/'), [self] { self->startSearch(); });
      add("reload", "System", "r", chr('r'), [self, reloadInventory] {
        if (reloadInventory()) self->setMessage("Inventory reloaded from the database", 2);
      });
      add("export inventory", "Data", "x", chr('x'), [self] { self->exportInventory(); });
      add("backup data", "Data", "k", chr('k'), [self] { self->backupData(); });
      add("retry save", "Data", "R", chr('R'), [self] { self->retrySaveState(); });
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;

    case Page::History:
      add("create checkpoint", "History", "c", chr('c'), [self] { self->beginHistoryCheckpoint(); });
      add("restore snapshot", "History", "s", chr('s'), [self] {
        self->beginHistoryRestore(InventoryRevertMode::Snapshot);
      });
      add("reverse changes", "History", "v", chr('v'), [self] {
        self->beginHistoryRestore(InventoryRevertMode::Reverse);
      });
      add("reload history", "System", "r", chr('r'), [self] {
        self->refreshInventoryCommits();
        self->setMessage("Inventory history reloaded", 2);
      });
      add("undo latest change", "System", "Ctrl+Z", special(KeyType::CtrlZ),
          [self] { self->undoLastInventoryChange(); });
      add("back", "Go", "Esc", special(KeyType::Escape), [self] {
        if (self->historyRecordOpen_) {
          self->historyRecordOpen_ = false;
          self->historyRecordSelection_ = 0;
          self->dirty_ = true;
        } else {
          self->changePage(Page::Home);
        }
      });
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;

    case Page::Stock:
      if (stocktakeActive_) {
        add("count selected part", "Stocktake", "Enter", special(KeyType::Enter),
            [self] { self->beginStocktakeCount(); });
        add("finish stocktake", "Stocktake", "s", chr('s'), [self] { self->finishStocktake(); });
        add("cancel stocktake", "Stocktake", "q", chr('q'), [self] { self->cancelStocktake(); });
        break;
      }
      if (closestSearchActive_) {
        add("edit closest target", "Search", "F", chr('F'), [self] { self->startClosestSearch(); });
        add("close closest search", "Search", "Esc", special(KeyType::Escape),
            [self] { self->clearClosestSearch(); });
      }
      if (hasItem) add("edit", "Edit", "e", chr('e'), [self] { self->beginEditCurrentItem(false); });
      add("new part", "Edit", "n", chr('n'), [self] { self->beginEditCurrentItem(true); });
      if (hasItem) add("add one", "Edit", "+", chr('+'), [self] { self->adjustQuantity(1); });
      if (hasItem) add("remove one", "Edit", "-", chr('-'), [self] { self->adjustQuantity(-1); });
      if (hasItem) add("set quantity", "Edit", "a", chr('a'), [self] {
        self->inputBuffer_ = to_string(self->selectedItem()->quantity);
        self->inputMode_ = App::InputMode::QuantityAdjust;
        self->setMessage("Enter the total quantity on hand", 3);
      });
      if (hasItem) add("datasheet", "Links", "d", chr('d'), openDatasheet);
      if (hasItem) add("product", "Links", "o", chr('o'), openProduct);
      if (hasItem) add("DigiKey", "Links", "g", chr('g'), openDigiKey);
      if (hasItem) add("print label", "Print", "p", chr('p'), [self] { self->printSelectedLabel(); });
      add(autoPrintScannedLabels_ ? "auto-label off" : "auto-label on", "Print", "P", chr('P'),
          [self] { self->toggleAutoPrintScannedLabels(); });
      add("racks", "Go", "m", chr('m'), [self] { self->openRackManagement(); });
      add(stockDetailsExpanded_ ? "hide details" : "show details", "View", "i", chr('i'), [self] {
        self->stockDetailsExpanded_ = !self->stockDetailsExpanded_;
        self->dirty_ = true;
      });
      add("filters", "View", "f", chr('f'), [self] { self->openStockFilterPanel(); });
      add("search", "System", "/", chr('/'), [self] { self->startSearch(); });
      if (!closestSearchActive_) {
        add("find closest to", "Search", "F", chr('F'), [self] { self->startClosestSearch(); });
      }
      add("reload", "System", "r", chr('r'), [self, reloadInventory] {
        if (reloadInventory()) {
          self->syncSelectionToFilter();
          self->setMessage("Inventory refreshed", 2);
        }
      });
      add("undo", "System", "Ctrl+Z", special(KeyType::CtrlZ), [self] { self->undoLastInventoryChange(); });
      if (hasItem)
        add("delete", "System", "Ctrl+Bksp", special(KeyType::CtrlBackspace),
            [self] { self->armDeleteConfirmation(); });
      add("start stocktake", "Data", "t", chr('t'), [self] { self->beginStocktake(); });
      add("export inventory", "Data", "x", chr('x'), [self] { self->exportInventory(); });
      add("backup data", "Data", "b", chr('b'), [self] { self->backupData(); });
      add("retry save", "Data", "R", chr('R'), [self] { self->retrySaveState(); });
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;

    case Page::Racks:
      // 'v' (not space): space is the global action-sheet key.
      add("place / move", "Slot", "v", chr('v'), [self] { self->beginOrCompleteRackMove(); });
      add("part label", "Slot", "p", chr('p'), [self] { self->printSelectedRackPartLabel(); });
      add("part minus one", "Slot", "-", chr('-'), [self] { self->adjustSelectedRackItemQuantity(-1); });
      add("part plus one", "Slot", "+", chr('+'), [self] { self->adjustSelectedRackItemQuantity(1); });
      add("part details", "Slot", "Enter", special(KeyType::Enter), [self] { self->openSelectedRackItemDetail(); });
      add("unassign", "Slot", "u", chr('u'), [self] { self->unassignSelectedRackItem(); });
      add("auto-assign", "Slot", "a", chr('a'), [self] { self->autoAssignSelectedRackItem(); });
      add("prev rack", "Racks", "[", chr('['), [self] { self->moveRackPage(-1); });
      add("next rack", "Racks", "]", chr(']'), [self] { self->moveRackPage(1); });
      add("jump", "Racks", "g", chr('g'), [self] {
        self->inputBuffer_.clear();
        self->inputMode_ = InputMode::RackJump;
        self->setMessage("Enter rack code like R3", 3);
      });
      add("filter", "Racks", "f", chr('f'), [self] { self->beginRackFilter(); });
      add("rack label", "Racks", "P", chr('P'), [self] { self->printSelectedRackLabel(); });
      add("create rack", "Admin", "c", chr('c'), [self] {
        self->inputBuffer_.clear();
        self->inputMode_ = InputMode::RackCreate;
        self->setMessage("Create a new empty rack by type", 3);
      });
      add("rename", "Admin", "r", chr('r'), [self] {
        if (const auto* rack = self->selectedRack()) {
          self->inputBuffer_ = rack->code;
          self->inputMode_ = InputMode::RackRename;
          self->setMessage("Enter a unique rack code like R12", 3);
        }
      });
      add("set type", "Admin", "t", chr('t'), [self] {
        if (const auto* rack = self->selectedRack()) {
          self->inputBuffer_ = rack->componentType;
          self->inputMode_ = InputMode::RackType;
          self->setMessage("Edit this rack's dedicated component type", 3);
        }
      });
      add("delete rack", "Admin", "x", chr('x'), [self] { self->deleteSelectedRack(); });
      add("search", "System", "/", chr('/'), [self] { self->startSearch(); });
      add("undo", "System", "Ctrl+Z", special(KeyType::CtrlZ), [self] {
        self->undoLastInventoryChange();
        self->syncRackSelection();
      });
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;

    case Page::Settings:
      if (settingsCategory_ == SettingsCategory::General) {
        add("choose data folder", "General", "b", chr('b'), [self] { self->stageInventatoryFolder(); });
        add("edit low-stock threshold", "General", "e", chr('e'),
            [self] { self->beginSettingsFieldEdit(0); });
        add("export inventory", "Data", "x", chr('x'), [self] { self->exportInventory(); });
        add("backup data", "Data", "k", chr('k'), [self] { self->backupData(); });
        add("restore backup", "Data", "r", chr('r'), [self] { self->restoreData(); });
      }
      if (settingsCategory_ == SettingsCategory::Updates) {
        add("check for updates", "Updates", "c", chr('c'), [self] { self->beginUpdateChecks(); });
      }
      if (settingsCategory_ == SettingsCategory::Appearance) {
        add("edit hex color", "Appearance", "e", chr('e'),
            [self] { self->beginSettingsFieldEdit(self->settingsField_); });
        add("open color picker", "Appearance", "p", chr('p'), [self] { self->openAppearancePicker(); });
        add("reset selected color", "Appearance", "r", chr('r'),
            [self] { self->resetSelectedAppearanceColor(); });
        add("reset all colors", "Appearance", "d", chr('d'), [self] { self->resetAppearanceColors(); });
      }
      if (settingsCategory_ == SettingsCategory::Printer) {
      add("refresh", "Printer", "r", chr('r'), [self] {
        self->refreshPrinterState();
        self->setMessage("Printer list refreshed", 2);
      });
      if (selectedPrinterQueue() != nullptr) {
        add("test", "Printer", "t", chr('t'), [self] {
          if (const auto* printer = self->selectedPrinterQueue()) {
            self->printerService_.setConfiguredPrinter(printer->name);
            self->printerCheck_ = self->printerService_.probeConfiguredPrinter();
            self->setMessage(self->printerCheck_.message.empty() ? "Printer checked" : self->printerCheck_.message, 3);
            self->dirty_ = true;
          }
        });
        add("save", "Printer", "s", chr('s'), [self] {
          if (const auto* printer = self->selectedPrinterQueue()) {
            self->printerService_.setConfiguredPrinter(printer->name);
            self->printerCheck_ = self->printerService_.probeConfiguredPrinter();
            self->saveState();
            self->settingsDraft_.printerQueue = printer->name;
            self->settingsDirty_ = true;
            self->setMessage(self->printerCheck_.ok ? "Printer saved and ready" : "Printer saved, but not ready", 3);
          }
        });
      }
      }
      if (settingsCategory_ == SettingsCategory::InventatoryScan) {
        add("refresh event queue", "Device", "v", chr('v'), [self] { self->refreshDeviceEventRecords(); });
        add("retry failed events", "Device", "y", chr('y'), [self] { self->retryFailedDeviceEvents(); });
        add("discard failed events", "Device", "x", chr('x'), [self] { self->discardFailedDeviceEvents(); });
        const bool setupComplete = self->inventatoryScanConfig_.setupComplete ||
                                   !self->inventatoryScanConfig_.deviceId.empty();
        if (!setupComplete) {
          add("begin setup", "Device", "b", chr('b'), [self] { self->openInventatoryScanSetup(); });
        } else {
          add("pair new device", "Device", "p", chr('p'), [self] { self->openInventatoryScanSetup(); });
          add("restart bridge", "Device", "h", chr('h'), [self] { self->restartDeviceService(); });
          add("check firmware", "Device", "f", chr('f'), [self] { self->beginScanFirmwareCheck(); });
          add("copy token", "Device", "t", chr('t'), [self] { self->copyInventatoryScanToken(); });
          add("regenerate token", "Device", "r", chr('r'), [self] { self->regenerateInventatoryScanToken(); });
          add("clear device", "Device", "c", chr('c'), [self] { self->clearInventatoryScanPairing(); });
        }
      }
      if (settingsCategory_ == SettingsCategory::DigiKey) {
        const bool configured = !trim(self->settings_.digiKeyClientId).empty() && self->hasStoredDigiKeySecret_;
        if (!configured) {
          add("begin setup", "DigiKey", "b", chr('b'), [self] { self->openDigiKeySetup(); });
        } else {
          add("test credentials", "DigiKey", "t", chr('t'), [self] { self->testStagedDigiKey(); });
          add("refresh inventory data", "DigiKey", "r", chr('r'), [self] { self->beginDigiKeyRefresh(); });
        }
      }
      if (settingsDirty_) {
        add("save settings", "Changes", "s", chr('s'), [self] { self->saveSettingsDraft(); });
        add("discard changes", "Changes", "Esc", special(KeyType::Escape), [self] { self->cancelSettingsDraft(); });
      }
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;

    case Page::Import:
      if (importCommitPending_) {
        add("retry import save", "Review", "R", chr('R'), [self] { self->finishImportReview(); });
        add("cancel import", "Review", "q", chr('q'), [self] {
          self->cancelImportSession();
          self->changePage(Page::Home);
          self->setMessage("CSV import cancelled", 3);
        });
      } else if (importSyncRunning_) {
        add("cancel DigiKey sync", "Sync", "c", chr('c'), [self] {
          if (self->importSyncCancelFlag_) self->importSyncCancelFlag_->store(true);
          self->importSyncCancelRequested_ = true;
          self->setMessage("Cancelling after the current DigiKey request", 4);
        });
      } else if (importSyncPrompt_ && importSyncHasRun_) {
        if (!importSyncFailedItemIds_.empty()) {
          add("retry failed sync", "Sync", "r", chr('r'), [self] { self->retryImportSync(); });
        }
        add("finish import", "Finish", "Enter", special(KeyType::Enter), [self] { self->finishCsvImport(false); });
      } else if (importSyncPrompt_) {
        add("sync with DigiKey", "Finish", "y", chr('y'), [self] { self->finishCsvImport(true); });
        add("finish without sync", "Finish", "n", chr('n'), [self] { self->finishCsvImport(false); });
      } else {
        add("edit row", "Review", "e", chr('e'), [self] { self->beginEditImportCandidate(); });
        add("cancel import", "Review", "q", chr('q'), [self] {
          self->cancelImportSession();
          self->changePage(Page::Home);
          self->setMessage("CSV import cancelled", 3);
        });
      }
      break;

    case Page::Projects:
      // The build walkthrough owns the keyboard while it runs, so only the
      // split and list views expose project commands here.
      if (bomDeductPrompt_) {
        if (bomBuildReady(bomAnalysis_)) {
          add("subtract from stock", "Finish", "y", chr('y'), [self] { self->finishBomBuild(true); });
          add("keep stock", "Finish", "n", chr('n'), [self] { self->finishBomBuild(false); });
        } else {
          add("return to shortages", "Finish", "Esc", special(KeyType::Escape), [self] {
            self->bomDeductPrompt_ = false;
            self->bomView_ = BomView::Split;
            self->dirty_ = true;
          });
        }
      } else if (bomView_ == BomView::Build) {
        add("next rack", "Build", "Enter", special(KeyType::Enter), [self] { self->advanceBomBuild(1); });
        add("previous rack", "Build", "Bksp", special(KeyType::Backspace), [self] { self->advanceBomBuild(-1); });
      } else if (bomView_ == BomView::Split && bomAnalysisValid_) {
        add("build", "Project", "b", chr('b'), [self] { self->beginBomBuild(); });
        add("more boards", "Project", "+", chr('+'), [self] { self->adjustBomBoards(1); });
        add("fewer boards", "Project", "-", chr('-'), [self] { self->adjustBomBoards(-1); });
        add("alternate match", "Match", "a", chr('a'), [self] { self->cycleBomAlternate(); });
        add("export shortages", "Order", "o", chr('o'), [self] { self->exportBomShortages(); });
        if (!bomAnalysis_.matches.empty()) {
          const auto index = min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
          if (!bomAnalysis_.matches[index].sufficient) {
            add("receive selected shortage", "Order", "r", chr('r'),
                [self] { self->beginBomRestock(); });
          }
        }
        add("all projects", "Go", "Esc", special(KeyType::Escape), [self] {
          self->bomView_ = BomView::List;
          self->dirty_ = true;
        });
      } else {
        add("open project", "Project", "Enter", special(KeyType::Enter),
            [self] { self->openSelectedBomProject(); });
        add("forget project", "Project", "d", chr('d'), [self] { self->deleteSelectedBomProject(); });
        add("import a BOM", "Create", "i", chr('i'), [self] { self->beginCsvImport(); });
      }
      if (bomProjectsDirty_) {
        add("retry project save", "Data", "R", chr('R'), [self] { self->retrySaveState(); });
      }
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;
  }

  return actions;
}

bool App::triggerMatches(const KeyEvent& trigger, const KeyEvent& key) const {
  if (trigger.type == KeyType::Unknown || trigger.type != key.type) {
    return false;
  }
  if (key.type == KeyType::Character) {
    return trigger.ch == key.ch;  // case-sensitive so 'p' and 'P' stay distinct
  }
  return true;
}

bool App::dispatchAction(const KeyEvent& key) {
  for (const auto& action : currentActions()) {
    if (triggerMatches(action.trigger, key)) {
      action.run();
      return true;
    }
  }
  return false;
}

void App::openActionSheet() {
  sheetActions_ = currentActions();
  if (sheetActions_.empty()) {
    setMessage("No actions available here", 2);
    return;
  }
  sheetIndex_ = 0;
  inputMode_ = InputMode::ActionSheet;
  dirty_ = true;
}

void App::handleActionSheetKey(const KeyEvent& key) {
  const int count = static_cast<int>(sheetActions_.size());
  if (key.type == KeyType::Up || (key.type == KeyType::Character && key.ch == 'k')) {
    sheetIndex_ = max(0, sheetIndex_ - 1);
    dirty_ = true;
  } else if (key.type == KeyType::Down || (key.type == KeyType::Character && key.ch == 'j')) {
    sheetIndex_ = min(count - 1, sheetIndex_ + 1);
    dirty_ = true;
  } else if (key.type == KeyType::Enter) {
    if (sheetIndex_ >= 0 && sheetIndex_ < count) {
      const auto action = sheetActions_[static_cast<size_t>(sheetIndex_)];
      inputMode_ = InputMode::None;  // close before running so the action can set its own mode
      action.run();
    } else {
      inputMode_ = InputMode::None;
    }
    dirty_ = true;
  } else if (key.type == KeyType::Escape ||
             (key.type == KeyType::Character && key.ch == ' ')) {
    inputMode_ = InputMode::None;
    dirty_ = true;
  }
}

ftxui::Element App::renderActionSheetUi() const {
  const auto* active = ftxui::ScreenInteractive::Active();
  const int screenWidth = active != nullptr ? active->dimx() : 120;

  ftxui::Elements rows;
  rows.push_back(fullLine("  Inventatory actions \xE2\x80\x94 \xE2\x86\x91\xE2\x86\x93 move  \xE2\x8F\x8E run  esc close",
                          uiAccentColor(), uiPanelRightBg()));
  rows.push_back(uiDivider());

  string currentGroup;
  for (size_t index = 0; index < sheetActions_.size(); ++index) {
    const auto& action = sheetActions_[index];
    if (action.group != currentGroup) {
      currentGroup = action.group;
      rows.push_back(fullLine("  " + currentGroup, uiAccentColor(), uiPanelLeftBg()));
    }
    const bool selected = static_cast<int>(index) == sheetIndex_;
    const auto bg = selected ? uiRowSelectedBg() : (index % 2 == 0 ? uiRowDarkBg() : uiRowLightBg());
    auto keyCell = ftxui::hbox({
                       styledText("    " + action.keyHint, selected ? uiTitleColor() : uiLinkColor()),
                       ftxui::filler(),
                   }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 14);
    auto row = ftxui::hbox({
                   keyCell,
                   styledText(action.label, selected ? uiTitleColor() : uiMutedColor()),
                   ftxui::filler(),
               }) |
               ftxui::bgcolor(bg);
    auto self = const_cast<App*>(this);
    rows.push_back(target(row, "action." + action.id, UiTargetKind::Action, [self, index] {
      if (index >= self->sheetActions_.size()) return;
      auto selected = self->sheetActions_[index];
      self->inputMode_ = InputMode::None;
      selected.run();
      self->dirty_ = true;
    }));
  }

  // Size the vbox itself (not a window wrapper) to the full terminal width so
  // its rows' filler()s stretch edge to edge and the sheet stays opaque; a
  // window would clamp only its outer size and leave the child at natural width.
  return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiPanelLeftBg()) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, screenWidth);
}

}  // namespace inventatory
