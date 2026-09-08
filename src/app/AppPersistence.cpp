// Inventatory - Hardware Inventory Management System
// Workspace loading, persistence, backup, restore, and history actions.

#include "App.h"
#include "app/AppActionSupport.h"

#include "import/CsvFormat.h"
#include "core/InventorySqlite.h"
#include "platform/DigiKeyApi.h"
#include "platform/CredentialStore.h"
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

void App::refreshInventoryMovements() {
  inventoryMovements_ = loadInventoryMovements(inventoryPath_);
}

void App::refreshInventoryCommits() {
  vector<InventoryCommit> loadedCommits;
  if (!loadInventoryCommits(inventoryPath_, loadedCommits)) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not reload inventory history: " + inventoryPath_.string();
    persistenceError_ = inventoryRecoveryDetail_ + ". The previous history was preserved.";
    return;
  }
  InventoryCommitDetail loadedDetail;
  size_t loadedSelection = 0;
  if (!loadedCommits.empty()) {
    loadedSelection = min(historySelection_, loadedCommits.size() - 1);
    if (!loadInventoryCommit(inventoryPath_, loadedCommits[loadedSelection].id, loadedDetail)) {
      inventoryRecoveryRequired_ = true;
      inventoryRecoveryDetail_ = "Inventatory could not reload inventory history details: " + inventoryPath_.string();
      persistenceError_ = inventoryRecoveryDetail_ + ". The previous history was preserved.";
      return;
    }
  }
  inventoryCommits_ = move(loadedCommits);
  historyRecordSelection_ = 0;
  historyRecordOpen_ = false;
  historySelection_ = loadedSelection;
  if (inventoryCommits_.empty()) {
    historySelection_ = 0;
    historyDetailValid_ = false;
    historyDetail_ = {};
    return;
  }
  historyDetail_ = move(loadedDetail);
  historyDetailValid_ = true;
}

void App::refreshHistoryDetail() {
  if (inventoryCommits_.empty()) {
    historyDetail_ = {};
    historyDetailValid_ = false;
    historySelection_ = 0;
    return;
  }
  historySelection_ = min(historySelection_, inventoryCommits_.size() - 1);
  InventoryCommitDetail loadedDetail;
  if (!loadInventoryCommit(inventoryPath_, inventoryCommits_[historySelection_].id, loadedDetail)) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not reload inventory history details: " + inventoryPath_.string();
    persistenceError_ = inventoryRecoveryDetail_ + ". The previous history was preserved.";
    return;
  }
  historyDetail_ = move(loadedDetail);
  historyDetailValid_ = true;
}

void App::moveHistorySelection(int delta) {
  historyRecordSelection_ = 0;
  historyRecordOpen_ = false;
  if (inventoryCommits_.empty()) {
    historySelection_ = 0;
    historyDetailValid_ = false;
    dirty_ = true;
    return;
  }
  const auto current = static_cast<int>(min(historySelection_, inventoryCommits_.size() - 1));
  historySelection_ = static_cast<size_t>(clamp(current + delta, 0,
                                                  static_cast<int>(inventoryCommits_.size() - 1)));
  refreshHistoryDetail();
  dirty_ = true;
}

void App::openSelectedHistoryCommit() {
  if (inventoryCommits_.empty()) {
    setMessage("No inventory commits yet", 3);
    return;
  }
  historyRecordSelection_ = 0;
  historyRecordOpen_ = false;
  refreshHistoryDetail();
  changePage(Page::History);
}

void App::beginHistoryCheckpoint() {
  inputBuffer_.clear();
  inputMode_ = InputMode::HistoryCheckpoint;
  setMessage("Enter a non-empty checkpoint name, then press Enter", 4);
}

void App::beginHistoryRestore(InventoryRevertMode mode) {
  if (!historyDetailValid_) refreshHistoryDetail();
  if (!historyDetailValid_) {
    setMessage("The selected inventory commit could not be loaded", 4);
    return;
  }
  if (persistedStoreValid_ && !inventoryCommitDiff(persistedStore_, store_).empty()) {
    setMessage("Save the current inventory before using History", 4);
    return;
  }
  if (mode == InventoryRevertMode::Reverse && (!historyDetail_.hasParent || historyDetail_.changes.empty())) {
    setMessage("The selected commit has no reversible inventory changes", 4);
    return;
  }
  InventoryStore preview;
  if (mode == InventoryRevertMode::Snapshot) {
    preview = historyDetail_.snapshot;
  } else {
    string conflict;
    if (!prepareInventoryCommitReverse(historyDetail_, store_, preview, conflict)) {
      setMessage(conflict.empty() ? "Reverse blocked because later changes conflict" : conflict, 6);
      return;
    }
  }
  const auto affected = inventoryCommitDiff(store_, preview);
  unordered_set<string> affectedItems;
  unordered_set<string> affectedRacks;
  for (const auto& change : affected) {
    if (change.entityType == "item") affectedItems.insert(change.entityId);
    if (change.entityType == "rack") affectedRacks.insert(change.entityId);
  }
  pendingHistoryRevertMode_ = mode;
  const auto action = mode == InventoryRevertMode::Snapshot ? "Restore" : "Reverse";
  historyConfirmationMessage_ = string(action) + " commit #" + to_string(historyDetail_.commit.sequence) + " (" +
                                historyDetail_.commit.message + ")? This affects " +
                                to_string(affectedItems.size()) + " parts and " + to_string(affectedRacks.size()) +
                                " racks.";
  inputBuffer_.clear();
  inputMode_ = InputMode::HistoryConfirm;
  dirty_ = true;
}

void App::cancelHistoryAction() {
  inputBuffer_.clear();
  historyConfirmationMessage_.clear();
  inputMode_ = InputMode::None;
  dirty_ = true;
}

bool App::applyHistoryRevert(InventoryRevertMode mode) {
  if (!historyDetailValid_) refreshHistoryDetail();
  if (!historyDetailValid_) {
    setMessage("The selected inventory commit could not be loaded", 4);
    cancelHistoryAction();
    return false;
  }
  if (persistedStoreValid_ && !inventoryCommitDiff(persistedStore_, store_).empty()) {
    setMessage("Save the current inventory before using History", 4);
    cancelHistoryAction();
    return false;
  }

  InventoryStore target;
  if (mode == InventoryRevertMode::Snapshot) {
    target = historyDetail_.snapshot;
  } else {
    string conflict;
    if (!prepareInventoryCommitReverse(historyDetail_, store_, target, conflict)) {
      setMessage(conflict.empty() ? "Reverse blocked because later changes conflict" : conflict, 6);
      cancelHistoryAction();
      return false;
    }
  }

  if (inventoryCommitDiff(store_, target).empty()) {
    setMessage("The selected operation would not change inventory", 3);
    cancelHistoryAction();
    return false;
  }

  store_ = move(target);
  InventoryCommitDraft draft;
  draft.source = "revert";
  draft.reference = historyDetail_.commit.id;
  draft.corrective = true;
  draft.revertedCommitId = historyDetail_.commit.id;
  draft.message = mode == InventoryRevertMode::Snapshot
                      ? "Restored snapshot from commit #" + to_string(historyDetail_.commit.sequence)
                      : "Reversed changes from commit #" + to_string(historyDetail_.commit.sequence);
  const bool saved = saveInventoryState(draft);
  cancelHistoryAction();
  syncSelectionToFilter();
  syncRackSelection();
  if (saved) setMessage(mode == InventoryRevertMode::Snapshot ? "Snapshot restored" : "Changes reversed", 4);
  return saved;
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

bool App::exportInventory() {
  filesystem::path target;
  const string filter = string("CSV files (*.csv)") + '\0' + "*.csv" + '\0' +
                        "All files (*.*)" + '\0' + "*.*" + '\0';
  if (!saveFileDialog(target, "Export Inventatory inventory", filter, "csv")) {
    setMessage("Inventory export cancelled", 2);
    return false;
  }

  string error;
  if (!exportInventoryCsv(store_, target, error)) {
    setMessage("Inventory export failed: " + error, 5);
    return false;
  }
  setMessage("Exported " + to_string(store_.items().size()) + " inventory parts to " + target.filename().string(), 5);
  return true;
}

bool App::backupData() {
  filesystem::path parent;
  if (!openFolderDialog(parent, "Choose a folder for the Inventatory backup")) {
    setMessage("Backup cancelled", 2);
    return false;
  }
  if (parent.empty()) {
    setMessage("No backup folder selected", 3);
    return false;
  }

  string stamp = nowTimestampString(time(nullptr));
  replace(stamp.begin(), stamp.end(), ':', '-');
  replace(stamp.begin(), stamp.end(), ' ', '_');
  auto destination = parent / ("Inventatory Backup " + stamp);
  for (int suffix = 2; filesystem::exists(destination); ++suffix) {
    destination = parent / ("Inventatory Backup " + stamp + "-" + to_string(suffix));
  }

  if (!saveState() || !saveQuickLabels(quickLabelsPath_, settings_.quickLabelPresets, settings_.quickLabelRevision)) {
    setMessage("Unable to save current data before backup", 5);
    return false;
  }
  string error;
  if (!createInventatoryBackup(dataPath_, settingsPath_, destination, softwareVersion(), error)) {
    setMessage("Backup failed: " + error, 5);
    return false;
  }
  setMessage("Backup created in " + destination.filename().string(), 6);
  return true;
}

bool App::restoreData() {
  const auto now = time(nullptr);
  filesystem::path selectedBackup;
  if (settingsConfirmAction_ == "restore-backup" && now <= settingsConfirmUntil_ &&
      !pendingRestoreBackupPath_.empty()) {
    selectedBackup = pendingRestoreBackupPath_;
  } else {
    if (!openFolderDialog(selectedBackup, "Choose an Inventatory backup bundle")) {
      setMessage("Restore cancelled", 2);
      return false;
    }
    string validationError;
    if (!validateInventatoryBackup(selectedBackup, validationError)) {
      setMessage("Restore refused: " + validationError, 6);
      return false;
    }
    pendingRestoreBackupPath_ = selectedBackup;
    settingsConfirmAction_ = "restore-backup";
    settingsConfirmUntil_ = now + 5;
    setMessage("Press Restore again within 5 seconds to replace current data", 5);
    dirty_ = true;
    return false;
  }

  settingsConfirmAction_.clear();
  settingsConfirmUntil_ = 0;
  pendingRestoreBackupPath_.clear();
  const bool serviceWasRunning = server_.running();
  mdnsService_.stop();
  server_.stop();
  stopWorkspaceBoundWork();
  if (!saveState() || !saveQuickLabels(quickLabelsPath_, settings_.quickLabelPresets, settings_.quickLabelRevision)) {
    if (serviceWasRunning) restartDeviceService();
    setMessage("Unable to save current data before restore", 5);
    return false;
  }

  string stamp = nowTimestampString(time(nullptr));
  replace(stamp.begin(), stamp.end(), ':', '-');
  replace(stamp.begin(), stamp.end(), ' ', '_');
  auto preRestore = dataPath_.parent_path() / ("Inventatory Pre-Restore " + stamp);
  for (int suffix = 2; filesystem::exists(preRestore); ++suffix) {
    preRestore = dataPath_.parent_path() / ("Inventatory Pre-Restore " + stamp + "-" + to_string(suffix));
  }
  string error;
  if (!createInventatoryBackup(dataPath_, settingsPath_, preRestore, softwareVersion(), error)) {
    if (serviceWasRunning) restartDeviceService();
    setMessage("Restore stopped; automatic pre-restore backup failed: " + error, 7);
    return false;
  }
  bool replacementWorkspaceActiveOnFailure = false;
  if (!restoreInventatoryBackup(selectedBackup, dataPath_, settingsPath_, error, nullptr,
                                &replacementWorkspaceActiveOnFailure)) {
    if (replacementWorkspaceActiveOnFailure) {
      // The replacement directory was published but cleanup or rollback did
      // not complete.  The old in-memory state no longer describes the files
      // on disk, so starting the service here could expose a stale workspace.
      server_.stop();
      mdnsService_.stop();
      inventoryRecoveryRequired_ = true;
      inventoryRecoveryDetail_ = "Restore activated but could not finish safely; the active workspace must be "
                                 "reloaded before saving. " + error;
      persistenceError_ = inventoryRecoveryDetail_;
      setMessage(inventoryRecoveryDetail_, 7);
    } else {
      if (serviceWasRunning) restartDeviceService();
      setMessage("Restore failed; current data was left unchanged: " + error, 7);
    }
    return false;
  }

  AppSettings restored;
  if (!loadAppSettings(settingsPath_, restored)) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Restore activated, but restored application settings could not be loaded. "
                               "Use the pre-restore backup.";
    persistenceError_ = inventoryRecoveryDetail_;
    server_.stop();
    mdnsService_.stop();
    setMessage(inventoryRecoveryDetail_, 7);
    return false;
  }
  settings_ = restored;
  settings_.dataDirectory = dataPath_;
  settingsDraft_ = settings_;
  activateWorkspaceContext(makeInventatoryDataPaths(dataPath_));
  error_code quickLabelsError;
  const bool quickLabelsFileExists = filesystem::exists(quickLabelsPath_, quickLabelsError);
  vector<string> restoredQuickLabels;
  uint32_t restoredQuickLabelRevision = 1;
  if (quickLabelsError ||
      (quickLabelsFileExists &&
       !loadQuickLabels(quickLabelsPath_, restoredQuickLabels, restoredQuickLabelRevision))) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Backup activated, but restored Quick Labels could not be loaded. "
                               "The Scan R1 service remains stopped; use the pre-restore backup.";
    persistenceError_ = inventoryRecoveryDetail_;
    server_.stop();
    mdnsService_.stop();
    setMessage(inventoryRecoveryDetail_, 7);
    return false;
  }
  settings_.quickLabelPresets = move(restoredQuickLabels);
  settings_.quickLabelRevision = restoredQuickLabelRevision;
  inventatoryScanConfig_ = {};
  inventatoryScanConfig_.token = generateInventatoryScanToken();
  const bool scannerTokenStored = CredentialStore::writeForWorkspace(
      dataPath_, kInventatoryScanTokenCredential, inventatoryScanConfig_.token);
  if (!scannerTokenStored) {
    // Do not leave the old credential valid after restoring another workspace.
    CredentialStore::eraseForWorkspace(dataPath_, kInventatoryScanTokenCredential);
    inventatoryScanConfig_.token.clear();
  }
  error_code cleanupError;
  filesystem::remove(inventatoryScanReplayStatePath(dataPath_), cleanupError);
  loadState();
  if (inventoryRecoveryRequired_) {
    server_.stop();
    mdnsService_.stop();
    setMessage("Backup activated, but restored data could not be loaded safely; Scan R1 remains stopped. "
                   "Use the pre-restore backup",
               7);
    return false;
  }
  hasStoredDigiKeySecret_ = CredentialStore::read("digikey-client-secret").has_value();
  applyUiAppearance(settings_.appearance);
  settingsDirty_ = false;
  restartDeviceService();
  string restoreMessage = "Backup restored. Scan R1 pairing was cleared; pair it again";
  if (!scannerTokenStored) restoreMessage += ". Scanner token storage needs attention";
  if (!hasStoredDigiKeySecret_) restoreMessage += ". DigiKey credentials need testing/re-entry";
  setMessage(restoreMessage, 8);
  return true;
}


}  // namespace inventatory
