// Inventatory - Hardware Inventory Management System
// Terminal application controller and shared app state.

#pragma once

#include "core/Inventory.h"
#include "core/InventoryTransfer.h"
#include "core/InventatoryScanProtocol.h"
#include "core/BomMatch.h"
#include "core/BomProjectStore.h"
#include "app/AppSettings.h"
#include "app/AppBootstrap.h"
#include "import/DigiKeyCsvImport.h"
#include "import/KicadBom.h"
#include "platform/DigiKeyApi.h"
#include "label_printer/LabelPrinter.h"
#include "platform/Console.h"
#include "platform/BleProvisioningService.h"
#include "platform/BackgroundController.h"
#include "platform/HttpServer.h"
#include "platform/MdnsService.h"
#include "platform/UpdateService.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <string>
#include <vector>
#include <unordered_map>

namespace inventatory {

std::filesystem::path documentsInventatoryPath();
std::filesystem::path discoverInventatoryDataPath();

class App {
 public:
  App(bool startInBackground, BackgroundController& backgroundController);
  int run();

 private:
  enum class Page {
    Home,
    Stock,
    Racks,
    Import,
    Projects,
    History,
    ScanSetup,
    DigiKeySetup,
    Settings,
    Onboarding,
  };

  // The Projects page hosts three surfaces: the pinned list, the have/need
  // split, and the rack-by-rack build walkthrough.
  enum class BomView { List, Split, Build };

  enum class ScanSetupStep {
    Introduction,
    WifiName,
    WifiPassword,
    PairingCode,
    FindScanner,
    Confirm,
    Complete,
  };

  enum class ScannerDashboardState {
    Unknown,
    Unpaired,
    Waiting,
    Online,
    Offline,
  };

  enum class DigiKeySetupStep {
    Introduction,
    ClientId,
    ClientSecret,
    Review,
  };

  enum class OnboardingStep { Welcome, DataFolder, BackgroundService, ScanR1, Complete };

  enum class WizardTransitionPhase { None, SelectionHold, Blank };

  struct WizardTransition {
    WizardTransitionPhase phase = WizardTransitionPhase::None;
    long long startedAt = -1;
    Page targetPage = Page::Onboarding;
    OnboardingStep targetOnboardingStep = OnboardingStep::Welcome;
    ScanSetupStep targetScanSetupStep = ScanSetupStep::Introduction;
    bool targetReturnToOnboardingAfterScan = false;
  };

  enum class SettingsCategory {
    General,
    Appearance,
    Updates,
    Printer,
    QuickLabels,
    InventatoryScan,
    DigiKey,
  };

  enum class StockDateFilter { All, Today, Last7Days, Last30Days, OlderThan30Days };
  enum class StockSortOrder { Az, Quantity, Za };
  enum class DashboardList { Warnings, Commits };

  enum class UiTargetKind { Navigation, Action, Row, Cell, Field, Link, Category, Button };

  struct UiTarget {
    std::string id;
    UiTargetKind kind = UiTargetKind::Button;
    ftxui::Box bounds;
    bool enabled = true;
    bool focusable = true;
    std::function<void()> activate;
  };

  enum class InputMode {
    None,
    Search,
    ClosestSearch,
    EditFieldMenu,
    EditValue,
    RackRename,
    RackType,
    RackCreate,
    RackJump,
    RackFilter,
    StockFilter,
    StocktakeCount,
    QuantityAdjust,
    BomRestock,
    HistoryCheckpoint,
    HistoryConfirm,
    ExitConfirmation,
    ActionSheet,
  };

  // A single discoverable command. The action bar and the bottom sheet are both
  // generated from these; the accelerator (trigger) dispatches it directly.
  // A trigger with type KeyType::Unknown is menu-only (no direct key).
  struct Action {
    std::string id;       // stable command identity: "stock.edit"
    std::string label;    // concise, sentence case: "edit item"
    std::string group;    // "Edit", "Links", "System"
    std::string keyHint;  // shown next to the label: "e", "Ctrl+Z", "P"
    KeyEvent trigger;     // key that runs it directly
    std::function<void()> run;
  };

  enum class EditField {
    PartName,
    Manufacturer,
    Category,
    Quantity,
    ReorderThreshold,
    Location,
    Tags,
    Parameters,
    Notes,
    LabelOverride,
    DigiKeyPart,
    DatasheetUrl,
    ProductUrl,
    Sku,
    RackLocation,
  };

  struct FieldOption {
    std::string label;
    EditField field;
  };

  struct WorkingCopy {
    InventoryItem item;
    bool isNew = false;
    size_t originalIndex = 0;
  };

  // One part to pull during the build walkthrough.
  struct BuildPick {
    std::string itemId;
    std::string slot;  // rack slot such as "B3"; empty for loose parts
    std::string label;
    std::string detail;
    int quantity = 0;
  };

  // One stop on the walkthrough: a rack, or the final loose-parts screen.
  struct BuildStep {
    std::string rackId;  // empty on the loose-parts step
    std::string title;
    std::string subtitle;
    std::vector<BuildPick> picks;
  };

  struct UndoSnapshot {
    std::vector<InventoryItem> items;
    std::vector<InventatoryRack> racks;
    std::vector<ActivityEntry> activities;
    size_t selectedPosition = 0;
    bool valid = false;
  };

  struct PendingDeviceQuantity {
    DeviceQuantityRequest request;
    DeviceQuantityResult result;
    std::mutex mutex;
    std::condition_variable ready;
    bool complete = false;
  };

  struct DigiKeyRefreshResult {
    std::string itemId;
    std::optional<DigiKeyProductDetails> details;
    std::string error;
  };

  struct ImportSyncBatchResult {
    std::vector<std::pair<std::string, std::optional<DigiKeyProductDetails>>> results;
    std::vector<std::string> failedItemIds;
  };

  void loadState();
  bool saveState(const std::string& movementSource = "manual",
                 const std::string& movementReference = {}, const std::string& commitMessage = {});
  bool saveInventoryState(const InventoryCommitDraft& draft);
  void refreshHistoryDetail();
  void moveHistorySelection(int delta);
  void openSelectedHistoryCommit();
  void beginHistoryCheckpoint();
  void beginHistoryRestore(InventoryRevertMode mode);
  void cancelHistoryAction();
  bool applyHistoryRevert(InventoryRevertMode mode);
  void handleKey(const KeyEvent& key);
  void handleDashboardKey(const KeyEvent& key);
  void handleStockKey(const KeyEvent& key);
  void handleRackManagementKey(const KeyEvent& key);
  void handleInventatoryScanSetupKey(const KeyEvent& key);
  void handleDigiKeySetupKey(const KeyEvent& key);
  void handleImportCsvKey(const KeyEvent& key);
  void handleBomProjectKey(const KeyEvent& key);
  void handleSettingsKey(const KeyEvent& key);
  void handleOnboardingKey(const KeyEvent& key);
  void handleSearchKey(const KeyEvent& key);
  void handleEditMenuKey(const KeyEvent& key);
  void handleEditValueKey(const KeyEvent& key);
  void handleRackValueKey(const KeyEvent& key);
  void handleStockFilterKey(const KeyEvent& key);
  void handleStocktakeCountKey(const KeyEvent& key);
  void handleQuantityAdjustKey(const KeyEvent& key);
  void handleBomRestockKey(const KeyEvent& key);
  void handleHistoryKey(const KeyEvent& key);
  void handleExitConfirmationKey(const KeyEvent& key);

  ftxui::Element renderUi() const;
  ftxui::Element renderHeaderUi() const;
  std::string pageName() const;
  ftxui::Element renderDashboardUi() const;
  ftxui::Element renderStockUi() const;
  ftxui::Element renderRackManagementUi() const;
  ftxui::Element renderInventatoryScanSetupUi() const;
  ftxui::Element renderDigiKeySetupUi() const;
  ftxui::Element renderImportCsvUi() const;
  ftxui::Element renderBomProjectUi() const;
  ftxui::Element renderHistoryUi() const;
  ftxui::Element renderSettingsUi() const;
  ftxui::Element renderOnboardingWordmark() const;
  ftxui::Element renderOnboardingFrame(ftxui::Element content) const;
  ftxui::Element renderOnboardingContent() const;
  ftxui::Element renderOnboardingUi() const;
  ftxui::Element renderWizardUi() const;
  ftxui::Element renderWizardContent() const;
  ftxui::Element renderInventatoryScanSetupContent() const;
  std::string settingsCategoryName(SettingsCategory category) const;
  std::string stockDateFilterName(StockDateFilter filter) const;
  bool stockDateFilterMatches(const InventoryItem& item) const;
  ftxui::Element renderSearchBarUi() const;
  ftxui::Element renderActionSheetUi() const;
  ftxui::Element renderMessageUi() const;
  ftxui::Element renderPageUi() const;

  // Action registry (contextual bar + bottom sheet). Defined in ActionRegistry.cpp.
  std::vector<Action> currentActions() const;
  bool triggerMatches(const KeyEvent& trigger, const KeyEvent& key) const;
  bool dispatchAction(const KeyEvent& key);
  void openActionSheet();
  void handleActionSheetKey(const KeyEvent& key);
  ftxui::Element target(ftxui::Element element, std::string id, UiTargetKind kind,
                        std::function<void()> activate, bool enabled = true, bool focusable = true) const;
  bool handleMouse(const ftxui::Mouse& mouse);
  void moveUiFocus(int delta);
  bool activateFocusedTarget();

  void setMessage(std::string text, int seconds = 3);
  bool messageVisible() const;
  void clearMessageIfExpired();
  void requestUserExit();
  void completeSettingsExit(bool saveChanges);
  void restartDeviceService();
  void processBackgroundWork();
  void processScanDigiKeyEnrichment();
  void beginDigiKeyRefresh();
  void processDigiKeyRefresh();
  void stopDigiKeyRefresh();
  void beginUpdateCheckIfDue();
  void beginUpdateChecks();
  void processUpdateCheck();
  void beginScanFirmwareCheck();
  void processScanFirmwareCheck();
  std::string scanFirmwareStatus() const;
  void runBackgroundLoop();
  void runInteractiveLoop();
  void markDirty();
  void refreshPrinterState();
  void refreshInventoryMovements();
  void refreshInventoryCommits();
  void openPrinterSetup();
  bool printSelectedLabel();
  bool printWireLabel(const std::string& text);
  bool printDeviceQuickLabel(const DeviceQuickLabelPrintRequest& request, DeviceQuickLabelPrintResult& result);
  void addQuickLabelPreset();
  void deleteQuickLabelPreset();
  void moveQuickLabelPreset(int direction);
  void testQuickLabelPreset();
  bool printLabelForItem(const InventoryItem& item, const std::string& successPrefix, bool openSetupOnMissingPrinter);
  void toggleAutoPrintScannedLabels();
  bool autoPrintScannedLabel(const std::string& itemId);
  std::string printerSummary() const;
  void openInventatoryScanSetup();
  void openDigiKeySetup();
  void advanceOnboarding();
  void finishOnboarding();
  void beginWizardTransition(Page targetPage, OnboardingStep targetOnboardingStep,
                             ScanSetupStep targetScanSetupStep,
                             bool targetReturnToOnboardingAfterScan);
  void updateWizardTransition();
  void applyWizardTransitionTarget();
  bool regenerateInventatoryScanToken();
  bool clearInventatoryScanPairing();
  bool copyInventatoryScanToken();
  void refreshBleSetupDiscovery();
  bool provisionSelectedBleSetupDevice();
  DeviceQuantityResult enqueueDeviceQuantity(const DeviceQuantityRequest& request);
  void enqueueDeviceStatus(const DeviceStatusReport& report);
  void processDeviceRequests();
  void enqueueDeviceDebug(const DeviceDebugReport& report);
  bool handleDeviceSync(const DeviceSyncRequest& request, DeviceSyncResponse& response, std::string& error);
  void processDeviceSyncEvents();
  void refreshDeviceEventRecords();
  void retryFailedDeviceEvents();
  void discardFailedDeviceEvents();
  void updateDashboardScannerState();
  void adjustDeviceDebugScroll(int delta);
  void moveDashboardSelection(DashboardList list, int delta);
  void jumpDashboardSelection(DashboardList list, bool toEnd);
  void selectDashboardRow(DashboardList list, size_t position);
  std::string inventatoryScanDeviceSummary() const;
  ftxui::Element renderDeviceDebugConsoleUi() const;

  std::vector<size_t> filteredIndices() const;
  std::vector<InventorySearchMatch> stockSearchMatches() const;
  size_t selectedIndex() const;
  InventoryItem* selectedItem();
  const InventoryItem* selectedItem() const;
  PrinterQueueInfo* selectedPrinterQueue();
  const PrinterQueueInfo* selectedPrinterQueue() const;
  void syncSelectionToFilter();
  void moveSelection(int delta);
  void changePage(Page page);
  bool chooseInventatoryFolder();
  void openSettings(SettingsCategory category = SettingsCategory::General);
  void beginSettingsEdit();
  bool settingsDraftHasChanges() const;
  bool saveSettingsDraft();
  void cancelSettingsDraft();
  bool stageInventatoryFolder();
  bool testStagedPrinter();
  bool testStagedDigiKey();
  void beginSettingsFieldEdit(int field);
  void commitSettingsFieldEdit();
  void resetSelectedAppearanceColor();
  void resetAppearanceColors();
  void openAppearancePicker();
  void moveAppearancePicker(int hueDelta, int valueDelta);
  void applyAppearancePickerColor();
  void closeAppearancePicker(bool accept);
  void armDeleteConfirmation();
  void cancelDeleteConfirmation();
  void clearDeleteConfirmationIfExpired();
  void confirmDeleteSelectedItem();
  bool deleteConfirmationActive() const;
  bool deleteConfirmationReady() const;
  int deleteConfirmationSecondsLeft() const;
  void openSelectedDetail();
  void openRackManagement();
  std::vector<size_t> sortedRackIndices() const;
  const InventatoryRack* selectedRack() const;
  InventatoryRack* selectedRack();
  std::string selectedRackSlot() const;
  InventoryItem* selectedRackItem();
  const InventoryItem* selectedRackItem() const;
  void syncRackSelection();
  void moveRackSlot(int rowDelta, int columnDelta);
  void moveRackPage(int delta);
  void beginOrCompleteRackMove();
  void unassignSelectedRackItem();
  void autoAssignSelectedRackItem();
  void renameSelectedRack(const std::string& value);
  void changeSelectedRackType(const std::string& value);
  void createRackWithType(const std::string& value);
  void deleteSelectedRack();
  void jumpToRack(const std::string& value);
  void beginRackFilter();
  bool printSelectedRackPartLabel();
  bool printSelectedRackLabel();
  void adjustSelectedRackItemQuantity(int delta);
  void openSelectedRackItemDetail();
  void openStockFilterPanel();
  void openStockDateFilterSubmenu();
  void applyStockDateFilter(StockDateFilter filter);
  void applyStockSortOrder(StockSortOrder order);
  void startSearch();
  void startClosestSearch();
  void handleClosestSearchKey(const KeyEvent& key);
  void clearClosestSearch();
  void cancelInput();
  void beginEditCurrentItem(bool createNew);
  void beginEditImportCandidate();
  void openFieldMenu();
  void commitEditField(EditField field, const std::string& value);
  void saveWorkingCopy();
  void adjustQuantity(int delta);
  void setSelectedQuantityFromInput(const std::string& value);
  void beginStocktake();
  void cancelStocktake();
  void finishStocktake();
  void beginStocktakeCount();
  int stocktakeCountFor(const InventoryItem& item) const;
  size_t stocktakeCountedItems() const;
  void captureUndoSnapshot();
  bool undoLastInventoryChange();
  void logActivity(const std::string& kind, const std::string& message);
  void pushScanCode(const DeviceScanRequest& request);
  void processScans();
  void beginCsvImport();
  void cancelImportSession();
  bool commitImportStage();
  void moveImportSelection(int delta);
  void acceptImportCandidate();
  void skipImportCandidate();
  void finishImportReview();
  void finishCsvImport(bool syncWithDigiKey);
  void beginImportSync(bool retryFailed = false);
  void processImportSync();
  void retryImportSync();
  CsvImportCandidate* currentImportCandidate();
  const CsvImportCandidate* currentImportCandidate() const;
  std::string importCompletionMessage() const;

  // KiCad BOM workflow. Analysis always runs against live stock, so a pinned
  // project stays accurate as inventory changes.
  void openBomProjects();
  void beginBomProject(const std::string& bomText, const std::string& name,
                       const std::filesystem::path& sourcePath);
  void refreshBomAnalysis();
  void adjustBomBoards(int delta);
  void cycleBomAlternate();
  void beginBomRestock();
  void deleteSelectedBomProject();
  void openSelectedBomProject();
  void moveBomSelection(int delta);
  void beginBomBuild();
  void advanceBomBuild(int delta);
  void finishBomBuild(bool subtractFromStock);
  bool exportBomShortages();
  void queueBomEnrichment();
  void processBomEnrichment();
  bool saveBomProjects();
  bool exportInventory();
  bool backupData();
  bool restoreData();
  void retrySaveState();
  std::vector<BuildStep> bomBuildSteps() const;
  BomProject* activeBomProject();
  const BomProject* activeBomProject() const;

  void openCurrentUrl(const std::string& url, const std::string& label);
  std::string fieldLabel(EditField field) const;
  std::string currentFieldValue(EditField field) const;
  std::vector<FieldOption> fieldOptions() const;
  std::string softwareVersion() const;
  std::string itemDetailText(const InventoryItem& item, int width) const;
  std::string summaryLine() const;
  std::string activePrompt() const;

  InventoryStore store_;
  InventoryStore persistedStore_;
  bool persistedStoreValid_ = false;
  InventoryStore importOriginalStore_;
  InventoryStore importStagedStore_;
  std::vector<ActivityEntry> activities_;
  LabelPrinterService printerService_;
  std::vector<PrinterQueueInfo> printerQueues_;
  PrinterCheckResult printerCheck_;
  LocalHttpServer server_;
  BackgroundController& backgroundController_;
  bool startInBackground_ = false;
  BleProvisioningService bleProvisioning_;
  MdnsService mdnsService_;
  std::filesystem::path root_;
  std::filesystem::path dataPath_;
  std::filesystem::path inventoryPath_;
  std::filesystem::path printerPath_;
  std::filesystem::path activityPath_;
  std::filesystem::path inventatoryScanConfigPath_;
  std::filesystem::path quickLabelsPath_;
  Page page_ = Page::Home;
  OnboardingStep onboardingStep_ = OnboardingStep::Welcome;
  bool onboardingActive_ = false;
  bool returnToOnboardingAfterScan_ = false;
  WizardTransition wizardTransition_;
  std::optional<KeyEvent> bufferedWizardKey_;
  std::optional<char> wizardSelectedOption_;
  InputMode inputMode_ = InputMode::None;
  std::string searchQuery_;
  std::string searchQueryBeforeEdit_;
  std::string closestSearchQuery_;
  std::string inputBuffer_;
  StockDateFilter stockDateFilter_ = StockDateFilter::All;
  StockSortOrder stockSortOrder_ = StockSortOrder::Az;
  bool stocktakeActive_ = false;
  bool stocktakeCommitPending_ = false;
  std::string stocktakeSessionId_;
  std::unordered_map<std::string, int> stocktakeCounts_;
  int stockFilterSelection_ = 0;
  bool stockDateFilterSubmenuOpen_ = false;
  // Vendor/catalogue identifiers are reference data, not what the page is for,
  // so the details block starts collapsed.
  bool stockDetailsExpanded_ = false;
  std::string message_;
  std::string persistenceError_;
  std::string pendingMovementSource_;
  std::string pendingMovementReference_;
  InventoryCommitDraft pendingCommitDraft_;
  bool pendingCommitDraftValid_ = false;
  bool inventoryRecoveryRequired_ = false;
  std::string inventoryRecoveryDetail_;
  time_t messageUntil_ = 0;
  long long messageFlashStartedAt_ = -1;
  size_t selectedPosition_ = 0;
  size_t closestSelectedPosition_ = 0;
  bool closestSearchActive_ = false;
  size_t stockScroll_ = 0;
  size_t detailScroll_ = 0;
  DashboardList dashboardList_ = DashboardList::Warnings;
  size_t dashboardWarningSelection_ = 0;
  size_t dashboardActivitySelection_ = 0;
  size_t rackSelection_ = 0;
  int rackRow_ = 0;
  int rackColumn_ = 0;
  std::string movingRackItemId_;
  std::string movingRackSource_;
  std::string rackFilter_;
  std::vector<DeviceScanRequest> scanQueue_;
  std::vector<CsvImportCandidate> importCandidates_;
  std::vector<std::string> importAcceptedItemIds_;
  std::filesystem::path importSourcePath_;
  bool importStageActive_ = false;
  bool importCommitPending_ = false;
  std::vector<InventoryHistoryPoint> inventoryHistory_;
  std::vector<InventoryMovement> inventoryMovements_;
  std::vector<InventoryCommit> inventoryCommits_;
  size_t historySelection_ = 0;
  InventoryCommitDetail historyDetail_;
  bool historyDetailValid_ = false;
  InventoryRevertMode pendingHistoryRevertMode_ = InventoryRevertMode::Snapshot;
  std::string historyConfirmationMessage_;
  std::mutex scanMutex_;
  InventatoryScanConfig inventatoryScanConfig_;
  std::mutex deviceQueueMutex_;
  std::vector<std::shared_ptr<PendingDeviceQuantity>> deviceQuantityQueue_;
  std::vector<DeviceStatusReport> deviceStatusQueue_;
  std::vector<DeviceDebugReport> deviceDebugQueue_;
  std::vector<DeviceSyncEventRecord> deviceEventRecords_;
  std::vector<std::string> deviceDebugLog_;
  std::unordered_map<std::string, DeviceQuantityResult> deviceRequestCache_;
  std::deque<std::string> deviceRequestOrder_;
  time_t deviceLastSeen_ = 0;
  std::string deviceFirmwareVersion_;
  int deviceRssi_ = 0;
  std::string deviceDebug_;
  std::string deviceLastResult_;
  int deviceProtocolVersion_ = 0;
  std::string deviceMode_;
  int devicePendingEventCount_ = 0;
  time_t deviceLastSync_ = 0;
  size_t deviceDebugScroll_ = 0;
  bool deviceDebugFollow_ = true;
  std::atomic<bool> running_{true};
  std::atomic<bool> backgroundQuitRequested_{false};
  std::atomic<bool> foregroundRequested_{false};
  bool dirty_ = true;
  WorkingCopy workingCopy_;
  UndoSnapshot undoSnapshot_;
  bool editingImportCandidate_ = false;
  size_t importEditIndex_ = 0;
  size_t importSelection_ = 0;
  bool importSyncPrompt_ = false;
  int importCreatedCount_ = 0;
  int importMergedCount_ = 0;
  int importSkippedCount_ = 0;
  int importSyncedCount_ = 0;
  int importSyncFailedCount_ = 0;
  std::future<ImportSyncBatchResult> importSyncFuture_;
  std::shared_ptr<std::atomic<bool>> importSyncCancelFlag_;
  std::vector<std::string> importSyncFailedItemIds_;
  size_t importSyncTotal_ = 0;
  size_t importSyncCompleted_ = 0;
  bool importSyncRunning_ = false;
  bool importSyncHasRun_ = false;
  bool importSyncCancelRequested_ = false;
  std::vector<BomProject> bomProjects_;
  std::string activeBomProjectId_;
  size_t bomProjectSelection_ = 0;
  BomView bomView_ = BomView::List;
  bool bomAnalysisValid_ = false;
  BomAnalysis bomAnalysis_;
  KicadBomFile bomFile_;
  size_t bomSplitSelection_ = 0;
  // Keep wheel/key navigation within the active half of the BOM split view.
  bool bomSplitShortFocused_ = false;
  mutable ftxui::Box bomReadyPanelBounds_;
  mutable ftxui::Box bomShortPanelBounds_;
  mutable ftxui::Box dashboardWarningPanelBounds_;
  mutable ftxui::Box dashboardActivityPanelBounds_;
  size_t bomBuildStep_ = 0;
  bool bomDeductPrompt_ = false;
  std::string bomRestockItemId_;
  std::string bomDeleteConfirmationProjectId_;
  time_t bomDeleteConfirmationUntil_ = 0;
  bool bomProjectsDirty_ = false;
  // Line keys still awaiting a DigiKey suggestion; drained one per tick so the
  // terminal stays responsive while lookups run.
  std::vector<std::string> bomEnrichmentQueue_;
  size_t bomEnrichmentTotal_ = 0;
  std::string bomEnrichmentActiveKey_;
  // One lookup in flight at a time, off the render thread. A DigiKey call takes
  // seconds and would otherwise freeze the terminal for the whole shortage run.
  // The client is declared first on purpose: members are destroyed in reverse,
  // so the future (which joins its task) must outlive the client it borrows.
  std::unique_ptr<DigiKeyApiClient> bomEnrichmentClient_;
  std::future<std::pair<std::string, std::string>> bomEnrichmentFuture_;
  std::deque<std::pair<std::string, std::string>> scanDigiKeyEnrichmentQueue_;
  std::future<std::pair<std::string, std::optional<DigiKeyProductDetails>>> scanDigiKeyEnrichmentFuture_;
  // Inventory-wide DigiKey recovery runs one lookup per tick so restoring
  // lost vendor metadata never blocks the terminal or changes stock counts.
  std::deque<std::pair<std::string, std::string>> digiKeyRefreshQueue_;
  size_t digiKeyRefreshTotal_ = 0;
  size_t digiKeyRefreshCompleted_ = 0;
  size_t digiKeyRefreshSucceeded_ = 0;
  size_t digiKeyRefreshFailed_ = 0;
  bool digiKeyRefreshChanged_ = false;
  std::string digiKeyRefreshActiveKey_;
  std::string digiKeyRefreshLastError_;
  std::unique_ptr<DigiKeyApiClient> digiKeyRefreshClient_;
  std::future<DigiKeyRefreshResult> digiKeyRefreshFuture_;
  int fieldMenuIndex_ = 0;
  std::vector<FieldOption> menuOptions_;
  std::vector<Action> sheetActions_;
  int sheetIndex_ = 0;
  std::string deleteConfirmationItemId_;
  time_t deleteConfirmationUntil_ = 0;
  size_t printerSelection_ = 0;
  size_t bleSetupSelection_ = 0;
  std::string bleWifiSsid_;
  std::string bleWifiPassword_;
  std::string blePairingCode_;
  std::string bleSetupMessage_;
  bool bleSetupOutcomeUncertain_ = false;
  ScanSetupStep scanSetupStep_ = ScanSetupStep::Introduction;
  DigiKeySetupStep digiKeySetupStep_ = DigiKeySetupStep::Introduction;
  std::string wireLabelText_;
  time_t scannerFlashUntil_ = 0;
  ScannerDashboardState scannerDashboardState_ = ScannerDashboardState::Unknown;
  long long scannerDashboardTransitionStartedAt_ = -1;
  bool scannerDashboardTransitionExpanding_ = false;
  time_t printerFlashUntil_ = 0;
  bool autoPrintScannedLabels_ = true;
  std::filesystem::path settingsPath_;
  AppSettings settings_;
  AppSettings settingsDraft_;
  SettingsCategory settingsCategory_ = SettingsCategory::General;
  int settingsField_ = 0;
  bool settingsDirty_ = false;
  bool settingsEditingField_ = false;
  bool appearancePickerOpen_ = false;
  int appearancePickerHue_ = 0;
  int appearancePickerValue_ = 0;
  std::uint32_t appearancePickerOriginalColor_ = 0;
  std::string stagedDigiKeySecret_;
  bool stagedDigiKeySecretChanged_ = false;
  bool hasStoredDigiKeySecret_ = false;
  std::unordered_map<std::string, DeviceQuickLabelPrintResult> quickLabelPrintResults_;
  std::deque<std::string> quickLabelPrintOrder_;
  mutable std::mutex quickLabelMutex_;
  std::string settingsConfirmAction_;
  std::filesystem::path pendingRestoreBackupPath_;
  std::optional<Page> pendingPageAfterSettings_;
  time_t settingsConfirmUntil_ = 0;
  std::future<UpdateCheckResult> updateCheckFuture_;
  std::future<UpdateCheckResult> scanFirmwareFuture_;
  bool updateCheckChecked_ = false;
  bool updateCheckFailed_ = false;
  std::string scanFirmwareLatestVersion_;
  bool scanFirmwareChecked_ = false;
  bool scanFirmwareCheckFailed_ = false;
  mutable std::vector<UiTarget> uiTargets_;
  mutable std::string hoveredTargetId_;
  int focusedTarget_ = -1;
};

}  // namespace inventatory
