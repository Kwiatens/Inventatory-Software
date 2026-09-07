// Inventatory - Hardware Inventory Management System
// Shared app state helpers and business actions.

#include "App.h"

#include "import/CsvFormat.h"
#include "core/AtomicFile.h"
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

namespace {

constexpr size_t kDeviceDebugWindowLines = 14;
constexpr size_t kDeviceStatusQueueLimit = 256;
constexpr size_t kDeviceDebugQueueLimit = 512;
constexpr size_t kScanQueueLimit = 256;
constexpr size_t kDeviceQuantityQueueLimit = 64;
constexpr uintmax_t kMaximumImportBytes = 25U * 1024U * 1024U;
constexpr const char* kInventatoryScanTokenCredential = "inventatory-scan-pairing-token";

optional<string> loadWorkspaceScannerToken(const filesystem::path& workspaceDirectory,
                                            const InventatoryScanConfig& config, bool& migratedLegacy) {
  migratedLegacy = false;
  if (const auto scoped = CredentialStore::readForWorkspace(workspaceDirectory, kInventatoryScanTokenCredential);
      scoped.has_value()) {
    return scoped;
  }

  // Versions before workspace-scoped credentials stored one global token.  It
  // is safe to migrate that token only when the workspace carries a completed
  // pairing identity; a fresh/empty workspace must receive a new token.
  if (config.setupComplete && !trim(config.deviceId).empty()) {
    if (const auto legacy = CredentialStore::read(kInventatoryScanTokenCredential); legacy.has_value() &&
        CredentialStore::writeForWorkspace(workspaceDirectory, kInventatoryScanTokenCredential, *legacy)) {
      migratedLegacy = true;
      return legacy;
    }
  }
  return nullopt;
}

void migrateLegacyScannerReplayState(const filesystem::path& workspaceDirectory) {
  const auto legacyPath = appSettingsDirectory() / "inventatory-scan-replay.state";
  const auto scopedPath = inventatoryScanReplayStatePath(workspaceDirectory);
  if (scopedPath.empty() || scopedPath == legacyPath) return;

  error_code error;
  if (filesystem::exists(scopedPath, error) || error) return;
  error.clear();
  if (!filesystem::exists(legacyPath, error) || error) return;

  ifstream input(legacyPath, ios::binary);
  if (!input) return;
  string contents((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
  if (!input.eof() || contents.size() > 256U) return;
  string ignored;
  // The server validates the fingerprint and counter before accepting the
  // state. Atomic replacement ensures a crash cannot leave a partial scope
  // marker that is mistaken for durable replay state.
  writeFileAtomically(scopedPath, contents, &ignored);
}

filesystem::path resolveInventoryDatabasePath(const filesystem::path& selectedPath) {
  error_code error;
  if (selectedPath.empty()) {
    return {};
  }

  if (filesystem::is_regular_file(selectedPath, error) &&
      toLower(selectedPath.extension().string()) == ".db") {
    return selectedPath;
  }

  return selectedPath / "inventory.db";
}

vector<string> splitFlexible(const string& text) {
  vector<string> values;
  string current;
  for (char ch : text) {
    if (ch == ',' || ch == ';' || ch == '\n') {
      current = trim(current);
      if (!current.empty()) {
        values.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }

  current = trim(current);
  if (!current.empty()) {
    values.push_back(current);
  }

  return values;
}

vector<Parameter> parseParameters(const string& text) {
  vector<Parameter> values;
  for (const auto& entry : splitFlexible(text)) {
    const auto equalsPos = entry.find('=');
    if (equalsPos == string::npos) {
      continue;
    }
    values.push_back({trim(entry.substr(0, equalsPos)), trim(entry.substr(equalsPos + 1))});
  }
  return values;
}

bool upsertParameter(vector<Parameter>& parameters, const string& name, const string& value) {
  const auto trimmedValue = trim(value);
  if (trimmedValue.empty()) {
    return false;
  }

  for (auto& parameter : parameters) {
    if (parameterLabelMatches(parameter.name, name)) {
      if (parameter.name.empty()) {
        parameter.name = name;
      }
      const bool changed = parameter.value != trimmedValue;
      parameter.value = trimmedValue;
      return changed;
    }
  }

  parameters.push_back({name, trimmedValue});
  return true;
}

bool mergeDigiKeyMetadata(InventoryItem& item, const DigiKeyProductDetails& details) {
  bool changed = false;

  const auto normalizePackageLabels = [&]() {
    for (auto& parameter : item.parameters) {
      if (parameterLabelMatches(parameter.name, "Package") && looksLikePackagingValue(parameter.value)) {
        parameter.name = "Packaging";
        changed = true;
      }
    }
  };
  normalizePackageLabels();

  const auto assignIfUseful = [&](string& target, const string& value, bool replaceUnknown = false) {
    const auto trimmed = trim(value);
    if (trimmed.empty()) {
      return;
    }
    if (target.empty() || (replaceUnknown && (target == "Unknown" || target == "Unsorted" ||
                                              target == "Scanned DigiKey Item"))) {
      target = trimmed;
      changed = true;
    }
  };

  if ((item.partName.empty() || item.partName == "Scanned DigiKey Item") && !trim(details.productDescription).empty()) {
    item.partName = trim(details.productDescription);
    changed = true;
  }

  assignIfUseful(item.manufacturer, details.manufacturerName, true);
  assignIfUseful(item.category, details.categoryName, true);
  assignIfUseful(item.sku, details.manufacturerPartNumber);
  assignIfUseful(item.productUrl, details.productUrl);
  assignIfUseful(item.datasheetUrl, details.datasheetUrl);

  // The retained provider record is the sole input for deterministic vendor
  // label resolution; direct item fields remain available for the UI.
  item.vendorMetadata = details.vendorMetadata;
  changed = true;

  for (const auto& parameter : details.parameters) {
    if (upsertParameter(item.parameters, parameter.name, parameter.value)) {
      changed = true;
    }
  }

  if (!trim(details.packagingType).empty()) {
    if (upsertParameter(item.parameters, "Packaging", details.packagingType)) {
      changed = true;
    }
  }
  if (!trim(details.packageName).empty()) {
    if (upsertParameter(item.parameters, "Package", details.packageName)) {
      changed = true;
    }
  }
  if (!trim(details.rohsStatus).empty()) {
    if (upsertParameter(item.parameters, "RoHS", details.rohsStatus)) {
      changed = true;
    }
  }
  if (!trim(details.leadStatus).empty()) {
    if (upsertParameter(item.parameters, "Lead Status", details.leadStatus)) {
      changed = true;
    }
  }
  if (!trim(details.productStatus).empty()) {
    if (upsertParameter(item.parameters, "Product Status", details.productStatus)) {
      changed = true;
    }
  }
  if (!trim(details.manufacturerLeadWeeks).empty()) {
    if (upsertParameter(item.parameters, "Lead Time", details.manufacturerLeadWeeks)) {
      changed = true;
    }
  }
  if (!trim(details.quantityAvailable).empty()) {
    if (upsertParameter(item.parameters, "Quantity Available", details.quantityAvailable)) {
      changed = true;
    }
  }
  if (!trim(details.unitPrice).empty()) {
    if (upsertParameter(item.parameters, "Unit Price", details.unitPrice)) {
      changed = true;
    }
  }
  if (!trim(details.detailedDescription).empty() && item.notes.empty()) {
    item.notes = trim(details.detailedDescription);
    changed = true;
  }
  if (!trim(details.lookupKey).empty()) {
    assignIfUseful(item.digikeyPartNumber, details.lookupKey);
  }

  if (item.syncStatus != "synced") {
    item.syncStatus = "synced";
    changed = true;
  }
  item.lastUpdated = time(nullptr);
  return changed;
}

struct DigiKeyApiHandle {
  unique_ptr<DigiKeyApiClient> client;
  string error;
};

DigiKeyApiHandle createDigiKeyApi() {
  const auto config = loadDigiKeyConfig();
  if (!config.valid()) {
    return {nullptr, "DigiKey API credentials are not configured"};
  }

  return {make_unique<DigiKeyApiClient>(config), {}};
}

string digiKeyRefreshLookup(const InventoryItem& item) {
  const auto provider = toLower(trim(item.vendorMetadata.provider));
  const bool taggedDigiKey = any_of(item.tags.begin(), item.tags.end(), [](const string& tag) {
    return toLower(trim(tag)) == "digikey";
  });

  if (!trim(item.digikeyPartNumber).empty()) {
    return trim(item.digikeyPartNumber);
  }
  if (provider == "digikey" && !trim(item.vendorMetadata.providerProductNumber).empty()) {
    return trim(item.vendorMetadata.providerProductNumber);
  }
  // Older imports may retain only the manufacturer/SKU field.  Use that as a
  // keyword lookup only when the item still carries an explicit DigiKey hint.
  if ((provider == "digikey" || taggedDigiKey) && !trim(item.sku).empty()) {
    return trim(item.sku);
  }
  return {};
}

}  // namespace

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
    bool migratedLegacyToken = false;
    if (const auto stored = loadWorkspaceScannerToken(dataPath_, inventatoryScanConfig_, migratedLegacyToken);
        stored.has_value()) {
      inventatoryScanConfig_.token = *stored;
      if (migratedLegacyToken) migrateLegacyScannerReplayState(dataPath_);
    } else {
      inventatoryScanConfig_.token = generateInventatoryScanToken();
    }
  }
  if (!CredentialStore::writeForWorkspace(dataPath_, kInventatoryScanTokenCredential,
                                          inventatoryScanConfig_.token)) {
    setMessage("Unable to save the scanner pairing token securely", 5);
  }

  vector<string> saveFailures;
  bool inventorySaved = false;
  if (inventoryLoaded || !inventoryFileExists) {
    inventorySaved = store_.save(inventoryPath_);
    if (!inventorySaved) saveFailures.push_back("inventory");
  }
  if (!printerService_.saveConfig(printerPath_)) saveFailures.push_back("printer settings");
  if (!saveScannerConfigChecked(false)) saveFailures.push_back("scanner settings");
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
  return pendingCommitDraftValid_ || activitySavePending_ || scannerConfigSavePending_ || appSettingsSavePending_ ||
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
  if (stateSaved && projectsSaved && appSettingsSaved && scannerSaved) {
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
  if (!restoreInventatoryBackup(selectedBackup, dataPath_, settingsPath_, error)) {
    if (serviceWasRunning) restartDeviceService();
    setMessage("Restore failed; current data was left unchanged: " + error, 7);
    return false;
  }

  AppSettings restored;
  if (!loadAppSettings(settingsPath_, restored)) {
    setMessage("Restore activated, but restored settings could not be loaded; use the pre-restore backup", 7);
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
    restartDeviceService();
    setMessage("Backup activated, but restored Quick Labels could not be loaded; use the pre-restore backup", 7);
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
    restartDeviceService();
    setMessage("Backup activated, but restored data could not be loaded safely; use the pre-restore backup", 7);
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
  auto activePaths = InventatoryDataPaths{dataPath_, inventoryPath_, printerPath_, activityPath_, inventatoryScanConfigPath_};
  const auto oldPaths = makeInventatoryDataPaths(dataPath_);
  const auto oldSettings = settings_;
  const auto oldContext = currentWorkspaceContext();
  InventoryStore candidate;
  bool candidateNeedsMigration = false;
  if (filesystem::exists(selectedInventoryPath)) {
    SqliteConnection candidateConnection;
    string candidateValidationError;
    if (!openDatabaseReadOnly(selectedInventoryPath, candidateConnection)) {
      setMessage("The selected folder contains an inventory database Inventatory cannot load", 6);
      return false;
    }
    if (!validateInventoryDatabase(candidateConnection, &candidateValidationError)) {
      candidateNeedsMigration = true;
    } else if (!candidate.load(selectedInventoryPath)) {
      setMessage("The selected folder contains an inventory database Inventatory cannot load", 6);
      return false;
    }
  }
  const bool serviceWasRunning = server_.running();
  mdnsService_.stop();
  server_.stop();
  stopWorkspaceBoundWork();
  if (candidateNeedsMigration && !candidate.load(selectedInventoryPath)) {
    if (serviceWasRunning) restartDeviceService();
    setMessage("The selected folder contains an inventory database that could not be migrated", 6);
    return false;
  }
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
  settings_.quickLabelPresets.clear();
  settings_.quickLabelRevision = 1;
  loadQuickLabels(quickLabelsPath_, settings_.quickLabelPresets, settings_.quickLabelRevision);

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
  bomSplitShortFocused_ = false;
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
  if (trim(inventatoryScanConfig_.token).empty()) {
    inventatoryScanConfig_.token = generateInventatoryScanToken();
    saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
  }
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                               inventatoryScanReplayStatePath(dataPath_));
  if (serviceWasRunning) restartDeviceService();
  setMessage("Loaded Inventatory folder: " + dataPath_.string(), 4);
  return true;
}

void App::captureUndoSnapshot() {
  undoSnapshot_.items = store_.items();
  undoSnapshot_.racks = store_.racks();
  undoSnapshot_.activities = activities_;
  undoSnapshot_.selectedPosition = selectedPosition_;
  undoSnapshot_.valid = true;
}

bool App::undoLastInventoryChange() {
  if (persistedStoreValid_ && !inventoryCommitDiff(persistedStore_, store_).empty()) {
    setMessage("Save the current inventory before using Ctrl+Z", 4);
    return false;
  }
  if (inventoryCommits_.empty()) refreshInventoryCommits();
  const auto latest = find_if(inventoryCommits_.begin(), inventoryCommits_.end(), [](const InventoryCommit& commit) {
    return !commit.checkpoint && (commit.changedItemCount > 0 || commit.changedRackCount > 0) &&
           !commit.parentId.empty();
  });
  if (latest == inventoryCommits_.end()) {
    setMessage("Nothing to undo", 3);
    return false;
  }

  InventoryCommitDetail detail;
  if (!loadInventoryCommit(inventoryPath_, latest->id, detail) || !detail.hasParent) {
    setMessage("The latest inventory commit could not be loaded", 5);
    return false;
  }
  store_ = detail.parentSnapshot;
  InventoryCommitDraft draft;
  draft.source = "undo";
  draft.reference = latest->id;
  draft.corrective = true;
  draft.revertedCommitId = latest->id;
  draft.message = "Undid commit #" + to_string(latest->sequence);
  const bool saved = saveInventoryState(draft);
  syncSelectionToFilter();
  syncRackSelection();
  if (saved) setMessage("Undid commit #" + to_string(latest->sequence), 4);
  return saved;
}

void App::setMessage(string text, int seconds) {
  message_ = move(text);
  messageUntil_ = time(nullptr) + seconds;
  messageFlashStartedAt_ = uiAnimationTicks();
  dirty_ = true;
}

bool App::messageVisible() const {
  return !message_.empty() && time(nullptr) <= messageUntil_;
}

void App::clearMessageIfExpired() {
  if (!messageVisible() && !message_.empty()) {
    message_.clear();
    dirty_ = true;
  }
}

void App::markDirty() {
  dirty_ = true;
}

string App::stockDateFilterName(StockDateFilter filter) const {
  switch (filter) {
    case StockDateFilter::All:
      return "All modification dates";
    case StockDateFilter::Today:
      return "Modified today";
    case StockDateFilter::Last7Days:
      return "Modified in the last 7 days";
    case StockDateFilter::Last30Days:
      return "Modified in the last 30 days";
    case StockDateFilter::OlderThan30Days:
      return "Modified over 30 days ago";
  }
  return "All modification dates";
}

bool App::stockDateFilterMatches(const InventoryItem& item) const {
  if (stockDateFilter_ == StockDateFilter::All) return true;
  if (item.lastUpdated == 0) return false;

  const auto age = difftime(time(nullptr), item.lastUpdated);
  constexpr double day = 24.0 * 60.0 * 60.0;
  switch (stockDateFilter_) {
    case StockDateFilter::Today:
      return age >= 0.0 && age < day;
    case StockDateFilter::Last7Days:
      return age >= 0.0 && age < 7.0 * day;
    case StockDateFilter::Last30Days:
      return age >= 0.0 && age < 30.0 * day;
    case StockDateFilter::OlderThan30Days:
      return age >= 30.0 * day;
    case StockDateFilter::All:
      return true;
  }
  return true;
}

vector<InventorySearchMatch> App::stockSearchMatches() const {
  auto matches = closestSearchActive_
                     ? findClosestPhysicalValues(store_.items(), closestSearchQuery_)
                     : rankedFilterItems(store_.items(), searchQuery_, store_.racks(), settings_.lowStockThreshold);

  if (closestSearchActive_) return matches;

  matches.erase(remove_if(matches.begin(), matches.end(), [&](const InventorySearchMatch& match) {
                  return !stockDateFilterMatches(store_.items()[match.itemIndex]);
                }),
                matches.end());

  const bool valueRanked = any_of(matches.begin(), matches.end(), [](const InventorySearchMatch& match) {
    return match.hasPhysicalComparison;
  });
  const auto bandRank = [](PhysicalValueMatchBand band) {
    switch (band) {
      case PhysicalValueMatchBand::Exact: return 0;
      case PhysicalValueMatchBand::Workable: return 1;
      case PhysicalValueMatchBand::Possible: return 2;
      case PhysicalValueMatchBand::None: return 3;
    }
    return 3;
  };
  const auto fallbackLess = [&](size_t lhs, size_t rhs) {
    const auto& left = store_.items()[lhs];
    const auto& right = store_.items()[rhs];
    if (stockSortOrder_ == StockSortOrder::Quantity && left.quantity != right.quantity) {
      return left.quantity > right.quantity;
    }
    if (stockSortOrder_ != StockSortOrder::Quantity) {
      const auto leftCategory = toLower(displayCategory(left.category));
      const auto rightCategory = toLower(displayCategory(right.category));
      if (leftCategory != rightCategory) {
        return stockSortOrder_ == StockSortOrder::Za ? leftCategory > rightCategory : leftCategory < rightCategory;
      }
    }
    const auto leftName = toLower(left.partName);
    const auto rightName = toLower(right.partName);
    if (leftName != rightName) {
      return stockSortOrder_ == StockSortOrder::Za ? leftName > rightName : leftName < rightName;
    }
    return left.id < right.id;
  };

  sort(matches.begin(), matches.end(), [&](const InventorySearchMatch& lhs, const InventorySearchMatch& rhs) {
    if (valueRanked) {
      const auto leftBand = bandRank(lhs.band);
      const auto rightBand = bandRank(rhs.band);
      if (leftBand != rightBand) return leftBand < rightBand;
      if (lhs.hasPhysicalComparison && rhs.hasPhysicalComparison &&
          lhs.relativeDifference != rhs.relativeDifference) {
        return lhs.relativeDifference < rhs.relativeDifference;
      }
    }
    return fallbackLess(lhs.itemIndex, rhs.itemIndex);
  });
  return matches;
}

vector<size_t> App::filteredIndices() const {
  vector<size_t> indices;
  const auto matches = stockSearchMatches();
  indices.reserve(matches.size());
  for (const auto& match : matches) indices.push_back(match.itemIndex);
  return indices;
}

size_t App::selectedIndex() const {
  const auto matches = stockSearchMatches();
  if (matches.empty()) {
    return numeric_limits<size_t>::max();
  }
  const auto position = closestSearchActive_ ? min(closestSelectedPosition_, matches.size() - 1)
                                             : min(selectedPosition_, matches.size() - 1);
  return matches[position].itemIndex;
}

InventoryItem* App::selectedItem() {
  const auto index = selectedIndex();
  if (index == numeric_limits<size_t>::max()) {
    return nullptr;
  }
  return &store_.items()[index];
}

const InventoryItem* App::selectedItem() const {
  const auto index = selectedIndex();
  if (index == numeric_limits<size_t>::max()) {
    return nullptr;
  }
  return &store_.items()[index];
}

void App::syncSelectionToFilter() {
  const auto matches = stockSearchMatches();
  if (matches.empty()) {
    if (closestSearchActive_) closestSelectedPosition_ = 0;
    else selectedPosition_ = 0;
    return;
  }
  if (closestSearchActive_) {
    if (closestSelectedPosition_ >= matches.size()) closestSelectedPosition_ = matches.size() - 1;
  } else if (selectedPosition_ >= matches.size()) {
    selectedPosition_ = matches.size() - 1;
  }
  dirty_ = true;
}

void App::moveSelection(int delta) {
  const auto matches = stockSearchMatches();
  if (matches.empty()) {
    if (closestSearchActive_) closestSelectedPosition_ = 0;
    else selectedPosition_ = 0;
    return;
  }

  const auto currentPosition = closestSearchActive_ ? closestSelectedPosition_ : selectedPosition_;
  const auto current = static_cast<int>(min(currentPosition, matches.size() - 1));
  const auto next = clamp(current + delta, 0, static_cast<int>(matches.size() - 1));
  if (closestSearchActive_) closestSelectedPosition_ = static_cast<size_t>(next);
  else selectedPosition_ = static_cast<size_t>(next);
  dirty_ = true;
}

bool App::deleteConfirmationActive() const {
  return !deleteConfirmationItemId_.empty();
}

bool App::deleteConfirmationReady() const {
  return deleteConfirmationActive() && time(nullptr) >= deleteConfirmationUntil_;
}

int App::deleteConfirmationSecondsLeft() const {
  if (!deleteConfirmationActive()) {
    return 0;
  }
  return max(0, static_cast<int>(deleteConfirmationUntil_ - time(nullptr)));
}

void App::armDeleteConfirmation() {
  const auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return;
  }

  deleteConfirmationItemId_ = item->id;
  deleteConfirmationUntil_ = time(nullptr) + 3;
  dirty_ = true;
}

void App::cancelDeleteConfirmation() {
  if (deleteConfirmationItemId_.empty()) {
    return;
  }

  deleteConfirmationItemId_.clear();
  deleteConfirmationUntil_ = 0;
  dirty_ = true;
}

void App::clearDeleteConfirmationIfExpired() {
  // Keep the confirmation popup visible after the countdown reaches zero.
}

void App::confirmDeleteSelectedItem() {
  if (!deleteConfirmationActive()) {
    setMessage("Press Ctrl+Backspace first to arm delete", 2);
    return;
  }

  if (!deleteConfirmationReady()) {
    setMessage("Wait " + to_string(deleteConfirmationSecondsLeft()) + " more second" +
                   (deleteConfirmationSecondsLeft() == 1 ? string() : string("s")) + " to confirm delete",
               2);
    return;
  }

  const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& item) {
    return item.id == deleteConfirmationItemId_;
  });
  if (it == store_.items().end()) {
    cancelDeleteConfirmation();
    setMessage("Item no longer available", 2);
    return;
  }

  const auto itemName = it->partName;
  store_.items().erase(it);
  cancelDeleteConfirmation();
  logActivity("delete", itemName + " deleted");
  saveState();
  syncSelectionToFilter();
  page_ = Page::Stock;
  setMessage(itemName + " deleted", 2);
}

void App::changePage(Page page) {
  if (page != page_ && page_ == Page::Stock && stocktakeActive_) {
    setMessage("Finish or cancel the stocktake before leaving Stock", 5);
    return;
  }
  if (page != page_ && page_ == Page::Import && importCommitPending_) {
    setMessage("Import is not saved yet. Press R to retry or Q to cancel it.", 6);
    return;
  }
  if (page != page_ && page_ == Page::Import && importStageActive_) {
    cancelImportSession();
  }
  if (page != page_ && page_ == Page::Settings && settingsDirty_ && inputMode_ != InputMode::ExitConfirmation) {
    pendingPageAfterSettings_ = page;
    inputMode_ = InputMode::ExitConfirmation;
    setMessage("Unsaved settings: press S to save, D to discard, or Esc to stay", 5);
    return;
  }
  page_ = page;
  inputMode_ = InputMode::None;
  if (page != Page::Stock) {
    closestSearchActive_ = false;
    closestSearchQuery_.clear();
    closestSelectedPosition_ = 0;
  }
  focusedTarget_ = -1;
  cancelDeleteConfirmation();
  if (page != Page::Racks) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
  }
  if (page != Page::Projects) {
    // Leaving the page abandons an in-progress walkthrough rather than letting
    // a half-finished pick resume out of context later.
    bomDeductPrompt_ = false;
    if (bomView_ == BomView::Build) {
      bomView_ = BomView::Split;
      bomBuildStep_ = 0;
    }
    bomDeleteConfirmationProjectId_.clear();
    bomDeleteConfirmationUntil_ = 0;
  }
  dirty_ = true;
}

void App::openSelectedDetail() {
  if (selectedItem() != nullptr) {
    page_ = Page::Stock;
    dirty_ = true;
  }
}

void App::openRackManagement() {
  syncRackSelection();
  changePage(Page::Racks);
  setMessage(store_.racks().empty() ? "No Inventatory racks exist yet; eligible parts create racks automatically"
                                    : "Rack management opened",
             3);
}

vector<size_t> App::sortedRackIndices() const {
  vector<size_t> indices;
  indices.reserve(store_.racks().size());
  for (size_t index = 0; index < store_.racks().size(); ++index) {
    if (!rackFilter_.empty()) {
      const auto& rack = store_.racks()[index];
      const auto filter = toLower(rackFilter_);
      const auto occupied = rackOccupiedSlotCount(store_, rack);
      const auto full = occupied >= static_cast<size_t>(rack.rows * rack.columns);
      const bool matchesSpecial = (filter == "free" && !full) || (filter == "full" && full) ||
                                  (filter == "empty" && occupied == 0);
      if (!matchesSpecial && !containsInsensitive(rack.code, rackFilter_) &&
          !containsInsensitive(rack.componentType, rackFilter_)) {
        continue;
      }
    }
    indices.push_back(index);
  }
  sort(indices.begin(), indices.end(), [&](size_t lhs, size_t rhs) {
    const auto lhsNumber = rackNumberFromCode(store_.racks()[lhs].code);
    const auto rhsNumber = rackNumberFromCode(store_.racks()[rhs].code);
    if (lhsNumber != rhsNumber) return lhsNumber < rhsNumber;
    return store_.racks()[lhs].code < store_.racks()[rhs].code;
  });
  return indices;
}

const InventatoryRack* App::selectedRack() const {
  const auto indices = sortedRackIndices();
  if (indices.empty()) return nullptr;
  const auto position = min(rackSelection_, indices.size() - 1);
  return &store_.racks()[indices[position]];
}

InventatoryRack* App::selectedRack() {
  const auto indices = sortedRackIndices();
  if (indices.empty()) return nullptr;
  const auto position = min(rackSelection_, indices.size() - 1);
  return &store_.racks()[indices[position]];
}

string App::selectedRackSlot() const {
  return rackSlotLabel(rackRow_, rackColumn_);
}

InventoryItem* App::selectedRackItem() {
  const auto* rack = selectedRack();
  if (rack == nullptr) return nullptr;
  return itemAtRackSlot(store_, rack->id, selectedRackSlot());
}

const InventoryItem* App::selectedRackItem() const {
  const auto* rack = selectedRack();
  if (rack == nullptr) return nullptr;
  return itemAtRackSlot(store_, rack->id, selectedRackSlot());
}

void App::syncRackSelection() {
  const auto indices = sortedRackIndices();
  if (indices.empty()) {
    rackSelection_ = 0;
    rackRow_ = 0;
    rackColumn_ = 0;
    return;
  }
  rackSelection_ = min(rackSelection_, indices.size() - 1);
  rackRow_ = clamp(rackRow_, 0, 4);
  rackColumn_ = clamp(rackColumn_, 0, 4);
  dirty_ = true;
}

void App::renameSelectedRack(const string& value) {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }
  auto code = trim(value);
  transform(code.begin(), code.end(), code.begin(), [](unsigned char ch) { return static_cast<char>(toupper(ch)); });
  if (rackNumberFromCode(code) <= 0 || code != "R" + to_string(rackNumberFromCode(code))) {
    setMessage("Rack code must look like R12", 3);
    return;
  }
  const auto duplicate = find_if(store_.racks().begin(), store_.racks().end(), [&](const InventatoryRack& candidate) {
    return candidate.id != rack->id && toLower(candidate.code) == toLower(code);
  });
  if (duplicate != store_.racks().end()) {
    setMessage(code + " already exists", 3);
    return;
  }
  captureUndoSnapshot();
  const auto previous = rack->code;
  rack->code = code;
  logActivity("rack", previous + " renamed to " + code);
  saveState();
  syncRackSelection();
  setMessage("Rack renamed to " + code, 2);
}

void App::changeSelectedRackType(const string& value) {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }
  const auto type = trim(value);
  if (type.empty()) {
    setMessage("Rack type cannot be empty", 3);
    return;
  }
  captureUndoSnapshot();
  const auto previous = rack->componentType;
  rack->componentType = type;
  logActivity("rack", rack->code + " type " + previous + " -> " + type);
  saveState();
  syncRackSelection();
  setMessage(rack->code + " type updated", 2);
}

void App::createRackWithType(const string& value) {
  const auto type = trim(value);
  if (type.empty()) {
    setMessage("Rack type cannot be empty", 3);
    return;
  }
  int nextNumber = 1;
  for (const auto& rack : store_.racks()) {
    nextNumber = max(nextNumber, rackNumberFromCode(rack.code) + 1);
  }
  InventatoryRack rack;
  rack.id = makeId();
  rack.code = "R" + to_string(nextNumber);
  rack.componentType = type;
  rack.createdAt = time(nullptr);
  captureUndoSnapshot();
  store_.racks().push_back(rack);
  rackFilter_.clear();
  rackSelection_ = sortedRackIndices().empty() ? 0 : sortedRackIndices().size() - 1;
  logActivity("rack", rack.code + " created for " + type);
  saveState();
  syncRackSelection();
  setMessage(rack.code + " created", 2);
}

void App::deleteSelectedRack() {
  const auto indices = sortedRackIndices();
  if (indices.empty()) {
    setMessage("No rack selected", 2);
    return;
  }
  const auto position = min(rackSelection_, indices.size() - 1);
  const auto rackIndex = indices[position];
  const auto& rack = store_.racks()[rackIndex];
  if (rackOccupiedSlotCount(store_, rack) != 0) {
    setMessage("Only empty racks can be deleted", 3);
    return;
  }
  const auto code = rack.code;
  captureUndoSnapshot();
  store_.racks().erase(store_.racks().begin() + static_cast<ptrdiff_t>(rackIndex));
  if (rackSelection_ > 0) --rackSelection_;
  movingRackItemId_.clear();
  movingRackSource_.clear();
  logActivity("rack", code + " deleted");
  saveState();
  syncRackSelection();
  setMessage(code + " deleted", 2);
}

void App::jumpToRack(const string& value) {
  const auto requested = toLower(trim(value));
  if (requested.empty()) {
    setMessage("Enter a rack code like R3", 2);
    return;
  }
  const auto indices = sortedRackIndices();
  for (size_t position = 0; position < indices.size(); ++position) {
    if (toLower(store_.racks()[indices[position]].code) == requested) {
      rackSelection_ = position;
      rackRow_ = 0;
      rackColumn_ = 0;
      setMessage("Jumped to " + store_.racks()[indices[position]].code, 2);
      dirty_ = true;
      return;
    }
  }
  setMessage("Rack not visible/found: " + value, 3);
}

void App::beginRackFilter() {
  inputBuffer_ = rackFilter_;
  inputMode_ = InputMode::RackFilter;
  setMessage("Filter by rack code, type, free, full, or empty", 4);
}

void App::adjustSelectedRackItemQuantity(int delta) {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }

  captureUndoSnapshot();
  item->quantity = max(0, item->quantity + delta);
  item->lastUpdated = time(nullptr);
  logActivity(delta > 0 ? "stock" : "usage", item->partName + " quantity changed to " + to_string(item->quantity));
  saveState();
  setMessage(item->partName + " quantity is now " + to_string(item->quantity), 2);
  dirty_ = true;
}

void App::openSelectedRackItemDetail() {
  const auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }

  const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& candidate) {
    return candidate.id == item->id;
  });
  if (it == store_.items().end()) {
    setMessage("Selected part is no longer available", 2);
    return;
  }

  // The stock selection is position-based, so reset the stock query before
  // selecting a rack part to ensure the detail panel can always show it.
  searchQuery_.clear();
  selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
  inputMode_ = InputMode::None;
  focusedTarget_ = -1;
  page_ = Page::Stock;
  syncSelectionToFilter();
  dirty_ = true;
}

bool App::printSelectedRackPartLabel() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return false;
  }
  const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& candidate) {
    return candidate.id == item->id;
  });
  if (it != store_.items().end()) {
    selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
  }
  return printSelectedLabel();
}

bool App::printSelectedRackLabel() {
  const auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return false;
  }

  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("No printer configured", 3);
    openPrinterSetup();
    return false;
  }

  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Printer unavailable while the workspace is changing", 4);
    return false;
  }
  if (printerWorkCompletion_ != nullptr) {
    setMessage("A printer job is already running; try again shortly", 3);
    return false;
  }

  PrinterWork work;
  work.kind = PrinterWorkKind::PrintRack;
  work.workspaceGeneration = context->generation;
  work.printerName = printerService_.configuredPrinter();
  work.rack = *rack;
  if (!enqueuePrinterWork(move(work))) {
    setMessage("Printer request queue is full; try again shortly", 4);
    return false;
  }
  setMessage("Printer job queued", 3);
  return true;
}

void App::moveRackSlot(int rowDelta, int columnDelta) {
  rackRow_ = clamp(rackRow_ + rowDelta, 0, 4);
  rackColumn_ = clamp(rackColumn_ + columnDelta, 0, 4);
  dirty_ = true;
}

void App::moveRackPage(int delta) {
  const auto indices = sortedRackIndices();
  if (indices.empty()) {
    rackSelection_ = 0;
    return;
  }
  const auto current = static_cast<int>(min(rackSelection_, indices.size() - 1));
  rackSelection_ = static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(indices.size() - 1)));
  movingRackItemId_.clear();
  movingRackSource_.clear();
  dirty_ = true;
}

void App::beginOrCompleteRackMove() {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }

  const auto slot = selectedRackSlot();
  auto* item = itemAtRackSlot(store_, rack->id, slot);
  if (movingRackItemId_.empty()) {
    if (item == nullptr) {
      setMessage("Select an occupied slot to move", 2);
      return;
    }
    movingRackItemId_ = item->id;
    movingRackSource_ = rack->code + "-" + slot;
    setMessage("Moving " + item->partName + "; choose an empty slot and press v", 4);
    dirty_ = true;
    return;
  }

  auto* movingItem = store_.findById(movingRackItemId_);
  if (movingItem == nullptr) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
    setMessage("Moving item no longer exists", 3);
    return;
  }
  if (rackLocation(*movingItem, store_.racks()) == rack->code + "-" + slot) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
    setMessage("Move cancelled", 2);
    dirty_ = true;
    return;
  }
  if (item != nullptr) {
    setMessage(rack->code + "-" + slot + " is already occupied", 3);
    return;
  }

  string error;
  captureUndoSnapshot();
  if (!moveItemToRackSlot(store_, *movingItem, *rack, slot, error)) {
    undoSnapshot_.valid = false;
    setMessage(error, 4);
    return;
  }
  movingItem->lastUpdated = time(nullptr);
  const auto target = rack->code + "-" + slot;
  logActivity("rack", movingItem->partName + " moved " + movingRackSource_ + " -> " + target);
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage("Moved to " + target, 2);
  dirty_ = true;
}

void App::unassignSelectedRackItem() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }
  const auto previous = rackLocation(*item, store_.racks());
  captureUndoSnapshot();
  unassignItemFromRack(*item);
  item->lastUpdated = time(nullptr);
  logActivity("rack", item->partName + " unassigned from " + previous);
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage("Rack location intentionally unassigned", 2);
}

void App::autoAssignSelectedRackItem() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }
  const auto previous = rackLocation(*item, store_.racks());
  captureUndoSnapshot();
  restoreAutomaticRackAssignment(store_, *item);
  item->lastUpdated = time(nullptr);
  const auto next = rackLocation(*item, store_.racks());
  logActivity("rack", item->partName + " AUTO " + previous + " -> " + (next.empty() ? "unassigned" : next));
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage(next.empty() ? "AUTO found no eligible rack placement" : "AUTO assigned " + next, 3);
}

void App::startSearch() {
  page_ = Page::Stock;
  closestSearchActive_ = false;
  closestSearchQuery_.clear();
  closestSelectedPosition_ = 0;
  inputMode_ = InputMode::Search;
  searchQueryBeforeEdit_ = searchQuery_;
  inputBuffer_ = searchQuery_;
  setMessage("Type to filter immediately; Enter keeps it, Esc restores the previous filter", 3);
}

void App::startClosestSearch() {
  page_ = Page::Stock;
  closestSearchActive_ = true;
  inputMode_ = InputMode::ClosestSearch;
  inputBuffer_ = closestSearchQuery_;
  closestSelectedPosition_ = 0;
  setMessage("Enter a physical value; results are ranked across the whole database", 4);
  dirty_ = true;
}

void App::clearClosestSearch() {
  closestSearchActive_ = false;
  closestSearchQuery_.clear();
  closestSelectedPosition_ = 0;
  if (inputMode_ == InputMode::ClosestSearch) inputMode_ = InputMode::None;
  inputBuffer_.clear();
  syncSelectionToFilter();
  dirty_ = true;
}

void App::cancelInput() {
  inputMode_ = InputMode::None;
  inputBuffer_.clear();
  dirty_ = true;
}

void App::beginEditCurrentItem(bool createNew) {
  editingImportCandidate_ = false;
  page_ = Page::Stock;
  workingCopy_ = {};
  if (createNew) {
    workingCopy_.isNew = true;
    workingCopy_.item.id = makeId();
    workingCopy_.item.partName = "New Part";
    workingCopy_.item.manufacturer = "Unknown";
    workingCopy_.item.category = "Unsorted";
    workingCopy_.item.location = "Unassigned";
    workingCopy_.item.syncStatus = "needs_metadata";
    workingCopy_.item.lastUpdated = time(nullptr);
    workingCopy_.item.createdAt = workingCopy_.item.lastUpdated;
    workingCopy_.originalIndex = store_.items().size();
  } else {
    const auto* current = selectedItem();
    if (current == nullptr) {
      setMessage("No item selected", 2);
      return;
    }
    workingCopy_.isNew = false;
    workingCopy_.item = *current;
    workingCopy_.originalIndex = selectedIndex();
  }

  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
  setMessage(createNew ? "Editing new part" : "Editing " + workingCopy_.item.partName, 3);
}

void App::beginEditImportCandidate() {
  auto* candidate = currentImportCandidate();
  if (candidate == nullptr) {
    setMessage("No import row selected", 2);
    return;
  }

  editingImportCandidate_ = true;
  importEditIndex_ = importSelection_;
  workingCopy_ = {};
  workingCopy_.item = candidate->item;
  workingCopy_.originalIndex = importSelection_;
  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
  page_ = Page::Import;
  setMessage("Editing import row: \xE2\x86\x91\xE2\x86\x93 field, \xE2\x8F\x8E edit, s save, esc cancel", 4);
}

void App::openFieldMenu() {
  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
}

void App::commitEditField(EditField field, const string& value) {
  const auto trimmed = trim(value);
  bool valid = true;

  switch (field) {
    case EditField::PartName:
      workingCopy_.item.partName = trimmed;
      break;
    case EditField::Manufacturer:
      workingCopy_.item.manufacturer = trimmed;
      break;
    case EditField::Category:
      workingCopy_.item.category = trimmed;
      break;
    case EditField::Quantity:
      try {
        workingCopy_.item.quantity = max(0, stoi(trimmed));
      } catch (...) {
        valid = false;
      }
      break;
    case EditField::ReorderThreshold:
      try {
        workingCopy_.item.reorderThreshold = max(0, stoi(trimmed));
      } catch (...) {
        valid = false;
      }
      break;
    case EditField::Location:
      workingCopy_.item.location = trimmed;
      break;
    case EditField::Tags:
      workingCopy_.item.tags = splitFlexible(trimmed);
      break;
    case EditField::Parameters:
      workingCopy_.item.parameters = parseParameters(trimmed);
      break;
    case EditField::Notes:
      workingCopy_.item.notes = trimmed;
      break;
    case EditField::LabelOverride:
      workingCopy_.item.labelOverride = trimmed;
      break;
    case EditField::DigiKeyPart:
      workingCopy_.item.digikeyPartNumber = trimmed;
      break;
    case EditField::DatasheetUrl:
      workingCopy_.item.datasheetUrl = trimmed;
      break;
    case EditField::ProductUrl:
      workingCopy_.item.productUrl = trimmed;
      break;
    case EditField::Sku:
      workingCopy_.item.sku = trimmed;
      break;
    case EditField::RackLocation: {
      string error;
      if (!setManualRackLocation(store_, workingCopy_.item, value, error)) {
        setMessage(error, 4);
        return;
      }
      break;
    }
  }

  if (!valid) {
    setMessage("Invalid numeric value", 3);
    return;
  }

  workingCopy_.item.lastUpdated = time(nullptr);
  setMessage(fieldLabel(field) + " updated", 2);
  inputBuffer_.clear();
  inputMode_ = InputMode::EditFieldMenu;
  dirty_ = true;
}

void App::saveWorkingCopy() {
  if (editingImportCandidate_) {
    if (importEditIndex_ < importCandidates_.size()) {
      importCandidates_[importEditIndex_].item = workingCopy_.item;
    }

    editingImportCandidate_ = false;
    inputMode_ = InputMode::None;
    page_ = Page::Import;
    setMessage("Import row updated", 2);
    dirty_ = true;
    return;
  }

  captureUndoSnapshot();
  if (workingCopy_.isNew) {
    store_.items().push_back(workingCopy_.item);
    reconcileRackAssignment(store_, store_.items().back());
    selectedPosition_ = store_.items().empty() ? 0 : store_.items().size() - 1;
  } else if (workingCopy_.originalIndex < store_.items().size()) {
    store_.items()[workingCopy_.originalIndex] = workingCopy_.item;
    reconcileRackAssignment(store_, store_.items()[workingCopy_.originalIndex]);
  }

  logActivity("edit", workingCopy_.item.partName + " updated");
  saveState();
  inputMode_ = InputMode::None;
  page_ = Page::Stock;
  syncSelectionToFilter();
  setMessage("Changes saved", 2);
}

void App::adjustQuantity(int delta) {
  auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return;
  }

  captureUndoSnapshot();
  const auto candidate = static_cast<long long>(item->quantity) + delta;
  item->quantity = static_cast<int>(clamp<long long>(candidate, 0, numeric_limits<int>::max()));
  item->lastUpdated = time(nullptr);
  logActivity(delta > 0 ? "stock" : "usage", item->partName + " quantity changed to " + to_string(item->quantity));
  const bool saved = saveState();
  setMessage(saved ? item->partName + " quantity is now " + to_string(item->quantity)
                  : "Quantity changed in memory; press R to retry saving",
             saved ? 2 : 5);
  dirty_ = true;
}

void App::setSelectedQuantityFromInput(const string& value) {
  auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return;
  }
  if (value.empty()) {
    setMessage("Enter a quantity from 0 to 2147483647", 4);
    return;
  }

  long long parsed = -1;
  try {
    size_t consumed = 0;
    parsed = stoll(value, &consumed);
    if (consumed != value.size() || parsed < 0 || parsed > numeric_limits<int>::max()) parsed = -1;
  } catch (...) {
    parsed = -1;
  }
  if (parsed < 0) {
    setMessage("Quantity must be a whole number from 0 to 2147483647", 4);
    return;
  }
  if (item->quantity == parsed) {
    setMessage("Quantity unchanged", 2);
    return;
  }

  captureUndoSnapshot();
  const auto previous = item->quantity;
  item->quantity = static_cast<int>(parsed);
  item->lastUpdated = time(nullptr);
  logActivity(parsed > previous ? "stock" : "usage",
              item->partName + " quantity set to " + to_string(item->quantity));
  const bool saved = saveState();
  setMessage(saved ? item->partName + " quantity set to " + to_string(item->quantity)
                  : "Quantity changed in memory; press R to retry saving",
             saved ? 2 : 5);
  dirty_ = true;
}

void App::logActivity(const string& kind, const string& message) {
  appendActivity(activities_, makeActivity(kind, message));
  const auto now = time(nullptr);
  if (kind == "scan") {
    scannerFlashUntil_ = now + 3;
  } else if (kind == "print") {
    printerFlashUntil_ = now + 3;
  }
  saveActivitiesChecked();
  dirty_ = true;
}

void App::toggleAutoPrintScannedLabels() {
  autoPrintScannedLabels_ = !autoPrintScannedLabels_;
  settings_.autoPrintScannedLabels = autoPrintScannedLabels_;
  settingsDraft_.autoPrintScannedLabels = autoPrintScannedLabels_;
  if (!saveAppSettings(settingsPath_, settings_)) {
    appSettingsSavePending_ = true;
    settingsDirty_ = true;
    persistenceError_ = "Could not save application settings; changes remain in memory.";
    setMessage(persistenceError_ + " Press R to retry.", 6);
  } else {
    appSettingsSavePending_ = false;
    setMessage(autoPrintScannedLabels_ ? "Auto label printing enabled" : "Auto label printing disabled", 3);
  }
  dirty_ = true;
}

bool App::autoPrintScannedLabel(const string& itemId) {
  if (!autoPrintScannedLabels_) {
    return false;
  }

  const auto* item = store_.findById(itemId);
  if (item == nullptr) {
    return false;
  }

  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("Auto label skipped: no printer configured", 4);
    return true;
  }

  printLabelForItem(*item, "Auto-printed label for ", false);
  return true;
}

void App::pushScanCode(const DeviceScanRequest& request) {
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;
  lock_guard<mutex> lock(scanMutex_);
  if (scanQueue_.size() >= kScanQueueLimit) {
    return;
  }
  scanQueue_.push_back({request, context->generation});
}

void App::processScans() {
  vector<QueuedScan> pending;
  {
    lock_guard<mutex> lock(scanMutex_);
    pending.swap(scanQueue_);
  }

  for (const auto& queued : pending) {
    if (!workspaceIsCurrent(queued.workspaceGeneration)) continue;
    const auto& request = queued.request;
    const auto& code = request.code;
    const auto resolution = resolveScanCode(store_, code);
    if (resolution.matched) {
      if (auto* item = store_.findById(resolution.itemId)) {
        const auto shouldTrySync = resolution.created || trim(item->syncStatus) != "synced" ||
                                   trim(item->partName) == "Scanned DigiKey Item";
        if (shouldTrySync) {
          const auto lookup = !trim(item->digikeyPartNumber).empty() ? item->digikeyPartNumber : code;
          if (!trim(lookup).empty()) scanDigiKeyEnrichmentQueue_.emplace_back(item->id, lookup);
        }
      }

      if (resolution.created) {
        if (auto* item = store_.findById(resolution.itemId)) {
          item->quantity = max(0, request.quantity);
          reconcileRackAssignment(store_, *item);
        }
        logActivity("scan", "Created item from code " + code + " qty " + to_string(max(0, request.quantity)));
      } else {
        if (auto* item = store_.findById(resolution.itemId)) {
          reconcileRackAssignment(store_, *item);
        }
        logActivity("scan", "Matched existing item with code " + code);
      }

      if (const auto* item = store_.findById(resolution.itemId)) {
        const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& entry) {
          return entry.id == item->id;
        });
        if (it != store_.items().end()) {
          selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
        }
      }

      changePage(Page::Stock);
      saveState("scanner", code, "Scanner event " + code);
      if (!resolution.created || !autoPrintScannedLabel(resolution.itemId)) {
        setMessage(resolution.message, 3);
      }
    } else {
      setMessage("Scan ignored: " + resolution.message, 3);
    }
    syncSelectionToFilter();
  }
}

void App::processScanDigiKeyEnrichment() {
  if (scanDigiKeyEnrichmentFuture_.valid()) {
    if (scanDigiKeyEnrichmentFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
    const auto result = scanDigiKeyEnrichmentFuture_.get();
    if (!workspaceIsCurrent(result.workspaceGeneration)) {
      return;
    }
    if (result.details) {
      if (auto* item = store_.findById(result.itemId); item != nullptr && mergeDigiKeyMetadata(*item, *result.details)) {
        logActivity("scan", "Synced DigiKey metadata for " + item->partName);
        saveState("digikey", result.itemId, "DigiKey enrichment");
      }
    }
  }
  if (scanDigiKeyEnrichmentQueue_.empty()) return;
  const auto [itemId, lookup] = scanDigiKeyEnrichmentQueue_.front();
  scanDigiKeyEnrichmentQueue_.pop_front();
  const auto config = loadDigiKeyConfig();
  if (!config.valid()) return;
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;
  const auto generation = context->generation;
  scanDigiKeyEnrichmentFuture_ = async(launch::async, [itemId, lookup, config, generation] {
    ScanDigiKeyEnrichmentResult result;
    result.itemId = itemId;
    result.workspaceGeneration = generation;
    DigiKeyApiClient client(config);
    string error;
    result.details = client.fetchProductDetails(lookup, &error);
    return result;
  });
}

void App::beginDigiKeyRefresh() {
  if (!digiKeyRefreshQueue_.empty() || digiKeyRefreshFuture_.valid()) {
    setMessage("DigiKey inventory refresh is already running", 3);
    return;
  }
  if (settingsDirty_) {
    setMessage("Save DigiKey settings before refreshing inventory data", 4);
    return;
  }

  auto api = createDigiKeyApi();
  if (api.client == nullptr) {
    setMessage("DigiKey refresh unavailable: " + api.error, 5);
    return;
  }

  digiKeyRefreshQueue_.clear();
  digiKeyRefreshTotal_ = 0;
  digiKeyRefreshCompleted_ = 0;
  digiKeyRefreshSucceeded_ = 0;
  digiKeyRefreshFailed_ = 0;
  digiKeyRefreshChanged_ = false;
  digiKeyRefreshActiveKey_.clear();
  digiKeyRefreshLastError_.clear();
  digiKeyRefreshClient_ = move(api.client);
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    stopDigiKeyRefresh();
    setMessage("DigiKey refresh unavailable while the workspace is changing", 5);
    return;
  }
  digiKeyRefreshGeneration_ = context->generation;
  for (const auto& item : store_.items()) {
    const auto lookup = digiKeyRefreshLookup(item);
    if (!lookup.empty()) {
      digiKeyRefreshQueue_.emplace_back(item.id, lookup);
    }
  }
  digiKeyRefreshTotal_ = digiKeyRefreshQueue_.size();
  if (digiKeyRefreshTotal_ == 0) {
    digiKeyRefreshClient_.reset();
    setMessage("No inventory items with a DigiKey identifier were found", 5);
    return;
  }

  setMessage("Refreshing DigiKey data for " + to_string(digiKeyRefreshTotal_) + " inventory items", 8);
  dirty_ = true;
}

void App::processDigiKeyRefresh() {
  if (digiKeyRefreshFuture_.valid()) {
    if (digiKeyRefreshFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
      return;
    }

    auto result = digiKeyRefreshFuture_.get();
    if (!workspaceIsCurrent(result.workspaceGeneration)) {
      digiKeyRefreshQueue_.clear();
      digiKeyRefreshClient_.reset();
      digiKeyRefreshActiveKey_.clear();
      return;
    }
    digiKeyRefreshActiveKey_.clear();
    ++digiKeyRefreshCompleted_;

    if (result.details) {
      auto* item = store_.findById(result.itemId);
      if (item == nullptr) {
        ++digiKeyRefreshFailed_;
        digiKeyRefreshLastError_ = "An inventory item disappeared during refresh";
      } else {
        if (mergeDigiKeyMetadata(*item, *result.details)) digiKeyRefreshChanged_ = true;
        ++digiKeyRefreshSucceeded_;
      }
    } else {
      ++digiKeyRefreshFailed_;
      digiKeyRefreshLastError_ = result.error.empty() ? "DigiKey returned no product details" : result.error;
    }

    if (digiKeyRefreshQueue_.empty()) {
      digiKeyRefreshClient_.reset();
      if (digiKeyRefreshChanged_ && !saveState("digikey", "inventory refresh", "DigiKey refresh batch")) {
        digiKeyRefreshLastError_ = persistenceError_;
      }
      const auto summary = "DigiKey refresh complete: " + to_string(digiKeyRefreshSucceeded_) + " updated, " +
                           to_string(digiKeyRefreshFailed_) + " failed";
      logActivity("sync", summary);
      setMessage(summary, 8);
      dirty_ = true;
      return;
    }
  }

  if (digiKeyRefreshQueue_.empty() || digiKeyRefreshClient_ == nullptr) {
    return;
  }

  const auto [itemId, lookup] = digiKeyRefreshQueue_.front();
  digiKeyRefreshQueue_.pop_front();
  digiKeyRefreshActiveKey_ = lookup;
  auto* client = digiKeyRefreshClient_.get();
  digiKeyRefreshFuture_ = async(launch::async, [client, itemId, lookup, generation = digiKeyRefreshGeneration_] {
    DigiKeyRefreshResult result;
    result.itemId = itemId;
    result.workspaceGeneration = generation;
    if (const auto details = client->fetchProductDetails(lookup, &result.error); details) {
      result.details = *details;
    }
    return result;
  });
  dirty_ = true;
}

void App::stopDigiKeyRefresh() {
  digiKeyRefreshQueue_.clear();
  digiKeyRefreshActiveKey_.clear();
  if (digiKeyRefreshFuture_.valid()) {
    digiKeyRefreshFuture_.wait();
    digiKeyRefreshFuture_ = {};
  }
  digiKeyRefreshClient_.reset();
  digiKeyRefreshTotal_ = 0;
  digiKeyRefreshCompleted_ = 0;
  digiKeyRefreshSucceeded_ = 0;
  digiKeyRefreshFailed_ = 0;
  digiKeyRefreshChanged_ = false;
  digiKeyRefreshGeneration_ = 0;
  digiKeyRefreshLastError_.clear();
}

DeviceQuantityResult App::enqueueDeviceQuantity(const DeviceQuantityRequest& request) {
  auto pending = make_shared<PendingDeviceQuantity>();
  pending->request = request;
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    DeviceQuantityResult unavailable;
    unavailable.httpStatus = 503;
    unavailable.error = "Inventatory workspace is unavailable";
    return unavailable;
  }
  pending->workspaceGeneration = context->generation;
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
    if (deviceQuantityQueue_.size() >= kDeviceQuantityQueueLimit) {
      DeviceQuantityResult unavailable;
      unavailable.httpStatus = 503;
      unavailable.error = "Inventatory request queue is full";
      return unavailable;
    }
    deviceQuantityQueue_.push_back(pending);
  }
  unique_lock<mutex> lock(pending->mutex);
  if (!pending->ready.wait_for(lock, chrono::seconds(3), [&] { return pending->complete; })) {
    pending->cancelled = true;
    DeviceQuantityResult timeout;
    timeout.httpStatus = 503;
    timeout.error = "Inventatory did not process the request in time";
    return timeout;
  }
  return pending->result;
}

bool App::printWireLabel(const string& text) {
  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("No printer configured", 3);
    openSettings(SettingsCategory::Printer);
    return false;
  }
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Printer unavailable while the workspace is changing", 4);
    return false;
  }
  if (printerWorkCompletion_ != nullptr) {
    setMessage("A printer job is already running; try again shortly", 3);
    return false;
  }
  PrinterWork work;
  work.kind = PrinterWorkKind::PrintWire;
  work.workspaceGeneration = context->generation;
  work.printerName = printerService_.configuredPrinter();
  work.text = text;
  if (!enqueuePrinterWork(move(work))) {
    setMessage("Printer request queue is full; try again shortly", 4);
    return false;
  }
  setMessage("Printer job queued", 3);
  return true;
}

void App::storeQuickLabelPrintResult(const DeviceQuickLabelPrintResult& result) {
  lock_guard<mutex> lock(quickLabelMutex_);
  const bool known = quickLabelPrintResults_.find(result.requestId) != quickLabelPrintResults_.end();
  quickLabelPrintResults_[result.requestId] = result;
  if (!known) quickLabelPrintOrder_.push_back(result.requestId);
  while (quickLabelPrintOrder_.size() > 64) {
    const auto evict = find_if(quickLabelPrintOrder_.begin(), quickLabelPrintOrder_.end(), [&](const string& id) {
      const auto entry = quickLabelPrintResults_.find(id);
      return entry == quickLabelPrintResults_.end() || entry->second.status != "pending";
    });
    if (evict == quickLabelPrintOrder_.end()) break;
    quickLabelPrintResults_.erase(*evict);
    quickLabelPrintOrder_.erase(evict);
  }
}

bool App::printDeviceQuickLabel(const DeviceQuickLabelPrintRequest& request, DeviceQuickLabelPrintResult& result) {
  result.requestId = request.requestId;
  string text;
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    const auto known = quickLabelPrintResults_.find(request.requestId);
    if (known != quickLabelPrintResults_.end() &&
        quickLabelResultMatchesRequest(known->second, request.requestId)) {
      result = known->second;
      return result.status == "completed";
    }
    if (request.revision != settings_.quickLabelRevision) {
      result = {request.requestId, "failed", "stale_presets", "Refresh quick labels"};
    } else if (request.presetIndex < 1 || request.presetIndex > static_cast<int>(settings_.quickLabelPresets.size())) {
      result = {request.requestId, "failed", "missing_preset", "Quick label not found"};
    } else {
      text = settings_.quickLabelPresets[static_cast<size_t>(request.presetIndex - 1)];
    }
  }

  if (!result.status.empty()) {
    storeQuickLabelPrintResult(result);
    return false;
  }

  string printerName;
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    printerName = settings_.printerQueue;
  }
  if (trim(printerName).empty()) {
    result = {request.requestId, "failed", "printer_unconfigured", "No printer configured"};
  } else {
    const auto context = currentWorkspaceContext();
    if (context == nullptr) {
      result = {request.requestId, "failed", "workspace_unavailable", "Workspace is changing"};
    } else {
      result = {request.requestId, "pending", "queued", "Label queued; poll with the same requestId"};
      PrinterWork work;
      work.kind = PrinterWorkKind::PrintWire;
      work.workspaceGeneration = context->generation;
      work.printerName = move(printerName);
      work.text = move(text);
      work.requestId = request.requestId;
      if (!enqueuePrinterWork(move(work))) {
        result = {request.requestId, "failed", "printer_queue_full", "Printer queue is full"};
      }
    }
  }

  storeQuickLabelPrintResult(result);
  return result.status == "completed";
}

void App::addQuickLabelPreset() {
  if (settingsDraft_.quickLabelPresets.size() >= kQuickLabelPresetLimit) {
    setMessage("A maximum of 12 quick labels is supported", 3);
    return;
  }
  settingsDraft_.quickLabelPresets.push_back("New label");
  settingsField_ = static_cast<int>(settingsDraft_.quickLabelPresets.size() - 1);
  settingsDirty_ = true;
  beginSettingsFieldEdit(settingsField_);
}

void App::deleteQuickLabelPreset() {
  if (settingsField_ < 0 || settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) return;
  settingsDraft_.quickLabelPresets.erase(settingsDraft_.quickLabelPresets.begin() + settingsField_);
  if (settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) --settingsField_;
  settingsDirty_ = true;
  dirty_ = true;
}

void App::openStockFilterPanel() {
  if (closestSearchActive_) return;
  inputMode_ = InputMode::StockFilter;
  stockDateFilterSubmenuOpen_ = false;
  stockFilterSelection_ = stockDateFilter_ != StockDateFilter::All ? 0
                          : stockSortOrder_ == StockSortOrder::Quantity ? 1
                          : stockSortOrder_ == StockSortOrder::Za ? 3 : 2;
  focusedTarget_ = -1;
  dirty_ = true;
}

void App::openStockDateFilterSubmenu() {
  stockDateFilterSubmenuOpen_ = true;
  stockFilterSelection_ = static_cast<int>(stockDateFilter_);
  dirty_ = true;
}

void App::applyStockDateFilter(StockDateFilter filter) {
  stockDateFilter_ = filter;
  stockFilterSelection_ = static_cast<int>(filter);
  stockDateFilterSubmenuOpen_ = false;
  inputMode_ = InputMode::None;
  syncSelectionToFilter();
  setMessage("Stock filter: " + stockDateFilterName(filter), 3);
  dirty_ = true;
}

void App::applyStockSortOrder(StockSortOrder order) {
  stockSortOrder_ = order;
  stockDateFilterSubmenuOpen_ = false;
  inputMode_ = InputMode::None;
  syncSelectionToFilter();
  const auto message = order == StockSortOrder::Az ? "Stock sorted A-Z"
                       : order == StockSortOrder::Za ? "Stock sorted Z-A"
                                                     : "Stock sorted by quantity";
  setMessage(message, 3);
  dirty_ = true;
}

void App::handleStockFilterKey(const KeyEvent& key) {
  const int optionCount = stockDateFilterSubmenuOpen_ ? 5 : 4;
  if (key.type == KeyType::Up || (key.type == KeyType::Character && key.ch == 'k')) {
    stockFilterSelection_ = max(0, stockFilterSelection_ - 1);
  } else if (key.type == KeyType::Down || (key.type == KeyType::Character && key.ch == 'j')) {
    stockFilterSelection_ = min(optionCount - 1, stockFilterSelection_ + 1);
  } else if (key.type == KeyType::Enter) {
    if (stockDateFilterSubmenuOpen_) {
      applyStockDateFilter(static_cast<StockDateFilter>(stockFilterSelection_));
    } else if (stockFilterSelection_ == 0) {
      openStockDateFilterSubmenu();
    } else if (stockFilterSelection_ == 1) {
      applyStockSortOrder(StockSortOrder::Quantity);
    } else if (stockFilterSelection_ == 2) {
      applyStockSortOrder(StockSortOrder::Az);
    } else {
      applyStockSortOrder(StockSortOrder::Za);
    }
    return;
  } else if (key.type == KeyType::Escape || (key.type == KeyType::Character && key.ch == 'f')) {
    if (stockDateFilterSubmenuOpen_) {
      stockDateFilterSubmenuOpen_ = false;
      stockFilterSelection_ = 0;
    } else {
      inputMode_ = InputMode::None;
    }
  } else if (key.type == KeyType::Left && stockDateFilterSubmenuOpen_) {
    stockDateFilterSubmenuOpen_ = false;
    stockFilterSelection_ = 0;
  } else if (key.type == KeyType::Character && key.ch >= '1' && key.ch <= '4' && !stockDateFilterSubmenuOpen_) {
    const auto choice = key.ch - '1';
    if (choice == 0) {
      openStockDateFilterSubmenu();
    } else if (choice == 1) {
      applyStockSortOrder(StockSortOrder::Quantity);
    } else if (choice == 2) {
      applyStockSortOrder(StockSortOrder::Az);
    } else {
      applyStockSortOrder(StockSortOrder::Za);
    }
    return;
  } else {
    return;
  }
  dirty_ = true;
}

void App::moveQuickLabelPreset(int direction) {
  const int destination = settingsField_ + direction;
  if (settingsField_ < 0 || destination < 0 || destination >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) return;
  swap(settingsDraft_.quickLabelPresets[settingsField_], settingsDraft_.quickLabelPresets[destination]);
  settingsField_ = destination;
  settingsDirty_ = true;
  dirty_ = true;
}

void App::testQuickLabelPreset() {
  if (settingsField_ < 0 || settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) {
    setMessage("Select a quick label first", 3);
    return;
  }
  if (settingsDraft_.printerQueue.empty()) {
    setMessage("No printer configured", 4);
    return;
  }
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Printer unavailable while the workspace is changing", 4);
    return;
  }
  if (printerWorkCompletion_ != nullptr) {
    setMessage("A printer job is already running; try again shortly", 3);
    return;
  }
  PrinterWork work;
  work.kind = PrinterWorkKind::PrintWire;
  work.workspaceGeneration = context->generation;
  work.printerName = settingsDraft_.printerQueue;
  work.text = settingsDraft_.quickLabelPresets[settingsField_];
  work.successPrefix = "Quick label sent";
  if (enqueuePrinterWork(move(work))) setMessage("Printer job queued", 3);
  else setMessage("Printer request queue is full; try again shortly", 4);
}

void App::enqueueDeviceStatus(const DeviceStatusReport& report, WorkspaceGeneration workspaceGeneration) {
  lock_guard<mutex> lock(deviceQueueMutex_);
  if (deviceStatusQueue_.size() >= kDeviceStatusQueueLimit) {
    deviceStatusQueue_.erase(deviceStatusQueue_.begin());
  }
  deviceStatusQueue_.push_back({report, workspaceGeneration});
}

void App::enqueueDeviceDebug(const DeviceDebugReport& report, WorkspaceGeneration workspaceGeneration) {
  lock_guard<mutex> lock(deviceQueueMutex_);
  if (deviceDebugQueue_.size() >= kDeviceDebugQueueLimit) {
    deviceDebugQueue_.erase(deviceDebugQueue_.begin());
  }
  deviceDebugQueue_.push_back({report, workspaceGeneration});
}

bool App::handleDeviceSync(const DeviceSyncRequest& request, DeviceSyncResponse& response, string& error) {
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    error = "Inventatory workspace is unavailable";
    return false;
  }
  DeviceStatusReport status;
  status.deviceId = request.deviceId;
  status.firmwareVersion = request.firmwareVersion;
  status.rssi = request.rssi;
  status.debug = "protocol=v" + to_string(request.protocolVersion) + " mode=" + request.mode +
                 " queue=" + to_string(request.queueDepth);
  status.protocolVersion = request.protocolVersion;
  status.mode = request.mode;
  status.pendingEventCount = request.queueDepth;
  enqueueDeviceStatus(status, context->generation);
  if (!acceptDeviceSyncEvents(context->paths.inventory, request, response, error)) return false;
  if (request.hasLookup) {
    response.lookupResult = lookupDeviceItem(context->paths.inventory, request.lookup);
    response.hasLookupResult = true;
  }
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    response.hasQuickLabels = true;
    response.quickLabelRevision = settings_.quickLabelRevision;
    response.quickLabelPresets = settings_.quickLabelPresets;
  }
  if (request.hasQuickLabelPrint) {
    response.hasQuickLabelPrintResult = true;
    printDeviceQuickLabel(request.quickLabelPrint, response.quickLabelPrintResult);
  }
  return true;
}

void App::refreshDeviceEventRecords() {
  deviceEventRecords_ = loadDeviceSyncEventRecords(inventoryPath_);
  devicePendingEventCount_ = static_cast<int>(count_if(
      deviceEventRecords_.begin(), deviceEventRecords_.end(), [](const DeviceSyncEventRecord& record) {
        return record.state == "received";
      }));
  dirty_ = true;
}

void App::retryFailedDeviceEvents() {
  size_t retried = 0;
  if (!retryFailedDeviceSyncEvents(inventoryPath_, retried)) {
    setMessage("Unable to reopen failed scanner events", 5);
    return;
  }
  refreshDeviceEventRecords();
  setMessage(retried == 0 ? "No failed scanner events to retry"
                          : "Reopened " + to_string(retried) + " failed scanner event" +
                                (retried == 1 ? string() : string("s")),
             5);
}

void App::discardFailedDeviceEvents() {
  const auto now = time(nullptr);
  if (settingsConfirmAction_ != "discard-device-events" || now > settingsConfirmUntil_) {
    settingsConfirmAction_ = "discard-device-events";
    settingsConfirmUntil_ = now + 5;
    setMessage("Discarding failed scanner events cannot be undone; activate again within 5 seconds to confirm", 5);
    return;
  }
  settingsConfirmAction_.clear();
  settingsConfirmUntil_ = 0;
  size_t discarded = 0;
  if (!discardFailedDeviceSyncEvents(inventoryPath_, discarded)) {
    setMessage("Unable to discard failed scanner events", 5);
    return;
  }
  refreshDeviceEventRecords();
  setMessage(discarded == 0 ? "No failed scanner events to discard"
                            : "Discarded " + to_string(discarded) + " failed scanner event" +
                                  (discarded == 1 ? string() : string("s")),
             5);
}

void App::processDeviceSyncEvents() {
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;
  const auto pending = loadPendingDeviceSyncEvents(context->paths.inventory, 1);
  if (pending.empty()) return;

  const auto& event = pending.front();
  if (event.deviceId.empty()) {
    setMessage("Inventatory Scan event has no durable device identity; it was left pending", 5);
    return;
  }
  auto candidate = store_;
  DeviceSyncResult result;
  result.resultId = event.eventId + "-result";
  result.eventId = event.eventId;
  result.deviceId = event.deviceId;
  result.status = "failed";
  result.code = "invalid_event";
  result.message = "Invalid inventory event";

  string affectedItemId;
  bool created = false;
  if (event.type == "inventory.adjust") {
    DeviceQuantityRequest request{{}, event.eventId, event.code, event.value};
    const auto quantityResult = applyDeviceQuantity(candidate, request);
    result.requestedDelta = event.value;
    result.appliedDelta = quantityResult.appliedDelta;
    result.quantity = quantityResult.quantity;
    result.itemName = quantityResult.item;
    if (quantityResult.ok) {
      result.status = "completed";
      result.code.clear();
      result.message = "Quantity updated";
      if (const auto* item = candidate.findByMachineCode(event.code)) {
        affectedItemId = item->id;
        result.existing = true;
        result.location = rackLocation(*item, candidate.racks());
        if (result.location.empty()) result.location = item->location;
      }
    } else {
      result.code = quantityResult.httpStatus == 404 ? "unknown_item" : "invalid_quantity";
      result.message = quantityResult.error;
    }
  } else if (event.type == "inventory.receive" && event.value > 0) {
    const auto resolution = resolveScanCode(candidate, event.code);
    if (!resolution.matched) {
      result.code = "scan_unresolved";
      result.message = resolution.message;
    } else if (auto* item = candidate.findById(resolution.itemId)) {
      affectedItemId = item->id;
      created = resolution.created;
      result.existing = !created;
      const int oldQuantity = item->quantity;
      const long long requested = static_cast<long long>(oldQuantity) + event.value;
      item->quantity = static_cast<int>(min<long long>(requested, numeric_limits<int>::max()));
      item->lastUpdated = time(nullptr);
      result.requestedDelta = event.value;
      result.appliedDelta = item->quantity - oldQuantity;
      result.quantity = item->quantity;

      string warning;
      const bool shouldEnrich = created || trim(item->syncStatus) != "synced" ||
                                trim(item->partName) == "Scanned DigiKey Item";
      if (shouldEnrich && !trim(event.code).empty()) scanDigiKeyEnrichmentQueue_.emplace_back(item->id, event.code);
      reconcileRackAssignment(candidate, *item);
      result.itemName = item->partName;
      result.location = rackLocation(*item, candidate.racks());
      if (result.location.empty()) result.location = item->location.empty() ? "UNASSIGNED" : item->location;
      result.status = warning.empty() ? "completed" : "completed_with_warning";
      result.code = warning.empty() ? string() : "metadata_sync_failed";
      result.message = warning.empty() ? (created ? "New item received" : "Existing item updated") : warning;
    }
  } else if (event.type == "inventory.receive") {
    result.code = "invalid_quantity";
    result.message = "Received quantity must be positive";
  } else {
    result.code = "unsupported_event";
    result.message = "Unsupported inventory event type";
  }

  if (!workspaceIsCurrent(context->generation)) return;
  if (!completeDeviceSyncEvent(candidate, context->paths.inventory, result, &store_)) {
    setMessage("Inventatory Scan event could not be committed", 4);
    return;
  }
  store_ = move(candidate);
  persistedStore_ = store_;
  persistedStoreValid_ = true;
  refreshInventoryMovements();
  refreshInventoryCommits();
  deviceLastResult_ = result.status == "failed"
                          ? "ERROR " + result.message
                          : (result.existing ? "EXISTING " : "NEW ") + result.itemName + " QTY " +
                                to_string(result.quantity);
  if (result.status == "failed") {
    logActivity("device error", result.message);
  } else {
    logActivity(created ? "scan receive" : "stock receive",
                result.itemName + " changed by " + to_string(result.appliedDelta) + " to " +
                    to_string(result.quantity));
    scannerFlashUntil_ = time(nullptr) + 3;
    if (created) autoPrintScannedLabel(affectedItemId);
  }
  saveActivitiesChecked();
  refreshDeviceEventRecords();
  dirty_ = true;
}

void App::adjustDeviceDebugScroll(int delta) {
  const auto total = deviceDebugLog_.size();
  if (total == 0) {
    deviceDebugScroll_ = 0;
    deviceDebugFollow_ = true;
    return;
  }
  const size_t step = static_cast<size_t>(delta < 0 ? -delta : delta);
  const auto maxScroll = total > kDeviceDebugWindowLines ? total - kDeviceDebugWindowLines : 0;
  if (delta < 0) {
    deviceDebugFollow_ = false;
    deviceDebugScroll_ = min(deviceDebugScroll_ + step, maxScroll);
  } else if (delta > 0) {
    deviceDebugFollow_ = false;
    deviceDebugScroll_ = deviceDebugScroll_ > step ? deviceDebugScroll_ - step : 0;
  }
}

void App::processDeviceRequests() {
  vector<shared_ptr<PendingDeviceQuantity>> quantities;
  vector<QueuedDeviceStatus> statuses;
  vector<QueuedDeviceDebug> debugReports;
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
    quantities.swap(deviceQuantityQueue_);
    statuses.swap(deviceStatusQueue_);
    debugReports.swap(deviceDebugQueue_);
  }

  for (const auto& queuedStatus : statuses) {
    if (!workspaceIsCurrent(queuedStatus.workspaceGeneration)) continue;
    const auto& status = queuedStatus.report;
    deviceLastSeen_ = time(nullptr);
    deviceFirmwareVersion_ = status.firmwareVersion;
    deviceRssi_ = status.rssi;
    deviceDebug_ = status.debug;
    if (status.protocolVersion > 0) {
      deviceProtocolVersion_ = status.protocolVersion;
      deviceMode_ = status.mode;
      devicePendingEventCount_ = status.pendingEventCount;
      deviceLastSync_ = time(nullptr);
    }
    if (trim(inventatoryScanConfig_.deviceId).empty() && !trim(status.deviceId).empty()) {
      inventatoryScanConfig_.deviceId = trim(status.deviceId);
      inventatoryScanConfig_.setupComplete = true;
      saveScannerConfigChecked(true);
      server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                   inventatoryScanReplayStatePath(dataPath_));
    }
    dirty_ = true;
  }

  for (const auto& queuedDebug : debugReports) {
    if (!workspaceIsCurrent(queuedDebug.workspaceGeneration)) continue;
    const auto& debug = queuedDebug.report;
    const auto now = time(nullptr);
    const auto level = trim(debug.level).empty() ? string("info") : trim(debug.level);
    ostringstream out;
    out << nowTimestampString(now) << " [" << level << "] " << debug.message;
    deviceDebugLog_.push_back(out.str());
    if (deviceDebugLog_.size() > 400U) {
      deviceDebugLog_.erase(deviceDebugLog_.begin(), deviceDebugLog_.begin() + 100);
    }
    if (deviceDebugFollow_) {
      deviceDebugScroll_ = deviceDebugLog_.size() > kDeviceDebugWindowLines
                               ? deviceDebugLog_.size() - kDeviceDebugWindowLines
                               : 0;
    }
    dirty_ = true;
  }

  for (const auto& pending : quantities) {
    {
      lock_guard<mutex> pendingLock(pending->mutex);
      if (pending->cancelled || !workspaceIsCurrent(pending->workspaceGeneration)) {
        pending->result = {};
        pending->result.httpStatus = 409;
        pending->result.error = "Inventatory workspace changed before the request was processed";
        pending->complete = true;
        pending->ready.notify_one();
        continue;
      }
    }
    bool pairingChanged = false;
    if (trim(inventatoryScanConfig_.deviceId).empty() && !trim(pending->request.deviceId).empty()) {
      inventatoryScanConfig_.deviceId = trim(pending->request.deviceId);
      inventatoryScanConfig_.setupComplete = true;
      pairingChanged = true;
    }
    const auto before = store_;
    auto result = applyDeviceQuantityCached(store_, pending->request, deviceRequestCache_, deviceRequestOrder_);
    if (pairingChanged) {
      saveScannerConfigChecked(false);
      server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                   inventatoryScanReplayStatePath(dataPath_));
    }
    if (result.ok) {
      logActivity(result.appliedDelta < 0 ? "usage scan" : "stock scan",
                  result.item + " quantity changed by " + to_string(result.appliedDelta) +
                      " to " + to_string(result.quantity));
      if (!saveState("scanner", pending->request.requestId)) {
        store_ = before;
        deviceRequestCache_.erase(pending->request.requestId);
        deviceRequestOrder_.erase(
            remove(deviceRequestOrder_.begin(), deviceRequestOrder_.end(), pending->request.requestId),
            deviceRequestOrder_.end());
        result.ok = false;
        result.httpStatus = 503;
        result.error = persistenceError_.empty() ? "Inventatory could not persist the scanner update" : persistenceError_;
        deviceLastResult_ = "ERROR " + result.error;
      } else {
        scannerFlashUntil_ = time(nullptr) + 3;
        deviceLastResult_ = (result.appliedDelta >= 0 ? "+" : "") + to_string(result.appliedDelta) +
                            " " + result.item + " QTY " + to_string(result.quantity);
      }
    } else {
      deviceLastResult_ = "ERROR " + result.error;
    }
    deviceLastSeen_ = time(nullptr);
    dirty_ = true;
    {
      lock_guard<mutex> lock(pending->mutex);
      pending->result = result;
      pending->complete = true;
    }
    pending->ready.notify_one();
  }
}

void App::updateDashboardScannerState() {
  ScannerDashboardState next = ScannerDashboardState::Unpaired;
  if (!trim(inventatoryScanConfig_.token).empty()) {
    if (trim(inventatoryScanConfig_.deviceId).empty()) {
      next = ScannerDashboardState::Waiting;
    } else {
      const auto now = time(nullptr);
      next = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15
                 ? ScannerDashboardState::Online
                 : ScannerDashboardState::Offline;
    }
  }

  if (next == scannerDashboardState_) return;
  const auto previous = scannerDashboardState_;
  scannerDashboardState_ = next;
  const bool collapsing = previous == ScannerDashboardState::Online && next == ScannerDashboardState::Offline;
  const bool expanding = previous == ScannerDashboardState::Offline && next == ScannerDashboardState::Online;
  scannerDashboardTransitionStartedAt_ = collapsing || expanding ? uiAnimationTicks() : -1;
  scannerDashboardTransitionExpanding_ = expanding;
  dirty_ = true;
}

string App::inventatoryScanDeviceSummary() const {
  if (trim(inventatoryScanConfig_.token).empty()) return "R1 UNPAIRED";
  if (trim(inventatoryScanConfig_.deviceId).empty()) return "R1 WAITING FOR DEVICE";
  if (deviceLastSeen_ == 0 || time(nullptr) - deviceLastSeen_ > 15) return "R1 OFFLINE";
  if (!deviceLastResult_.empty()) return "R1 ONLINE  " + deviceLastResult_;
  return "R1 ONLINE  RSSI " + to_string(deviceRssi_);
}

void App::beginCsvImport() {
  filesystem::path selectedPath;
  if (!openCsvFileDialog(selectedPath)) {
    setMessage("CSV import cancelled", 2);
    return;
  }

  ifstream input(selectedPath, ios::binary);
  if (!input) {
    setMessage("Unable to open " + selectedPath.filename().string(), 6);
    return;
  }
  error_code fileError;
  if (const auto size = filesystem::file_size(selectedPath, fileError); fileError || size > kMaximumImportBytes) {
    setMessage("CSV import exceeds the 25 MiB safety limit", 6);
    return;
  }
  ostringstream buffer;
  buffer << input.rdbuf();
  const auto text = buffer.str();

  // The file picker is the only way in, so detection replaces asking the user
  // which dialect they just chose.
  if (detectCsvFormat(text) == CsvFormat::KicadBom) {
    beginBomProject(text, projectNameFromPath(selectedPath), selectedPath);
    return;
  }

  const auto result = parseDigiKeyCsvText(text, store_.items());
  if (!result.ok) {
    setMessage("CSV import failed: " + result.error, 6);
    return;
  }

  importCandidates_ = result.candidates;
  importOriginalStore_ = store_;
  importStagedStore_ = store_;
  importStageActive_ = true;
  importCommitPending_ = false;
  importAcceptedItemIds_.clear();
  importSourcePath_ = selectedPath;
  importSelection_ = 0;
  importSyncPrompt_ = false;
  importCreatedCount_ = 0;
  importMergedCount_ = 0;
  importSkippedCount_ = 0;
  importSyncedCount_ = 0;
  importSyncFailedCount_ = 0;
  importSyncFailedItemIds_.clear();
  importSyncTotal_ = 0;
  importSyncCompleted_ = 0;
  importSyncRunning_ = false;
  importSyncHasRun_ = false;
  importSyncCancelRequested_ = false;
  editingImportCandidate_ = false;
  inputMode_ = InputMode::None;
  page_ = Page::Import;

  string summary = "Loaded " + to_string(importCandidates_.size()) + " CSV rows for review";
  if (!result.warnings.empty()) {
    summary += " · " + to_string(result.warnings.size()) + " rows skipped";
  }
  setMessage(summary, 4);
}

void App::cancelImportSession() {
  if (!importStageActive_ && !importCommitPending_) return;
  if (importCommitPending_) {
    // The staged copy was assigned to the live view only to let the existing
    // save-retry path retry the exact same operation. Restore the pre-import
    // view when the user explicitly cancels that retry.
    store_ = importOriginalStore_;
  }
  importCandidates_.clear();
  importAcceptedItemIds_.clear();
  importSourcePath_.clear();
  importOriginalStore_ = {};
  importStagedStore_ = {};
  importStageActive_ = false;
  importCommitPending_ = false;
  importSelection_ = 0;
  importSyncPrompt_ = false;
  importSyncHasRun_ = false;
  importSyncFailedItemIds_.clear();
  editingImportCandidate_ = false;
  inputMode_ = InputMode::None;
  dirty_ = true;
}

bool App::commitImportStage() {
  if (!importStageActive_) return true;
  store_ = importStagedStore_;
  if (!saveState("import", importSourcePath_.filename().string())) {
    importCommitPending_ = true;
    setMessage("Import is staged but not saved. Press R to retry or Q to cancel.", 7);
    dirty_ = true;
    return false;
  }
  importStageActive_ = false;
  importCommitPending_ = false;
  return true;
}

CsvImportCandidate* App::currentImportCandidate() {
  if (importCandidates_.empty()) {
    return nullptr;
  }
  importSelection_ = min(importSelection_, importCandidates_.size() - 1);
  return &importCandidates_[importSelection_];
}

const CsvImportCandidate* App::currentImportCandidate() const {
  if (importCandidates_.empty()) {
    return nullptr;
  }
  const auto index = min(importSelection_, importCandidates_.size() - 1);
  return &importCandidates_[index];
}

void App::moveImportSelection(int delta) {
  if (importCandidates_.empty()) {
    importSelection_ = 0;
    dirty_ = true;
    return;
  }

  const auto current = static_cast<int>(min(importSelection_, importCandidates_.size() - 1));
  importSelection_ = static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(importCandidates_.size() - 1)));
  dirty_ = true;
}

void App::acceptImportCandidate() {
  auto* candidate = currentImportCandidate();
  if (candidate == nullptr) {
    finishImportReview();
    return;
  }

  string acceptedId;
  if (candidate->hasConflict) {
      auto* existing = importStagedStore_.findById(candidate->existingItemId);
    if (existing != nullptr) {
      captureUndoSnapshot();
      existing->quantity = candidate->item.quantity > 0 &&
                                  existing->quantity > numeric_limits<int>::max() - candidate->item.quantity
                              ? numeric_limits<int>::max()
                              : max(0, existing->quantity + candidate->item.quantity);
      existing->lastUpdated = time(nullptr);
      mergeImportedMetadata(*existing, candidate->item);
      if (candidate->item.rackAssignment != RackAssignmentMode::Automatic) {
        existing->rackId = candidate->item.rackId;
        existing->rackSlot = candidate->item.rackSlot;
        existing->rackAssignment = candidate->item.rackAssignment;
      }
      reconcileRackAssignment(importStagedStore_, *existing);
      acceptedId = existing->id;
      ++importMergedCount_;
    }
  }

  if (acceptedId.empty()) {
    captureUndoSnapshot();
    importStagedStore_.items().push_back(candidate->item);
    reconcileRackAssignment(importStagedStore_, importStagedStore_.items().back());
    acceptedId = importStagedStore_.items().back().id;
    ++importCreatedCount_;
  }

  importAcceptedItemIds_.push_back(acceptedId);
  importCandidates_.erase(importCandidates_.begin() + static_cast<ptrdiff_t>(importSelection_));
  if (importSelection_ >= importCandidates_.size() && !importCandidates_.empty()) {
    importSelection_ = importCandidates_.size() - 1;
  }

  if (importCandidates_.empty()) {
    finishImportReview();
  } else {
    setMessage("Accepted import row", 2);
  }
  dirty_ = true;
}

void App::skipImportCandidate() {
  if (importCandidates_.empty()) {
    finishImportReview();
    return;
  }

  importCandidates_.erase(importCandidates_.begin() + static_cast<ptrdiff_t>(importSelection_));
  ++importSkippedCount_;
  if (importSelection_ >= importCandidates_.size() && !importCandidates_.empty()) {
    importSelection_ = importCandidates_.size() - 1;
  }

  if (importCandidates_.empty()) {
    finishImportReview();
  } else {
    setMessage("Skipped import row", 2);
  }
  dirty_ = true;
}

void App::finishImportReview() {
  if (importStageActive_ && !commitImportStage()) return;
  if (importCommitPending_) return;
  if (importAcceptedItemIds_.empty()) {
    finishCsvImport(false);
    return;
  }
  importSyncPrompt_ = !importAcceptedItemIds_.empty();
  page_ = Page::Import;
  inputMode_ = InputMode::None;
  dirty_ = true;
}

void App::beginImportSync(bool retryFailed) {
  if (importSyncRunning_) {
    setMessage("DigiKey import sync is already running", 3);
    return;
  }

  const auto itemIds = retryFailed ? importSyncFailedItemIds_ : importAcceptedItemIds_;
  if (itemIds.empty()) {
    setMessage(retryFailed ? "There are no failed DigiKey rows to retry" : "No accepted rows need DigiKey sync", 3);
    return;
  }

  const auto api = createDigiKeyApi();
  importSyncTotal_ = itemIds.size();
  importSyncCompleted_ = 0;
  importSyncFailedCount_ = 0;
  importSyncFailedItemIds_.clear();
  importSyncCancelRequested_ = false;
  importSyncHasRun_ = false;

  vector<pair<string, string>> requests;
  for (const auto& itemId : itemIds) {
    const auto* item = store_.findById(itemId);
    if (item == nullptr) {
      importSyncFailedItemIds_.push_back(itemId);
      ++importSyncFailedCount_;
      continue;
    }
    const auto lookup = !trim(item->digikeyPartNumber).empty() ? item->digikeyPartNumber : item->sku;
    if (trim(lookup).empty()) {
      importSyncFailedItemIds_.push_back(itemId);
      ++importSyncFailedCount_;
      continue;
    }
    requests.emplace_back(itemId, lookup);
  }

  if (api.client == nullptr) {
    importSyncCompleted_ = importSyncTotal_;
    importSyncHasRun_ = true;
    importSyncPrompt_ = true;
    setMessage("DigiKey sync unavailable: " + api.error + "; press R to retry after configuring it", 6);
    return;
  }

  if (requests.empty()) {
    importSyncCompleted_ = importSyncTotal_;
    importSyncHasRun_ = true;
    importSyncPrompt_ = true;
    setMessage("No accepted rows had a DigiKey identifier; press Enter to finish", 5);
    return;
  }

  const auto config = loadDigiKeyConfig();
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("DigiKey sync unavailable while the workspace is changing", 5);
    return;
  }
  importSyncGeneration_ = context->generation;
  importSyncCancelFlag_ = make_shared<atomic<bool>>(false);
  const auto cancelFlag = importSyncCancelFlag_;
  const auto generation = importSyncGeneration_;
  importSyncFuture_ = async(launch::async, [requests = move(requests), config, cancelFlag, generation] {
    ImportSyncBatchResult batch;
    batch.workspaceGeneration = generation;
    DigiKeyApiClient client(config);
    for (size_t index = 0; index < requests.size(); ++index) {
      if (cancelFlag->load()) {
        for (size_t remaining = index; remaining < requests.size(); ++remaining) {
          batch.failedItemIds.push_back(requests[remaining].first);
        }
        break;
      }
      string error;
      auto details = client.fetchProductDetails(requests[index].second, &error);
      if (details) {
        batch.results.emplace_back(requests[index].first, move(details));
      } else {
        batch.failedItemIds.push_back(requests[index].first);
      }
    }
    return batch;
  });
  importSyncRunning_ = true;
  importSyncPrompt_ = false;
  setMessage("DigiKey sync is running in the background; the terminal remains available", 5);
  dirty_ = true;
}

void App::processImportSync() {
  if (!importSyncFuture_.valid() || importSyncFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;

  const auto batch = importSyncFuture_.get();
  if (!workspaceIsCurrent(batch.workspaceGeneration)) {
    importSyncRunning_ = false;
    importSyncHasRun_ = false;
    importSyncCancelFlag_.reset();
    importSyncPrompt_ = false;
    importSyncFailedItemIds_.clear();
    setMessage("DigiKey sync result discarded because the workspace changed", 5);
    dirty_ = true;
    return;
  }
  const bool cancelled = importSyncCancelRequested_;
  importSyncCompleted_ = min(importSyncTotal_, batch.results.size() + batch.failedItemIds.size() +
                                             importSyncFailedItemIds_.size());
  if (cancelled) {
    for (const auto& result : batch.results) importSyncFailedItemIds_.push_back(result.first);
    importSyncFailedItemIds_.insert(importSyncFailedItemIds_.end(), batch.failedItemIds.begin(), batch.failedItemIds.end());
    importSyncFailedCount_ += static_cast<int>(batch.results.size() + batch.failedItemIds.size());
  } else {
    for (const auto& result : batch.results) {
      if (auto* item = store_.findById(result.first); item != nullptr && result.second.has_value()) {
        mergeDigiKeyMetadata(*item, *result.second);
        reconcileRackAssignment(store_, *item);
        ++importSyncedCount_;
      } else {
        importSyncFailedItemIds_.push_back(result.first);
        ++importSyncFailedCount_;
      }
    }
    importSyncFailedItemIds_.insert(importSyncFailedItemIds_.end(), batch.failedItemIds.begin(), batch.failedItemIds.end());
    importSyncFailedCount_ += static_cast<int>(batch.failedItemIds.size());
  }

  importSyncRunning_ = false;
  importSyncHasRun_ = true;
  importSyncCancelFlag_.reset();
  if (!cancelled && !batch.results.empty() && !saveState("digikey", "import sync", "DigiKey enrichment batch")) {
    setMessage("DigiKey metadata is in memory; press R to retry saving", 6);
  } else {
    setMessage(cancelled ? "DigiKey sync cancelled; press R to retry failed rows"
                         : "DigiKey sync finished; press R to retry failed rows or Enter to finish",
               6);
  }
  importSyncPrompt_ = true;
  dirty_ = true;
}

void App::retryImportSync() {
  if (importSyncFailedItemIds_.empty()) {
    setMessage("There are no failed DigiKey rows to retry", 3);
    return;
  }
  beginImportSync(true);
}

void App::finishCsvImport(bool syncWithDigiKey) {
  if (syncWithDigiKey) {
    beginImportSync();
    return;
  }
  if (importSyncRunning_) {
    setMessage("Wait for DigiKey sync to finish or cancel it first", 4);
    return;
  }

  const auto summary = importCompletionMessage();
  logActivity("import", summary);
  importCandidates_.clear();
  importAcceptedItemIds_.clear();
  importSourcePath_.clear();
  importSelection_ = 0;
  importSyncPrompt_ = false;
  importSyncHasRun_ = false;
  importSyncFailedItemIds_.clear();
  importSyncTotal_ = 0;
  importSyncCompleted_ = 0;
  importSyncCancelRequested_ = false;
  editingImportCandidate_ = false;
  importStageActive_ = false;
  importCommitPending_ = false;
  importOriginalStore_ = {};
  importStagedStore_ = {};
  changePage(Page::Home);
  setMessage(summary, 8);
}

string App::importCompletionMessage() const {
  return "CSV import complete: " + to_string(importCreatedCount_) + " new, " +
         to_string(importMergedCount_) + " merged, " + to_string(importSkippedCount_) + " skipped, " +
         to_string(importSyncedCount_) + " synced, " + to_string(importSyncFailedCount_) + " sync failed";
}

// ---------------------------------------------------------------------------
// KiCad BOM projects
// ---------------------------------------------------------------------------

BomProject* App::activeBomProject() {
  if (activeBomProjectId_.empty()) {
    return nullptr;
  }
  const auto it = find_if(bomProjects_.begin(), bomProjects_.end(),
                          [&](const BomProject& project) { return project.id == activeBomProjectId_; });
  return it == bomProjects_.end() ? nullptr : &*it;
}

const BomProject* App::activeBomProject() const {
  return const_cast<App*>(this)->activeBomProject();
}

bool App::saveBomProjects() {
  if (inventoryRecoveryRequired_) {
    persistenceError_ = inventoryRecoveryDetail_.empty()
                            ? "Inventory recovery is required before BOM projects can be saved."
                            : inventoryRecoveryDetail_ + ". BOM projects were not changed.";
    return false;
  }
  const bool saved = inventatory::saveBomProjects(inventoryPath_, bomProjects_);
  if (!saved) {
    bomProjectsDirty_ = true;
    persistenceError_ = "Could not save BOM projects; changes remain in memory.";
    setMessage(persistenceError_ + " Press R to retry.", 6);
  } else {
    bomProjectsDirty_ = false;
  }
  return saved;
}

void App::openBomProjects() {
  bomView_ = BomView::List;
  bomDeductPrompt_ = false;
  bomProjectSelection_ = bomProjects_.empty() ? 0 : min(bomProjectSelection_, bomProjects_.size() - 1);
  changePage(Page::Projects);
}

void App::beginBomProject(const string& bomText, const string& name, const filesystem::path& sourcePath) {
  bomFile_ = parseKicadBomText(bomText, name);
  if (!bomFile_.ok) {
    setMessage("BOM import failed: " + bomFile_.error, 6);
    return;
  }

  // A freshly imported project is held in memory until the user pins it, so a
  // one-off analysis never clutters the list.
  BomProject project;
  project.id = "bom-" + makeId().substr(0, 12);
  project.name = name;
  project.sourcePath = sourcePath.string();
  project.boards = 1;
  project.createdAt = time(nullptr);
  project.lastOpened = project.createdAt;
  project.bomText = bomText;

  // Replace an existing project built from the same file rather than stacking
  // duplicates every time the user re-imports after a schematic change.
  const auto existing = find_if(bomProjects_.begin(), bomProjects_.end(), [&](const BomProject& candidate) {
    return !candidate.sourcePath.empty() && candidate.sourcePath == project.sourcePath;
  });
  if (existing != bomProjects_.end()) {
    project.id = existing->id;
    project.boards = existing->boards;
    project.createdAt = existing->createdAt;
    project.lastBuilt = existing->lastBuilt;
    project.overrides = existing->overrides;
    project.enrichment = existing->enrichment;
    *existing = project;
  } else {
    bomProjects_.insert(bomProjects_.begin(), project);
  }

  activeBomProjectId_ = project.id;
  bomProjectSelection_ = 0;
  bomSplitSelection_ = 0;
  bomSplitShortFocused_ = false;
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomView_ = BomView::Split;
  inputMode_ = InputMode::None;
  refreshBomAnalysis();
  queueBomEnrichment();
  // Persist straight away: an imported BOM should survive a restart without the
  // user having to ask for it.
  const bool projectSaved = saveBomProjects();
  changePage(Page::Projects);

  string summary = to_string(bomAnalysis_.readyCount) + " ready · " + to_string(bomAnalysis_.shortCount) + " short";
  if (!projectSaved) summary += " · project changes unsaved; press R to retry";
  if (!bomFile_.warnings.empty()) {
    summary += " · " + to_string(bomFile_.warnings.size()) + " rows not orderable";
  }
  setMessage(summary, 6);
}

void App::refreshBomAnalysis() {
  auto* project = activeBomProject();
  if (project == nullptr || !bomFile_.ok) {
    bomAnalysisValid_ = false;
    return;
  }

  bomAnalysis_ = analyzeBom(bomFile_, store_.items(), project->boards, project->overrides);
  bomAnalysisValid_ = true;
  if (!bomAnalysis_.matches.empty()) {
    bomSplitSelection_ = min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
  } else {
    bomSplitSelection_ = 0;
  }
  dirty_ = true;
}

void App::openSelectedBomProject() {
  if (bomProjects_.empty()) {
    return;
  }
  auto& project = bomProjects_[min(bomProjectSelection_, bomProjects_.size() - 1)];
  bomFile_ = parseKicadBomText(project.bomText, project.name);
  if (!bomFile_.ok) {
    setMessage("Stored BOM could not be read: " + bomFile_.error, 6);
    return;
  }

  project.lastOpened = time(nullptr);
  activeBomProjectId_ = project.id;
  bomSplitSelection_ = 0;
  bomSplitShortFocused_ = false;
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomView_ = BomView::Split;
  refreshBomAnalysis();
  queueBomEnrichment();
  saveBomProjects();
}

void App::moveBomSelection(int delta) {
  if (bomView_ == BomView::List) {
    if (bomProjects_.empty()) {
      bomProjectSelection_ = 0;
      dirty_ = true;
      return;
    }
    const auto current = static_cast<int>(min(bomProjectSelection_, bomProjects_.size() - 1));
    bomProjectSelection_ =
        static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(bomProjects_.size() - 1)));
  } else if (bomView_ == BomView::Split) {
    if (!bomAnalysisValid_ || bomAnalysis_.matches.empty()) {
      bomSplitSelection_ = 0;
      dirty_ = true;
      return;
    }
    vector<size_t> visibleIndices;
    for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
      if (bomAnalysis_.matches[index].sufficient != bomSplitShortFocused_) {
        visibleIndices.push_back(index);
      }
    }
    if (visibleIndices.empty()) {
      dirty_ = true;
      return;
    }
    const auto selected = find(visibleIndices.begin(), visibleIndices.end(), bomSplitSelection_);
    const int current = selected == visibleIndices.end() ? 0 : static_cast<int>(distance(visibleIndices.begin(), selected));
    const auto next = clamp(current + delta, 0, static_cast<int>(visibleIndices.size() - 1));
    bomSplitSelection_ = visibleIndices[static_cast<size_t>(next)];
  }
  dirty_ = true;
}

void App::adjustBomBoards(int delta) {
  auto* project = activeBomProject();
  if (project == nullptr) {
    return;
  }
  const auto boards = clamp(project->boards + delta, 1, 999);
  if (boards == project->boards) {
    return;
  }
  project->boards = boards;
  refreshBomAnalysis();
  const bool saved = saveBomProjects();
  setMessage(saved ? to_string(boards) + (boards == 1 ? " board" : " boards")
                  : "Board count changed in memory; press R to retry saving the project",
             saved ? 2 : 5);
}

void App::cycleBomAlternate() {
  auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_ || bomAnalysis_.matches.empty()) {
    return;
  }

  auto& match = bomAnalysis_.matches[min(bomSplitSelection_, bomAnalysis_.matches.size() - 1)];
  if (match.candidates.size() < 2) {
    setMessage("No alternate match for this line", 2);
    return;
  }

  match.chosen = (match.chosen + 1) % match.candidates.size();
  project->overrides[bomLineKey(bomAnalysis_.lines[match.lineIndex])] = match.chosenItemId();
  recomputeBomTotals(bomAnalysis_, store_.items());
  const bool saved = saveBomProjects();

  const auto* item = store_.findById(match.chosenItemId());
  setMessage(saved ? "Matched to " + (item == nullptr ? string("unknown part") : item->partName) + "  (" +
                         to_string(match.chosen + 1) + "/" + to_string(match.candidates.size()) + ")"
                  : "Alternate match changed in memory; press R to retry saving the project",
             5);
}

void App::deleteSelectedBomProject() {
  if (bomProjects_.empty()) {
    return;
  }
  const auto index = min(bomProjectSelection_, bomProjects_.size() - 1);
  const auto selectedId = bomProjects_[index].id;
  const auto now = time(nullptr);
  if (bomDeleteConfirmationProjectId_ != selectedId || now > bomDeleteConfirmationUntil_) {
    bomDeleteConfirmationProjectId_ = selectedId;
    bomDeleteConfirmationUntil_ = now + 5;
    setMessage("Press d again within 5 seconds to forget " + bomProjects_[index].name, 5);
    dirty_ = true;
    return;
  }
  bomDeleteConfirmationProjectId_.clear();
  bomDeleteConfirmationUntil_ = 0;
  const auto name = bomProjects_[index].name;
  if (bomProjects_[index].id == activeBomProjectId_) {
    activeBomProjectId_.clear();
    bomAnalysisValid_ = false;
  }
  bomProjects_.erase(bomProjects_.begin() + static_cast<long>(index));
  bomProjectSelection_ = bomProjects_.empty() ? 0 : min(index, bomProjects_.size() - 1);
  if (!saveBomProjects()) {
    setMessage("Could not save project deletion; press R to retry", 5);
    return;
  }
  setMessage(name + " forgotten", 3);
}

void App::beginBomRestock() {
  if (!bomAnalysisValid_ || bomAnalysis_.matches.empty()) return;
  const auto index = min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
  const auto& match = bomAnalysis_.matches[index];
  if (match.sufficient) {
    setMessage("That BOM line is already covered", 2);
    return;
  }
  const auto itemId = match.chosenItemId();
  if (itemId.empty() || store_.findById(itemId) == nullptr) {
    setMessage("This line has no matched stock item; add it manually from Stock", 5);
    changePage(Page::Stock);
    beginEditCurrentItem(true);
    return;
  }
  bomRestockItemId_ = itemId;
  inputBuffer_ = to_string(max(1, match.needed - match.available));
  inputMode_ = InputMode::BomRestock;
  setMessage("Enter received quantity; default is the shortage", 4);
  dirty_ = true;
}

vector<App::BuildStep> App::bomBuildSteps() const {
  vector<BuildStep> steps;
  if (!bomAnalysisValid_) {
    return steps;
  }

  // Rack stops come first in rack order so the shelf is walked once, then a
  // single stop collects everything that lives outside a rack.
  map<string, BuildStep> rackSteps;
  BuildStep loose;
  loose.title = "NOT IN A RACK";

  for (const auto& match : bomAnalysis_.matches) {
    const auto itemId = match.chosenItemId();
    if (itemId.empty()) {
      const auto& line = bomAnalysis_.lines[match.lineIndex];
      BuildPick pick;
      pick.label = line.designation + " (add in Stock)";
      pick.detail = packageFromFootprint(line.footprint);
      pick.slot = "Add in Stock";
      pick.quantity = match.needed;
      loose.picks.push_back(move(pick));
      continue;
    }
    const auto* item = store_.findById(itemId);
    if (item == nullptr) {
      continue;
    }

    const auto& line = bomAnalysis_.lines[match.lineIndex];
    BuildPick pick;
    pick.itemId = itemId;
    pick.slot = item->rackSlot;
    pick.label = line.designation;
    pick.detail = packageFromFootprint(line.footprint);
    pick.quantity = match.needed;

    if (item->rackId.empty() || item->rackSlot.empty()) {
      pick.slot = item->location.empty() ? "Unfiled" : item->location;
      loose.picks.push_back(move(pick));
      continue;
    }

    auto& step = rackSteps[item->rackId];
    step.rackId = item->rackId;
    step.picks.push_back(move(pick));
  }

  vector<const InventatoryRack*> orderedRacks;
  for (const auto& rack : store_.racks()) {
    if (rackSteps.count(rack.id) != 0) {
      orderedRacks.push_back(&rack);
    }
  }
  sort(orderedRacks.begin(), orderedRacks.end(), [](const InventatoryRack* lhs, const InventatoryRack* rhs) {
    const auto left = rackNumberFromCode(lhs->code);
    const auto right = rackNumberFromCode(rhs->code);
    return left != right ? left < right : lhs->code < rhs->code;
  });

  for (const auto* rack : orderedRacks) {
    auto step = rackSteps[rack->id];
    step.title = "RACK " + to_string(max(1, rackNumberFromCode(rack->code)));
    step.subtitle = rack->componentType;
    sort(step.picks.begin(), step.picks.end(),
         [](const BuildPick& lhs, const BuildPick& rhs) { return lhs.slot < rhs.slot; });
    steps.push_back(move(step));
  }

  if (!loose.picks.empty()) {
    sort(loose.picks.begin(), loose.picks.end(),
         [](const BuildPick& lhs, const BuildPick& rhs) { return lhs.slot < rhs.slot; });
    steps.push_back(move(loose));
  }
  return steps;
}

void App::beginBomBuild() {
  if (!bomAnalysisValid_) {
    return;
  }
  if (bomBuildSteps().empty()) {
    setMessage("Nothing to pick: no BOM line matched a part in stock", 4);
    return;
  }
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomView_ = BomView::Build;
  dirty_ = true;
}

void App::advanceBomBuild(int delta) {
  const auto steps = bomBuildSteps();
  if (steps.empty()) {
    bomView_ = BomView::Split;
    dirty_ = true;
    return;
  }

  const auto next = static_cast<int>(bomBuildStep_) + delta;
  if (next < 0) {
    bomView_ = BomView::Split;
    bomBuildStep_ = 0;
    dirty_ = true;
    return;
  }
  if (next >= static_cast<int>(steps.size())) {
    if (!bomBuildReady(bomAnalysis_)) {
      setMessage("Build walkthrough viewed; completion is disabled while BOM shortages remain", 6);
      bomBuildStep_ = steps.size() - 1;
      dirty_ = true;
      return;
    }
    // Past the last stop the walkthrough asks its one and only question.
    bomDeductPrompt_ = true;
    dirty_ = true;
    return;
  }

  bomBuildStep_ = static_cast<size_t>(next);
  dirty_ = true;
}

void App::finishBomBuild(bool subtractFromStock) {
  if (!bomBuildReady(bomAnalysis_)) {
    bomDeductPrompt_ = false;
    setMessage("Build cannot be completed until every BOM line has enough stock", 6);
    return;
  }
  const auto steps = bomBuildSteps();
  int parts = 0;
  int pieces = 0;

  if (subtractFromStock) {
    captureUndoSnapshot();
    const auto now = time(nullptr);
    for (const auto& step : steps) {
      for (const auto& pick : step.picks) {
        auto* item = store_.findById(pick.itemId);
        if (item == nullptr) {
          continue;
        }
        item->quantity = max(0, item->quantity - pick.quantity);
        item->lastUpdated = now;
        reconcileRackAssignment(store_, *item);
        ++parts;
        pieces += pick.quantity;
      }
    }
    if (!saveState("bom_build", activeBomProjectId_)) {
      setMessage("Build stock changes are in memory; press R to retry saving", 6);
      return;
    }
  }

  auto* project = activeBomProject();
  const auto previousLastBuilt = project == nullptr ? time_t(0) : project->lastBuilt;
  if (project != nullptr) {
    project->lastBuilt = time(nullptr);
    if (!saveBomProjects()) {
      project->lastBuilt = previousLastBuilt;
      bomDeductPrompt_ = false;
      setMessage("Build stock was saved, but the project timestamp was not; press R to retry", 7);
      return;
    }
  }

  const auto name = project == nullptr ? string("Project") : project->name;
  if (subtractFromStock) {
    logActivity("build", name + " · " + to_string(parts) + " parts · " + to_string(pieces) + " pieces · " +
                             to_string(bomAnalysis_.boards) + " boards");
  }

  bomDeductPrompt_ = false;
  bomBuildStep_ = 0;
  bomView_ = BomView::Split;
  refreshBomAnalysis();
  setMessage(subtractFromStock ? "Build complete · stock updated · Ctrl+Z undoes it"
                               : "Build complete · stock unchanged",
             6);
}

bool App::exportBomShortages() {
  if (!bomAnalysisValid_) {
    return false;
  }
  const auto* project = activeBomProject();
  if (project == nullptr) {
    return false;
  }

  filesystem::path target = project->sourcePath.empty()
                                ? dataPath_ / (project->name + " shortage.csv")
                                : filesystem::path(project->sourcePath).parent_path() /
                                      (filesystem::path(project->sourcePath).stem().string() + "-shortage.csv");

  ofstream output(target, ios::binary);
  if (!output) {
    setMessage("Unable to write " + target.filename().string(), 5);
    return false;
  }

  const auto quote = [](const string& value) {
    string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
      if (ch == '"') {
        escaped.push_back('"');
      }
      escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
  };

  output << "Designation,Footprint,Package,Designators,Needed,On hand,Suggested DigiKey part\r\n";
  size_t rows = 0;
  for (const auto& match : bomAnalysis_.matches) {
    if (match.sufficient) {
      continue;
    }
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const auto suggestion = project->enrichment.find(bomLineKey(line));
    output << quote(line.designation) << ',' << quote(line.footprint) << ','
           << quote(packageFromFootprint(line.footprint)) << ',' << quote(join(line.designators, ' ')) << ','
           << match.needed << ',' << match.available << ','
           << quote(suggestion == project->enrichment.end() ? string() : suggestion->second) << "\r\n";
    ++rows;
  }

  setMessage("Wrote " + to_string(rows) + " shortages to " + target.filename().string(), 6);
  return true;
}

void App::queueBomEnrichment() {
  bomEnrichmentQueue_.clear();
  bomEnrichmentTotal_ = 0;
  const auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_) {
    bomEnrichmentProjectId_.clear();
    ++bomEnrichmentSequence_;
    if (!bomEnrichmentFuture_.valid()) {
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
      bomEnrichmentClient_.reset();
    }
    return;
  }
  bomEnrichmentProjectId_ = project->id;
  // A new queue run invalidates any result still in flight.  The old future is
  // allowed to finish against its captured client, then its project/sequence
  // pair is checked before anything is applied or saved.
  ++bomEnrichmentSequence_;
  // Silently skipped without credentials, so an offline user never sees an
  // error they cannot act on.
  if (!loadDigiKeyConfig().valid()) {
    return;
  }
  if (const auto context = currentWorkspaceContext(); context != nullptr) {
    bomEnrichmentGeneration_ = context->generation;
  } else {
    return;
  }
  for (const auto& match : bomAnalysis_.matches) {
    if (match.sufficient) {
      continue;
    }
    const auto key = bomLineKey(bomAnalysis_.lines[match.lineIndex]);
    if (project->enrichment.count(key) != 0) {
      continue;  // cached with the pinned project
    }
    if (find(bomEnrichmentQueue_.begin(), bomEnrichmentQueue_.end(), key) == bomEnrichmentQueue_.end()) {
      bomEnrichmentQueue_.push_back(key);
    }
  }
  bomEnrichmentTotal_ = bomEnrichmentQueue_.size();
}

void App::processBomEnrichment() {
  // Collect a finished lookup first, then start the next one. Only ever one
  // request is outstanding, so the shared client's cached token is safe.
  if (bomEnrichmentFuture_.valid()) {
    if (bomEnrichmentFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
      return;
    }
    const auto result = bomEnrichmentFuture_.get();
    const auto context = currentWorkspaceContext();
    if (result.requestSequence != bomEnrichmentSequence_) {
      // A project reopen/re-import superseded this lookup.  It is safe to
      // release the old client now that its future has been joined, ensuring
      // a subsequent run uses the current credentials.
      bomEnrichmentClient_.reset();
    }
    if (bomEnrichmentScopeMatches(bomEnrichmentProjectId_, result.projectId,
                                  context == nullptr ? 0 : context->generation,
                                  result.workspaceGeneration, bomEnrichmentSequence_,
                                  result.requestSequence)) {
      const auto targetProject = find_if(bomProjects_.begin(), bomProjects_.end(), [&](BomProject& candidate) {
        return candidate.id == result.projectId;
      });
      if (targetProject != bomProjects_.end() && !result.key.empty()) {
        targetProject->enrichment[result.key] = result.suggestion;
        dirty_ = true;
      }
    }
    if (result.projectId == bomEnrichmentActiveProjectId_) {
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
    }
    if (context == nullptr || !workspaceGenerationMatches(context->generation, result.workspaceGeneration)) {
      bomEnrichmentQueue_.clear();
      bomEnrichmentTotal_ = 0;
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
      bomEnrichmentClient_.reset();
      return;
    }
    if (bomEnrichmentQueue_.empty() && result.requestSequence == bomEnrichmentSequence_) {
      bomEnrichmentClient_.reset();
      // Persist the project identified by the result, never whichever project
      // happens to be selected when the future completes.
      if (!result.projectId.empty()) saveBomProjects();
      return;
    }
  }

  auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_ || project->id != bomEnrichmentProjectId_) {
    if (!bomEnrichmentFuture_.valid()) {
      bomEnrichmentQueue_.clear();
      bomEnrichmentTotal_ = 0;
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
      bomEnrichmentClient_.reset();
    }
    return;
  }

  if (bomEnrichmentQueue_.empty()) {
    return;
  }

  if (bomEnrichmentClient_ == nullptr) {
    const auto config = loadDigiKeyConfig();
    if (!config.valid()) {
      bomEnrichmentQueue_.clear();
      bomEnrichmentTotal_ = 0;
      bomEnrichmentActiveKey_.clear();
      return;
    }
    bomEnrichmentClient_ = make_unique<DigiKeyApiClient>(config);
  }

  const auto key = bomEnrichmentQueue_.front();
  bomEnrichmentQueue_.erase(bomEnrichmentQueue_.begin());
  bomEnrichmentActiveKey_ = key;
  bomEnrichmentActiveProjectId_ = project->id;

  const auto line = find_if(bomAnalysis_.lines.begin(), bomAnalysis_.lines.end(),
                            [&](const BomLine& candidate) { return bomLineKey(candidate) == key; });
  if (line == bomAnalysis_.lines.end()) {
    return;
  }

  // fetchProductDetails falls back to a keyword search, so a free-form value
  // such as "470uF Radial 8.0mm" resolves as well as a real part number.
  const auto keywords = trim(line->designation + " " + packageFromFootprint(line->footprint));
  auto* client = bomEnrichmentClient_.get();
  const auto generation = bomEnrichmentGeneration_;
  const auto projectId = project->id;
  const auto requestSequence = bomEnrichmentSequence_;
  bomEnrichmentFuture_ = async(launch::async, [client, key, keywords, projectId, generation, requestSequence] {
    string error;
    if (const auto details = client->fetchProductDetails(keywords, &error)) {
      const auto suggestion =
          details->manufacturerPartNumber.empty() ? details->lookupKey : details->manufacturerPartNumber;
      return BomEnrichmentResult{key, suggestion.empty() ? string("-") : suggestion, projectId, generation,
                                 requestSequence};
    }
    return BomEnrichmentResult{key, string("-"), projectId, generation, requestSequence};
  });
  dirty_ = true;
}

void App::openCurrentUrl(const string& url, const string& label) {
  if (trim(url).empty()) {
    setMessage("No " + label + " link stored for this item", 3);
    return;
  }
  if (openUrl(url)) {
    setMessage("Opened " + label + " link", 2);
  } else {
    setMessage("Unable to open " + label + " link", 3);
  }
}

string App::fieldLabel(EditField field) const {
  switch (field) {
    case EditField::PartName:
      return "Part name";
    case EditField::Manufacturer:
      return "Manufacturer";
    case EditField::Category:
      return "Category";
    case EditField::Quantity:
      return "Quantity";
    case EditField::ReorderThreshold:
      return "Reorder threshold";
    case EditField::Location:
      return "Location";
    case EditField::Tags:
      return "Tags";
    case EditField::Parameters:
      return "Parameters";
    case EditField::Notes:
      return "Notes";
    case EditField::LabelOverride:
      return "Label override";
    case EditField::DigiKeyPart:
      return "DigiKey part";
    case EditField::DatasheetUrl:
      return "Datasheet URL";
    case EditField::ProductUrl:
      return "Product URL";
    case EditField::Sku:
      return "SKU";
    case EditField::RackLocation:
      return "Rack location";
  }
  return "Field";
}

string App::currentFieldValue(EditField field) const {
  const auto* item = workingCopy_.item.id.empty() && !workingCopy_.isNew ? selectedItem() : &workingCopy_.item;
  if (item == nullptr) {
    return {};
  }

  switch (field) {
    case EditField::PartName:
      return item->partName;
    case EditField::Manufacturer:
      return item->manufacturer;
    case EditField::Category:
      return item->category;
    case EditField::Quantity:
      return to_string(item->quantity);
    case EditField::ReorderThreshold:
      return to_string(item->reorderThreshold);
    case EditField::Location:
      return item->location;
    case EditField::Tags:
      return join(item->tags, ',');
    case EditField::Parameters: {
      ostringstream out;
      for (size_t index = 0; index < item->parameters.size(); ++index) {
        if (index > 0) {
          out << "; ";
        }
        out << item->parameters[index].name << '=' << item->parameters[index].value;
      }
      return out.str();
    }
    case EditField::Notes:
      return item->notes;
    case EditField::LabelOverride:
      return item->labelOverride;
    case EditField::DigiKeyPart:
      return item->digikeyPartNumber;
    case EditField::DatasheetUrl:
      return item->datasheetUrl;
    case EditField::ProductUrl:
      return item->productUrl;
    case EditField::Sku:
      return item->sku;
    case EditField::RackLocation: {
      const auto location = rackLocation(*item, store_.racks());
      return location.empty() ? (item->rackAssignment == RackAssignmentMode::Automatic ? "AUTO" : "") : location;
    }
  }

  return {};
}

vector<App::FieldOption> App::fieldOptions() const {
  return {
      {"Part name", EditField::PartName},
      {"Manufacturer", EditField::Manufacturer},
      {"Category", EditField::Category},
      {"Quantity", EditField::Quantity},
      {"Reorder threshold", EditField::ReorderThreshold},
      {"Location", EditField::Location},
      {"Rack location", EditField::RackLocation},
      {"Tags", EditField::Tags},
      {"Parameters", EditField::Parameters},
      {"Notes", EditField::Notes},
      {"Label override", EditField::LabelOverride},
      {"DigiKey part", EditField::DigiKeyPart},
      {"Datasheet URL", EditField::DatasheetUrl},
      {"Product URL", EditField::ProductUrl},
      {"SKU", EditField::Sku},
  };
}

string App::softwareVersion() const {
#ifdef Inventatory_VERSION_STRING
  return Inventatory_VERSION_STRING;
#else
  return "dev";
#endif
}

string App::itemDetailText(const InventoryItem& item, int width) const {
  ostringstream out;
  const auto fields = stockPreviewFields(item, rackLocation(item, store_.racks()));
  for (const auto& field : fields) {
    const auto line = field.label + field.value;
    for (const auto& wrapped : wrapText(line, width)) {
      out << wrapped << '\n';
    }
  }

  return out.str();
}

string App::summaryLine() const {
  const auto summary = summarize(store_.items(), settings_.lowStockThreshold);
  ostringstream out;
  out << summary.itemCount << " items"
      << " | " << summary.totalUnits << " units"
      << " | " << summary.lowStockCount << " low"
      << " | " << summary.missingMetadataCount << " missing metadata"
      << " | " << summary.unsyncedCount << " unsynced";
  return out.str();
}

string App::activePrompt() const {
  if (inputMode_ == InputMode::EditValue && fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
    return fieldLabel(menuOptions_[fieldMenuIndex_].field) + ": ";
  }
  if (inputMode_ == InputMode::RackRename) return "Rename rack to: ";
  if (inputMode_ == InputMode::RackType) return "Rack type: ";
  if (inputMode_ == InputMode::RackCreate) return "New rack type: ";
  if (inputMode_ == InputMode::RackJump) return "Jump to rack: ";
  if (inputMode_ == InputMode::RackFilter) return "Rack filter: ";
  if (inputMode_ == InputMode::QuantityAdjust) return "Quantity on hand: ";
  if (inputMode_ == InputMode::StocktakeCount) return "Physical count: ";
  if (inputMode_ == InputMode::HistoryCheckpoint) return "Checkpoint name: ";
  if (inputMode_ == InputMode::HistoryConfirm) return historyConfirmationMessage_;
  if (inputMode_ == InputMode::ExitConfirmation) return "S save  ·  D discard  ·  Esc cancel";
  return "";
}

}  // namespace inventatory
