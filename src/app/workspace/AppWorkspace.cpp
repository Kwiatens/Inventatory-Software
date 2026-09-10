// Inventatory - Hardware Inventory Management System
// Workspace selection and cross-workspace state transitions.

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

bool App::chooseInventatoryFolder() {
  filesystem::path selectedPath;
  if (!openFolderDialog(selectedPath, "Select Inventatory folder")) {
    setMessage("Inventatory folder selection cancelled", 2);
    return false;
  }

  if (selectedPath.empty()) {
    setMessage("No folder selected", 2);
    return false;
  }
  if (filesystem::exists(selectedPath / "manifest.tsv")) {
    setMessage("That folder is a backup bundle; use Settings > Restore backup", 6);
    return false;
  }

  const auto selectedInventoryPath = resolveInventoryDatabasePath(selectedPath);
  const auto selectedDataDirectory = selectedInventoryPath.parent_path();
  vector<string> selectedQuickLabels;
  uint32_t selectedQuickLabelRevision = 1;
  error_code selectedQuickLabelsError;
  const auto selectedQuickLabelsPath = quickLabelsPath(selectedDataDirectory);
  const bool selectedQuickLabelsExists = filesystem::exists(selectedQuickLabelsPath, selectedQuickLabelsError);
  if (selectedQuickLabelsError ||
      (selectedQuickLabelsExists &&
       !loadQuickLabels(selectedQuickLabelsPath, selectedQuickLabels, selectedQuickLabelRevision))) {
    setMessage("The selected folder contains unreadable Quick Labels settings", 6);
    return false;
  }
  vector<ActivityEntry> selectedActivities;
  error_code selectedActivityError;
  const auto selectedActivityPath = selectedDataDirectory / "activity.tsv";
  const bool selectedActivityExists = filesystem::exists(selectedActivityPath, selectedActivityError);
  if (selectedActivityError ||
      (selectedActivityExists && !loadActivities(selectedActivityPath, selectedActivities))) {
    setMessage("The selected folder contains unreadable activity history", 6);
    return false;
  }
  error_code selectedPrinterError;
  const auto selectedPrinterPath = selectedDataDirectory / "printer.conf";
  const bool selectedPrinterExists = filesystem::exists(selectedPrinterPath, selectedPrinterError);
  if (selectedPrinterError) {
    setMessage("Unable to inspect the selected printer settings", 6);
    return false;
  }
  if (selectedPrinterExists) {
    LabelPrinterService selectedPrinter;
    if (!selectedPrinter.loadConfig(selectedPrinterPath)) {
      setMessage("The selected folder contains unreadable printer settings", 6);
      return false;
    }
  }
  auto activePaths = InventatoryDataPaths{dataPath_, inventoryPath_, printerPath_, activityPath_, inventatoryScanConfigPath_};
  const auto oldPaths = makeInventatoryDataPaths(dataPath_);
  const auto oldSettings = settings_;
  const auto oldContext = currentWorkspaceContext();
  InventoryStore candidate;
  if (filesystem::exists(selectedInventoryPath)) {
    SqliteConnection candidateConnection;
    string candidateValidationError;
    if (!openDatabaseReadOnly(selectedInventoryPath, candidateConnection)) {
      setMessage("The selected folder contains an inventory database Inventatory cannot load", 6);
      return false;
    }
    if (!validateInventoryDatabase(candidateConnection, &candidateValidationError) ||
        !candidate.load(selectedInventoryPath)) {
      setMessage("The selected folder contains an inventory database Inventatory cannot load", 6);
      return false;
    }
  }
  const bool serviceWasRunning = server_.running();
  mdnsService_.stop();
  server_.stop();
  stopWorkspaceBoundWork();
  if (inventoryRecoveryRequired_) {
    activePaths = makeInventatoryDataPaths(selectedInventoryPath.parent_path());
  } else if (!switchInventatoryDataPathsAfterSaving(activePaths, selectedInventoryPath.parent_path(),
                                                     [this] { return saveState(); })) {
    if (serviceWasRunning) restartDeviceService();
    setMessage(persistenceError_.empty() ? "Unable to save the current Inventatory data" : persistenceError_, 5);
    return false;
  }
  activePaths.inventory = selectedInventoryPath;
  auto candidateSettings = settings_;
  candidateSettings.dataDirectory = activePaths.dataDirectory;
  if (!saveAppSettings(settingsPath_, candidateSettings)) {
    if (serviceWasRunning) restartDeviceService();
    setMessage("Unable to save the selected data folder path; the previous workspace remains active", 5);
    return false;
  }
  settings_ = candidateSettings;
  settingsDraft_ = settings_;
  activateWorkspaceContext(activePaths);
  inventatoryScanConfig_ = {};
  settings_.quickLabelPresets = move(selectedQuickLabels);
  settings_.quickLabelRevision = selectedQuickLabelRevision;

  printerQueues_.clear();
  printerCheck_ = {};
  inventoryHistory_.clear();
  inventoryMovements_.clear();
  deviceEventRecords_.clear();
  scanQueue_.clear();
  importCandidates_.clear();
  importAcceptedItemIds_.clear();
  importSourcePath_.clear();
  importOriginalStore_ = {};
  importStagedStore_ = {};
  importStageActive_ = false;
  importCommitPending_ = false;
  workingCopy_ = {};
  undoSnapshot_ = {};
  editingImportCandidate_ = false;
  importEditIndex_ = 0;
  importSelection_ = 0;
  importSyncPrompt_ = false;
  bomProjects_.clear();
  bomProjectsDirty_ = false;
  activeBomProjectId_.clear();
  bomAnalysisValid_ = false;
  bomAnalysis_ = {};
  bomFile_ = {};
  bomView_ = BomView::List;
  bomProjectSelection_ = 0;
  bomSplitSelection_ = 0;
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomEnrichmentQueue_.clear();
  bomEnrichmentTotal_ = 0;
  bomEnrichmentActiveKey_.clear();
  // Join any in-flight lookup before dropping the client it borrows.
  if (bomEnrichmentFuture_.valid()) {
    bomEnrichmentFuture_.wait();
    bomEnrichmentFuture_ = {};
  }
  bomEnrichmentClient_.reset();
  // The selected workspace may legitimately have no inventory.db.  Clear the
  // old workspace before loadState() so that the missing-file path creates an
  // empty database rather than saving the previous workspace's in-memory
  // inventory into the new folder.
  store_ = {};
  persistedStore_ = {};
  persistedStoreValid_ = false;
  selectedPosition_ = 0;
  searchQuery_.clear();
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  page_ = Page::Home;
  dirty_ = true;

  loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
  loadState();
  if (inventoryRecoveryRequired_) {
    settings_ = oldSettings;
    settingsDraft_ = oldSettings;
    const bool rollbackSaved = saveAppSettings(settingsPath_, oldSettings);
    if (oldContext != nullptr) activateWorkspaceContext(oldContext->paths);
    else activateWorkspaceContext(oldPaths);
    inventatoryScanConfig_ = {};
    loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
    loadState();
    if (!rollbackSaved) {
      appSettingsSavePending_ = true;
      persistenceError_ = "Unable to restore the previous application settings; press R to retry saving.";
    }
    if (serviceWasRunning) restartDeviceService();
    setMessage(rollbackSaved ? "Selected folder also contains an unreadable inventory database"
                             : "Selected folder failed and previous settings could not be restored; press R to retry saving",
               7);
    return false;
  }
  if (!inventatoryScanConfig_.setupComplete) {
    setMessage("Loaded folder; its Scan R1 pairing was not carried over. Pair the scanner again", 7);
    return true;
  }
  if (trim(inventatoryScanConfig_.token).empty()) {
    inventatoryScanConfig_.token = generateInventatoryScanToken();
    if (!saveScannerCredentialChecked(false)) {
      server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                   inventatoryScanReplayStatePath(dataPath_));
      setMessage("Loaded folder, but scanner token storage failed; pair Scan R1 again", 7);
      return true;
    }
    saveScannerConfigChecked(false);
  }
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                               inventatoryScanReplayStatePath(dataPath_));
  if (serviceWasRunning) restartDeviceService();
  setMessage("Loaded Inventatory folder: " + dataPath_.string(), 4);
  return true;
}


}  // namespace inventatory
