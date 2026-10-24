// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Contextual action registry. The action bar, the action sheet and key shortcuts all come from
// currentActions().

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "core/inventory/VendorProvider.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/tme/TmeApi.h"
#include "ui/shared/ActionSheetLayout.h"
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
KeyEvent chr(char c) { return {KeyType::Character, c, {}}; }
KeyEvent special(KeyType type) { return {type, '\0', {}}; }
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
    // Symbol-only hints can collide; number later duplicates so each row keeps its own id.
    const string baseId = id;
    for (int duplicate = 2; any_of(actions.begin(), actions.end(), [&](const Action& other) { return other.id == id; });
         ++duplicate) {
      id = baseId + "." + to_string(duplicate);
    }
    actions.push_back(Action{move(id), move(label), move(group), move(keyHint), trigger, move(run)});
  };

  const bool hasItem = selectedItem() != nullptr;

  auto reloadInventory = [self] {
    return self->reloadInventoryState();
  };
  auto openDatasheet = [self] {
    if (const auto* item = self->selectedItem()) self->openCurrentUrl(item->datasheetUrl, "datasheet");
  };
  auto openProduct = [self] {
    if (const auto* item = self->selectedItem()) self->openCurrentUrl(item->productUrl, "product");
  };
  auto openTme = [self] {
    if (const auto* item = self->selectedItem()) self->openCurrentUrl(tmeProductPageUrl(tmeProductSymbol(*item)), "TME");
  };
  auto openMouser = [self] {
    if (const auto* item = self->selectedItem()) self->openCurrentUrl(mouserProductUrlOf(*item), "Mouser");
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
    case Page::History:
      add("create checkpoint", "History", "c", chr('c'), [self] { self->beginHistoryCheckpoint(); });
      add("restore state at this point", "History", "s", chr('s'), [self] {
        self->beginHistoryRestore(InventoryRevertMode::Snapshot);
      });
      add("revert commit", "History", "v", chr('v'), [self] {
        self->beginHistoryRestore(InventoryRevertMode::Reverse);
      });
      add("search history", "Search", "/", chr('/'), [self] { self->startHistorySearch(); });
      add("cycle history filter", "Search", "f", chr('f'), [self] { self->cycleHistoryFilter(); });
      add("reload history", "System", "r", chr('r'), [self] {
        self->refreshInventoryCommits();
        self->setMessage("Inventory history reloaded", 2, UiMessageSeverity::Success);
      });
      add("undo latest change", "System", "Ctrl+Z", special(KeyType::CtrlZ),
          [self] { self->undoLastInventoryChange(); });
      add("back", "Go", "Esc", special(KeyType::Escape), [self] {
        if (self->historyRecordOpen_) {
          self->historyRecordOpen_ = false;
          self->historyRecordSelection_ = 0;
          self->dirty_ = true;
        } else {
          self->changePage(Page::Stock);
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
        self->inputReplaceOnType_ = true;
        self->inputMode_ = App::InputMode::QuantityAdjust;
        self->setMessage("Enter the total quantity on hand", 3);
      });
      if (hasItem) add("datasheet", "Links", "d", chr('d'), openDatasheet);
      if (hasItem) add("product", "Links", "o", chr('o'), openProduct);
      if (hasItem) add("DigiKey", "Links", "g", chr('g'), openDigiKey);
      if (const auto* selected = self->selectedItem(); selected != nullptr && !tmeProductSymbol(*selected).empty()) {
        add("TME", "Links", "l", chr('l'), openTme);
      }
      // 'm' is racks, so Mouser takes 's', a letter of its name.
      if (const auto* selected = self->selectedItem(); selected != nullptr && !mouserPartNumberOf(*selected).empty()) {
        add("Mouser", "Links", "s", chr('s'), openMouser);
      }
      if (hasItem) add("check prices", "Links", "c", chr('c'), [self] { self->beginOfferRefresh(); });
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
          self->setMessage("Inventory refreshed", 2, UiMessageSeverity::Success);
        }
      });
      add("undo", "System", "Ctrl+Z", special(KeyType::CtrlZ), [self] { self->undoLastInventoryChange(); });
      if (hasItem)
        add("delete", "System", "Del", special(KeyType::Delete),
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
      if (selectedRackItem() != nullptr) add("datasheet", "Links", "d", chr('d'), [self] { self->openSelectedRackDatasheet(); });
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
      // Enter and the row cursor are page-local; the other keys show on the row buttons.
      if (settingsCategory_ == SettingsCategory::General) {
        add("choose data folder", "General", "b", chr('b'), [self] { self->stageInventatoryFolder(); });
        add("edit low-stock threshold", "General", "e", chr('e'),
            [self] { self->beginSettingsFieldEdit(0); });
        add("export inventory", "Data", "x", chr('x'), [self] { self->exportInventory(); });
        add("back up data", "Data", "k", chr('k'), [self] { self->backupData(); });
        add("restore backup", "Data", "r", chr('r'), [self] { self->restoreData(); });
      }
      if (settingsCategory_ == SettingsCategory::Updates) {
        add("check for software updates", "Updates", "c", chr('c'), [self] { self->beginUpdateChecks(); });
        if (isVersionNewer(settings_.latestAvailableVersion, self->softwareVersion())) {
          add("update Inventatory", "Updates", "u", chr('u'), [self] { self->beginSoftwareUpdate(); });
        }
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
        add("refresh printers", "Printer", "r", chr('r'), [self] { self->refreshPrinterState(); });
        add("switch EU/US symbols", "Printer", "y", chr('y'), [self] { self->toggleSymbolStandard(); });
        if (!settingsDraft_.printerQueue.empty()) {
          add("test print", "Printer", "t", chr('t'), [self] { self->testStagedPrinter(); });
        }
      }
      if (settingsCategory_ == SettingsCategory::QuickLabels) {
        const int presetCount = static_cast<int>(settingsDraft_.quickLabelPresets.size());
        const bool presetSelected = settingsField_ >= 0 && settingsField_ < presetCount;
        if (settingsDraft_.quickLabelPresets.size() < kQuickLabelPresetLimit) {
          add("add preset", "Presets", "a", chr('a'), [self] { self->addQuickLabelPreset(); });
        }
        if (presetSelected) {
          add("test print preset", "Presets", "t", chr('t'), [self] { self->testQuickLabelPreset(); });
          add("remove preset", "Presets", "x", chr('x'), [self] { self->deleteQuickLabelPreset(); });
          if (settingsField_ > 0) add("move preset up", "Presets", "[", chr('['), [self] { self->moveQuickLabelPreset(-1); });
          if (settingsField_ + 1 < presetCount) {
            add("move preset down", "Presets", "]", chr(']'), [self] { self->moveQuickLabelPreset(1); });
          }
        }
        if (!wireLabelText_.empty()) {
          add("print wire label", "Custom", "w", chr('w'), [self] { self->printWireLabel(self->wireLabelText_); });
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
          add("copy token", "Device", "t", chr('t'), [self] { self->copyInventatoryScanToken(); });
          add("regenerate token", "Device", "r", chr('r'), [self] { self->regenerateInventatoryScanToken(); });
          add("forget device", "Device", "c", chr('c'), [self] { self->clearInventatoryScanPairing(); });
        }
      }
      if (settingsCategory_ == SettingsCategory::Vendors) {
        using settings_page_detail::VendorSection;
        const auto section = settings_page_detail::vendorFieldRef(settingsField_).section;
        // e and t act on the vendor under the cursor; r and c act on all of them.
        const auto vendorKeys = [&](const char* name, VendorSetupVendor vendor, bool configured, bool running,
                                    const function<void()>& test) {
          const int keyField = vendor == VendorSetupVendor::DigiKey ? settings_page_detail::kDigiKeyKeyField
                               : vendor == VendorSetupVendor::Tme   ? settings_page_detail::kTmeKeyField
                                                                    : settings_page_detail::kMouserKeyField;
          if (configured) {
            add(self->revealedVendorField_ == keyField ? "hide keys" : "show keys", name, "v", chr('v'),
                [self, keyField] { self->requestVendorKeyReveal(keyField); });
          }
          add(configured ? "edit API key" : "set up API key", name, "e", chr('e'), [self, vendor] { self->openVendorSetup(vendor); });
          if (configured && !running) add("test credentials", name, "t", chr('t'), test);
        };
        if (section == VendorSection::DigiKey) {
          vendorKeys("DigiKey", VendorSetupVendor::DigiKey,
                     !trim(self->settings_.digiKeyClientId).empty() && self->hasStoredDigiKeySecret_,
                     false, [self] { self->testStagedDigiKey(); });
        } else if (section == VendorSection::Tme) {
          vendorKeys("TME", VendorSetupVendor::Tme, self->hasStoredTmeToken_ && self->hasStoredTmeSecret_,
                     self->tmeRefresh_.running(), [self] { self->testStagedTme(); });
        } else if (section == VendorSection::Mouser) {
          vendorKeys("Mouser", VendorSetupVendor::Mouser,
                     self->hasStoredMouserKey_ || self->mouserKeyFromEnvironment_, self->mouserRefresh_.running(),
                     [self] { self->testStagedMouser(); });
        }
        if (self->digiKeyRefresh_.running() || self->tmeRefresh_.running() || self->mouserRefresh_.running()) {
          add("cancel refresh", "Sync", "c", chr('c'), [self] {
            self->digiKeyRefresh_.queue.clear();
            self->tmeRefresh_.queue.clear();
            self->mouserRefresh_.queue.clear();
            self->setMessage("Cancelling after the current vendor requests", 4);
          });
        } else if (!settingsDirty_) {
          add("refresh all vendors", "Sync", "r", chr('r'), [self] { self->beginAllVendorRefresh(); });
        }
      }
      if (settingsDirty_) {
        add("save settings", "Changes", "s", chr('s'), [self] { self->saveSettingsDraft(); });
        add("discard changes", "Changes", "Esc", special(KeyType::Escape), [self] { self->requestSettingsDiscard(); });
      }
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;

    case Page::Import:
      if (importCommitPending_) {
        add("retry import save", "Review", "R", chr('R'), [self] { self->finishImportReview(); });
        add("cancel import", "Review", "q", chr('q'), [self] {
          self->cancelImportSession();
          self->changePage(Page::Stock);
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
        add("cancel import", "Review", "q", chr('q'), [self] { self->requestImportDiscard(); });
      }
      break;

    case Page::Projects:
      // Find in racks owns the keyboard while it runs. Each project view lists only its own commands.
      if (bomDeductPrompt_) {
        add("deduct from stock", "Finish", "y", chr('y'), [self] { self->finishBomBuild(true); });
        add("keep stock", "Finish", "n", chr('n'), [self] { self->finishBomBuild(false); });
        add("took one more", "Amounts", "+", chr('+'), [self] { self->adjustBomPickTaken(1); });
        add("took one less", "Amounts", "-", chr('-'), [self] { self->adjustBomPickTaken(-1); });
        add("back to the last rack", "Build", "Bksp", special(KeyType::Backspace),
            [self] { self->advanceBomBuild(-1); });
      } else if (bomView_ == BomView::Build) {
        add("rack done", "Build", "Enter", special(KeyType::Enter), [self] { self->advanceBomBuild(1); });
        add("previous rack", "Build", "Bksp", special(KeyType::Backspace), [self] { self->advanceBomBuild(-1); });
      } else if (bomView_ == BomView::List || !bomAnalysisValid_) {
        add("open project", "Project", "Enter", special(KeyType::Enter),
            [self] { self->openSelectedBomProject(); });
        add("forget project", "Project", "d", chr('d'), [self] { self->deleteSelectedBomProject(); });
        add("import a BOM", "Create", "i", chr('i'), [self] { self->beginCsvImport(); });
      } else {
        const auto back = [&] {
          add("back to the project", "Go", "Esc", special(KeyType::Escape), [self] { self->openBomOverview(); });
        };
        const auto boards = [&] {
          add("more boards", "Project", "+", chr('+'), [self] { self->adjustBomBoards(1); });
          add("fewer boards", "Project", "-", chr('-'), [self] { self->adjustBomBoards(-1); });
        };
        const auto findInRacks = [&] {
          add("find in racks", "Project", "f", chr('f'), [self] { self->requestBomFindInRacks(); });
        };
        switch (bomView_) {
          case BomView::Overview:
            add("open step", "Project", "Enter", special(KeyType::Enter),
                [self] { self->openBomStep(self->bomOverviewSelection_); });
            findInRacks();
            add("all lines", "Project", "t", chr('t'), [self] {
              self->bomView_ = BomView::Lines;
              self->dirty_ = true;
            });
            boards();
            add("shopping list", "Order", "o", chr('o'), [self] { self->exportBomShortages(); });
            add("all projects", "Go", "Esc", special(KeyType::Escape), [self] {
              self->bomView_ = BomView::List;
              self->dirty_ = true;
            });
            break;
          case BomView::Lines:
            add("open its step", "Project", "Enter", special(KeyType::Enter),
                [self] { self->openBomLine(self->bomSplitSelection_); });
            findInRacks();
            boards();
            back();
            break;
          case BomView::StartNotice:
            add("start anyway", "Find in racks", "Enter", special(KeyType::Enter), [self] {
              self->beginBomBuild();
              if (self->bomView_ != BomView::Build) self->openBomOverview();
            });
            add("get them first", "Find in racks", "s", chr('s'), [self] { self->openFirstOpenBomStep(); });
            back();
            break;
          case BomView::Confirm:
            add("use this part", "Confirm", "Enter", special(KeyType::Enter), [self] { self->useBomConfirmCandidate(); });
            add("none of these", "Confirm", "n", chr('n'), [self] { self->markBomLineNotInStock(); });
            add("skip", "Confirm", "s", chr('s'), [self] { self->advanceBomStep(false); });
            back();
            break;
          case BomView::Choose: {
            const auto* project = activeBomProject();
            bool chosen = false;
            if (project != nullptr) {
              const auto found = project->enrichment.find(bomPicker_.lineKey);
              chosen = found != project->enrichment.end() && bomEnrichmentChosen(found->second);
            }
            if (!bomPicker_.entries.empty()) {
              add("use this part", "Choose", "Enter", special(KeyType::Enter), [self] { self->useBomPickerSelection(); });
            } else if (chosen && !bomPickerFuture_.valid()) {
              add("keep it", "Choose", "Enter", special(KeyType::Enter), [self] { self->advanceBomStep(true); });
            }
            add("open in store", "Choose", "l", chr('l'), [self] { self->openBomPartPage(); });
            add("search", "Choose", "/", chr('/'), [self] { self->beginBomPartSearchEdit(); });
            add("change store", "Choose", "v", chr('v'), [self] { self->cycleBomStore(); });
            add("skip", "Choose", "s", chr('s'), [self] { self->advanceBomStep(false); });
            back();
            break;
          }
          case BomView::Order:
            if (bomOrderConfirm_) {
              add("mark them ordered", "Order", "y", chr('y'), [self] { self->markBomOrdered(true); });
              add("cancel", "Order", "n", chr('n'), [self] {
                self->bomOrderConfirm_ = false;
                self->dirty_ = true;
              });
            } else {
              add("open this page", "Order", "Enter", special(KeyType::Enter), [self] { self->openBomOrderPage(false); });
              add("save cart file", "Order", "c", chr('c'), [self] { self->saveBomCartFiles(); });
              add("open next page", "Order", "l", chr('l'), [self] { self->openBomOrderPage(true); });
              add("mark ordered", "Order", "m", chr('m'), [self] { self->markBomOrdered(false); });
            }
            back();
            break;
          case BomView::Receive:
            add("receive", "Receive", "Enter", special(KeyType::Enter), [self] { self->beginBomReceive(); });
            add("receive all", "Receive", "a", chr('a'), [self] { self->receiveAllBomOrders(); });
            back();
            break;
          default:
            break;
        }
      }
      if (bomProjectsDirty_) {
        add("retry project save", "Data", "R", chr('R'), [self] { self->retrySaveState(); });
      }
      if (!bomDeductPrompt_ && bomView_ != BomView::Build) {
        // A finished build is an ordinary inventory commit, so the shared undo reverts its stock deduction.
        add("undo", "System", "Ctrl+Z", special(KeyType::CtrlZ), [self] { self->undoLastInventoryChange(); });
      }
      add("quit", "System", "q", chr('q'), [self] { self->requestUserExit(); });
      break;
    case Page::VendorSetup:
    case Page::Onboarding:
    case Page::ScanSetup:
    case Page::Update:
      break;  // These pages register no contextual actions.
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
  auto actions = currentActions();
  if (actions.empty()) {
    setMessage("No actions available here", 2, UiMessageSeverity::Warning);
    return;
  }
  // Group the rows so Up/Down follow the order they are drawn in.
  vector<string> groups;
  groups.reserve(actions.size());
  for (const auto& action : actions) groups.push_back(action.group);
  sheetActions_.clear();
  sheetActions_.reserve(actions.size());
  for (const auto index : action_sheet::groupedOrder(groups)) sheetActions_.push_back(move(actions[index]));
  sheetIndex_ = 0;
  inputMode_ = InputMode::ActionSheet;
  dirty_ = true;
}

// The header Actions control works like Space, but never replaces a prompt the user is typing into.
void App::activateActionsControl() {
  if (inputMode_ == InputMode::ActionSheet) {
    inputMode_ = InputMode::None;
    dirty_ = true;
    return;
  }
  if (inputMode_ != InputMode::None) {
    setMessage("Finish or cancel the current input first", 2, UiMessageSeverity::Warning);
    return;
  }
  openActionSheet();
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
  const int screenHeight = active != nullptr ? active->dimy() : 40;

  vector<string> groups;
  groups.reserve(sheetActions_.size());
  for (const auto& action : sheetActions_) groups.push_back(action.group);
  const auto allRows = action_sheet::rowsFor(groups);
  const auto cursorRow = action_sheet::rowOfAction(allRows, static_cast<size_t>(max(0, sheetIndex_)));
  const auto window = action_sheet::windowFor(allRows.size(), cursorRow,
                                              static_cast<size_t>(action_sheet::maxBodyRows(screenHeight)));
  const bool moreAbove = window.first > 0;
  const bool moreBelow = window.first + window.count < allRows.size();

  string title = "  Actions   Up/Down move   Enter run   Esc close   Space opens this";
  if (moreAbove || moreBelow) {
    title += "   " + to_string(min(static_cast<size_t>(max(0, sheetIndex_)) + 1, sheetActions_.size())) + "/" +
             to_string(sheetActions_.size()) + (moreAbove ? ", more above" : "") +
             (moreBelow ? ", more below" : "");
  }

  ftxui::Elements rows;
  rows.push_back(fullLine(title, uiAccentColor(), uiPanelRightBg()));
  rows.push_back(uiDivider());

  auto self = const_cast<App*>(this);
  for (size_t position = window.first; position < window.first + window.count; ++position) {
    const auto& row = allRows[position];
    const auto& action = sheetActions_[row.action];
    if (row.header) {
      rows.push_back(fullLine("  " + action.group, uiAccentColor(), uiPanelLeftBg()));
      continue;
    }
    const size_t index = row.action;
    const bool selected = static_cast<int>(index) == sheetIndex_;
    const auto bg = selected ? uiRowSelectedBg() : uiSurfaceBg();
    auto keyCell = ftxui::hbox({
                       styledText("    " + action.keyHint, selected ? uiTitleColor() : uiLinkColor()),
                       ftxui::filler(),
                   }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 14);
    auto line = ftxui::hbox({
                    keyCell,
                    styledText(action.label, selected ? uiTitleColor() : uiMutedColor()),
                    ftxui::filler(),
                }) |
                ftxui::bgcolor(bg);
    rows.push_back(target(line, "action." + action.id, UiTargetKind::Action, [self, index] {
      if (index >= self->sheetActions_.size()) return;
      auto selected = self->sheetActions_[index];
      self->inputMode_ = InputMode::None;
      selected.run();
      self->dirty_ = true;
    }));
  }

  // Size the vbox itself, not a wrapping window, so its rows' fillers stretch to the full width.
  return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiPanelLeftBg()) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, screenWidth);
}

}  // namespace inventatory
