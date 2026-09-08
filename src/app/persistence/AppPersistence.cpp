// Inventatory - Hardware Inventory Management System
// Workspace loading, persistence, backup, restore, and history actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <memory>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::loadState() {
  persistedStoreValid_ = false;
  activitySavePending_ = false;
  scannerConfigSavePending_ = false;
  appSettingsSavePending_ = false;
  pendingMovementSource_.clear();
  pendingMovementReference_.clear();
  error_code inventoryError;
  const bool inventoryFileExists = filesystem::exists(inventoryPath_, inventoryError);
  InventoryStore loadedStore;
  const bool inventoryLoaded = !inventoryFileExists || loadedStore.load(inventoryPath_);
  if (inventoryFileExists && !inventoryLoaded) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not read the existing inventory database: " + inventoryPath_.string();
    persistenceError_ = inventoryRecoveryDetail_ + ". It has not been changed.";
    dirty_ = true;
    return;
  }
  // A missing database is a valid empty workspace.  Activate the candidate
  // only after an existing database has loaded successfully, so a failed
  // switch cannot overwrite the prior in-memory inventory and an empty target
  // can never inherit that inventory during its initial save.
  store_ = move(loadedStore);
  inventoryRecoveryRequired_ = false;
  inventoryRecoveryDetail_.clear();
  error_code scanConfigError;
  const bool scanConfigFileExists = filesystem::exists(inventatoryScanConfigPath_, scanConfigError);
  InventatoryScanConfig loadedScanConfig;
  if (scanConfigError || (scanConfigFileExists && !loadInventatoryScanConfig(inventatoryScanConfigPath_, loadedScanConfig))) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not read the existing scanner configuration: " +
                               inventatoryScanConfigPath_.string();
    persistenceError_ = inventoryRecoveryDetail_ + ". It has not been changed.";
    return;
  }
  if (scanConfigFileExists) inventatoryScanConfig_ = move(loadedScanConfig);
  vector<ActivityEntry> loadedActivities;
  error_code activityError;
  const bool activityFileExists = filesystem::exists(activityPath_, activityError);
  const bool activityLoadFailed = activityError ||
                                  (activityFileExists && !loadActivities(activityPath_, loadedActivities));
  activityPersistenceBlocked_ = activityLoadFailed;
  activities_ = activityLoadFailed ? vector<ActivityEntry>{} : move(loadedActivities);
  vector<BomProject> loadedBomProjects;
  const bool bomProjectsLoaded = !inventoryFileExists || inventatory::loadBomProjects(inventoryPath_, loadedBomProjects);
  if (!bomProjectsLoaded) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not read the existing BOM projects from: " + inventoryPath_.string();
    persistenceError_ = inventoryRecoveryDetail_ + ". They have not been changed.";
    return;
  }
  bomProjects_ = move(loadedBomProjects);
  bomProjectsDirty_ = false;
  refreshDeviceEventRecords();
  refreshInventoryMovements();
  const bool commitHistoryReady = ensureInventoryCommitHistory(inventoryPath_, store_);
  refreshInventoryCommits();
  if (!commitHistoryReady || inventoryRecoveryRequired_) {
    if (!inventoryRecoveryRequired_) {
      inventoryRecoveryRequired_ = true;
      inventoryRecoveryDetail_ = "Inventatory could not prepare inventory history: " + inventoryPath_.string();
      persistenceError_ = inventoryRecoveryDetail_ + ". It has not been changed.";
    }
    return;
  }
  printerService_.loadConfig(printerPath_);
  refreshPrinterState();
  if (activities_.empty()) {
    activities_.push_back(makeActivity("system", "Inventory loaded"));
    activities_.push_back(makeActivity("system", "Terminal dashboard initialized"));
  }
  // DigiKey metadata is fetched on demand during scan-driven workflows, not at startup.

  if (trim(inventatoryScanConfig_.token).empty()) {
    const auto resolution = resolveWorkspaceScannerCredential(dataPath_, inventatoryScanConfig_);
    switch (resolution.status) {
      case WorkspaceScannerCredentialStatus::Loaded:
        inventatoryScanConfig_.token = resolution.token.value_or(string());
        break;
      case WorkspaceScannerCredentialStatus::FreshCredential:
        inventatoryScanConfig_.token = generateInventatoryScanToken();
        break;
      case WorkspaceScannerCredentialStatus::RequiresPairing:
        inventatoryScanConfig_.token.clear();
        inventatoryScanConfig_.deviceId.clear();
        inventatoryScanConfig_.setupComplete = false;
        setMessage("This workspace does not carry its Scan R1 credential; pair the scanner again", 7);
        break;
    }
  }
  if (!inventatoryScanConfig_.token.empty()) saveScannerCredentialChecked(false);

  vector<string> saveFailures;
  bool inventorySaved = false;
  if (inventoryLoaded || !inventoryFileExists) {
    inventorySaved = store_.save(inventoryPath_);
    if (!inventorySaved) saveFailures.push_back("inventory");
  }
  if (!printerService_.saveConfig(printerPath_)) saveFailures.push_back("printer settings");
  if (!saveScannerConfigChecked(false)) saveFailures.push_back("scanner settings");
  if (scannerCredentialSavePending_ && !saveScannerCredentialChecked(false)) {
    saveFailures.push_back("scanner pairing token");
  }
  if (activityLoadFailed) {
    activitySavePending_ = true;
    saveFailures.push_back("activity history (unreadable; original preserved)");
  } else if (!saveActivitiesChecked(false)) {
    saveFailures.push_back("activity history");
  }
  if (!commitHistoryReady) saveFailures.push_back("inventory commits");
  persistenceError_ = saveFailures.empty()
                          ? string()
                          : "Could not save " + join(saveFailures, ',') + "; changes remain in memory.";
  if (inventorySaved) {
    persistedStore_ = store_;
    persistedStoreValid_ = true;
    refreshInventoryMovements();
  }
}

bool App::reloadInventoryState() {
  InventoryStore loadedStore;
  if (!loadedStore.load(inventoryPath_)) {
    persistenceError_ = "Unable to reload the inventory database; the in-memory data was kept.";
    setMessage(persistenceError_, 5);
    return false;
  }

  vector<InventoryHistoryPoint> loadedHistory;
  if (!loadInventoryHistory(inventoryPath_, loadedHistory)) {
    persistenceError_ = "Unable to reload inventory history; the in-memory data was kept.";
    setMessage(persistenceError_, 5);
    return false;
  }

  vector<InventoryCommit> loadedCommits;
  if (!loadInventoryCommits(inventoryPath_, loadedCommits)) {
    persistenceError_ = "Unable to reload inventory commits; the in-memory data was kept.";
    setMessage(persistenceError_, 5);
    return false;
  }
  InventoryCommitDetail loadedDetail;
  const size_t loadedSelection = loadedCommits.empty()
                                     ? 0
                                     : min(historySelection_, loadedCommits.size() - 1);
  if (!loadedCommits.empty() && !loadInventoryCommit(inventoryPath_, loadedCommits[loadedSelection].id, loadedDetail)) {
    persistenceError_ = "Unable to reload inventory history details; the in-memory data was kept.";
    setMessage(persistenceError_, 5);
    return false;
  }
  if (loadedHistory.empty()) {
    appendInventoryHistory(loadedHistory,
                           makeInventoryHistoryPoint(loadedStore.items(), settings_.lowStockThreshold));
    if (!saveInventoryHistory(inventoryPath_, loadedHistory)) {
      persistenceError_ = "Unable to save reloaded inventory history; the in-memory data was kept.";
      setMessage(persistenceError_, 5);
      return false;
    }
  }

  const auto loadedMovements = loadInventoryMovements(inventoryPath_);
  store_ = move(loadedStore);
  persistedStore_ = store_;
  persistedStoreValid_ = true;
  inventoryHistory_ = move(loadedHistory);
  inventoryMovements_ = loadedMovements;
  inventoryCommits_ = move(loadedCommits);
  historySelection_ = loadedSelection;
  historyRecordSelection_ = 0;
  historyRecordOpen_ = false;
  if (inventoryCommits_.empty()) {
    historyDetail_ = {};
    historyDetailValid_ = false;
  } else {
    historyDetail_ = move(loadedDetail);
    historyDetailValid_ = true;
  }
  persistenceError_.clear();
  return true;
}

bool App::saveInventoryState(const InventoryCommitDraft& draft) {
  if (inventoryRecoveryRequired_) {
    persistenceError_ = inventoryRecoveryDetail_ + ". Recovery is required before Inventatory can save.";
    return false;
  }
  ensureInventoryIdentifiers(store_.items());
  reconcileRackAssignments(store_);
  InventoryCommitDraft effectiveDraft = draft;
  const auto movements = persistedStoreValid_
                             ? inventoryMovementDiff(persistedStore_, store_, effectiveDraft.source,
                                                     effectiveDraft.reference)
                             : vector<InventoryMovement>();
  const auto changes = persistedStoreValid_ ? inventoryCommitDiff(persistedStore_, store_) : vector<InventoryFieldChange>();
  unordered_set<string> changedItems;
  unordered_set<string> changedRacks;
  for (const auto& change : changes) {
    if (change.entityType == "item") changedItems.insert(change.entityId);
    if (change.entityType == "rack") changedRacks.insert(change.entityId);
  }
  if (trim(effectiveDraft.message).empty()) {
    string operation = "Updated inventory";
    if (effectiveDraft.source == "import") operation = "Imported inventory";
    else if (effectiveDraft.source == "stocktake") operation = "Completed stocktake";
    else if (effectiveDraft.source == "bom_build") operation = "Built project inventory";
    else if (effectiveDraft.source == "scanner") operation = "Recorded scanner event";
    else if (effectiveDraft.source == "digikey") operation = "Applied DigiKey enrichment";
    else if (effectiveDraft.source == "undo") operation = "Undid inventory commit";
    else if (effectiveDraft.source == "revert") operation = "Corrected inventory history";
    else if (effectiveDraft.source == "checkpoint") operation = "Inventory checkpoint";
    effectiveDraft.message = operation;
    if (!effectiveDraft.reference.empty()) effectiveDraft.message += " [" + effectiveDraft.reference + "]";
    effectiveDraft.message += " · " + to_string(changedItems.size()) +
                              (changedItems.size() == 1 ? " part, " : " parts, ") +
                              to_string(changedRacks.size()) + (changedRacks.size() == 1 ? " rack" : " racks");
  }
  vector<string> saveFailures;
  InventoryCommit committed;
  const bool needsCommit = effectiveDraft.checkpoint || !changes.empty();
  const bool inventorySaved = needsCommit
                                  ? store_.saveWithCommit(inventoryPath_, persistedStore_, effectiveDraft, movements, nullptr,
                                                          &committed)
                                  : (movements.empty() ? store_.save(inventoryPath_)
                                                       : store_.saveWithMovements(inventoryPath_, movements));
  if (!inventorySaved) {
    saveFailures.push_back("inventory");
    pendingCommitDraft_ = effectiveDraft;
    pendingCommitDraftValid_ = needsCommit;
    pendingMovementSource_ = draft.source;
    pendingMovementReference_ = draft.reference;
  } else {
    persistedStore_ = store_;
    persistedStoreValid_ = true;
    refreshInventoryMovements();
    refreshInventoryCommits();
    pendingCommitDraft_ = {};
    pendingCommitDraftValid_ = false;
    pendingMovementSource_.clear();
    pendingMovementReference_.clear();
  }
  if (!printerService_.saveConfig(printerPath_)) saveFailures.push_back("printer settings");
  if (scannerConfigSavePending_ && !saveScannerConfigChecked(false)) saveFailures.push_back("scanner settings");
  if (!savePendingAppSettings()) saveFailures.push_back("application settings");
  if (!saveActivitiesChecked(false)) saveFailures.push_back("activity history");
  persistenceError_ = saveFailures.empty()
                          ? string()
                          : "Could not save " + join(saveFailures, ',') + "; changes remain in memory.";
  if (!persistenceError_.empty()) {
    setMessage(persistenceError_ + " Press R to retry.", 6);
  }
  return saveFailures.empty();
}

bool App::saveScannerConfigChecked(bool notify) {
  if (saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_)) {
    scannerConfigSavePending_ = false;
    return true;
  }
  scannerConfigSavePending_ = true;
  persistenceError_ = "Could not save scanner settings; changes remain in memory.";
  if (notify) setMessage(persistenceError_ + " Press R to retry.", 6);
  return false;
}

bool App::saveScannerCredentialChecked(bool notify) {
  if (!inventatoryScanConfig_.token.empty() &&
      CredentialStore::writeForWorkspace(dataPath_, kInventatoryScanTokenCredential,
                                         inventatoryScanConfig_.token)) {
    scannerCredentialSavePending_ = false;
    return true;
  }
  scannerCredentialSavePending_ = true;
  persistenceError_ =
      "Could not save the scanner pairing token securely; the Scan R1 service is disabled until it succeeds.";
  if (notify) setMessage(persistenceError_ + " Press R to retry.", 6);
  return false;
}

bool App::savePendingAppSettings() {
  if (!appSettingsSavePending_) return true;
  if (saveAppSettings(settingsPath_, settings_)) {
    appSettingsSavePending_ = false;
    return true;
  }
  persistenceError_ = "Could not save application settings; changes remain in memory.";
  return false;
}

bool App::saveActivitiesChecked(bool notify) {
  if (activityPersistenceBlocked_) {
    activitySavePending_ = true;
    persistenceError_ =
        "Activity history is unreadable; the original file was preserved and will not be overwritten.";
    if (notify) {
      setMessage(persistenceError_, 6);
    }
    return false;
  }
  if (saveActivities(activityPath_, activities_)) {
    activitySavePending_ = false;
    return true;
  }

  activitySavePending_ = true;
  persistenceError_ = "Could not save activity history; changes remain in memory.";
  if (notify) {
    setMessage(persistenceError_ + " Press R to retry.", 6);
  }
  return false;
}

bool App::hasPendingPersistence() const {
  return pendingCommitDraftValid_ || activitySavePending_ || scannerCredentialSavePending_ ||
         scannerConfigSavePending_ || appSettingsSavePending_ ||
         !persistenceError_.empty();
}

bool App::saveState(const string& movementSource, const string& movementReference, const string& commitMessage) {
  InventoryCommitDraft draft;
  draft.source = movementSource;
  draft.reference = movementReference;
  draft.message = commitMessage;
  return saveInventoryState(draft);
}

void App::retrySaveState() {
  const bool stateSaved = pendingCommitDraftValid_
                              ? saveInventoryState(pendingCommitDraft_)
                              : saveState(pendingMovementSource_.empty() ? string("manual") : pendingMovementSource_,
                                          pendingMovementReference_);
  const bool projectsSaved = !bomProjectsDirty_ || saveBomProjects();
  const bool appSettingsSaved = savePendingAppSettings();
  const bool scannerSaved = !scannerConfigSavePending_ || saveScannerConfigChecked(false);
  const bool scannerCredentialSaved = !scannerCredentialSavePending_ || saveScannerCredentialChecked(false);
  if (stateSaved && projectsSaved && appSettingsSaved && scannerSaved && scannerCredentialSaved) {
    if (scannerCredentialSaved && !server_.running() && !inventatoryScanConfig_.token.empty()) {
      restartDeviceService();
    }
    setMessage("All Inventatory changes are saved", 3);
  } else if (!projectsSaved) {
    setMessage("BOM project changes are still unsaved; press R to retry", 5);
  }
}

}  // namespace inventatory
