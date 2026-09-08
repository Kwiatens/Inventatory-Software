// Inventatory - backup, restore, and inventory export workflows.

#include "App.h"
#include "app/AppActionSupport.h"

#include "platform/CredentialStore.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <string>

namespace inventatory {

using namespace std;
using namespace app_actions;

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
