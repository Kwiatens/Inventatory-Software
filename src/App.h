// Inventatory - Hardware Inventory Management System
// Terminal application controller and shared app state.

#pragma once

#include "core/Inventory.h"
#include "core/InventatoryScanProtocol.h"
#include "core/CatalogueDatabase.h"
#include "app/AppSettings.h"
#include "import/BomCsvImport.h"
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
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

namespace inventatory {

std::filesystem::path documentsInventatoryPath();
std::filesystem::path discoverInventatoryDataPath();
std::filesystem::path legacyDatabasePath();
void copyDatabaseSidecar(const std::filesystem::path& sourceBase,
                         const std::filesystem::path& destinationBase, const std::string& suffix);
void ensureInventoryDatabaseCopied(const std::filesystem::path& localBase);
std::filesystem::path locateDotEnvFile();

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
    ScanSetup,
    Settings,
    Catalogue,
    Onboarding,
  };

  enum class ScanSetupStep {
    Introduction,
    WifiName,
    WifiPassword,
    PairingCode,
    FindScanner,
    Confirm,
    Complete,
  };

  enum class OnboardingStep { Welcome, DataFolder, BackgroundService, ScanR1, Complete };

  enum class SettingsCategory { General, Updates, Printer, QuickLabels, InventatoryScan };

  enum class StockDateFilter { All, Today, Last7Days, Last30Days, OlderThan30Days };
  enum class CatalogueFlow { Sources, WaitingForDownload, ManualMapping, Preview, Importing, Results, Warnings };
  enum class StockSortOrder { Az, Quantity, Za };

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
    EditFieldMenu,
    EditValue,
    RackRename,
    RackType,
    RackCreate,
    RackJump,
    RackFilter,
    StockFilter,
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
    DatasheetUrl,
    ManufacturerPartNumber,
    EnrichmentStatus,
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

  void loadState();
  bool saveState();
  void handleKey(const KeyEvent& key);
  void handleDashboardKey(const KeyEvent& key);
  void handleStockKey(const KeyEvent& key);
  void handleRackManagementKey(const KeyEvent& key);
  void handleInventatoryScanSetupKey(const KeyEvent& key);
  void handleImportCsvKey(const KeyEvent& key);
  void handleSettingsKey(const KeyEvent& key);
  void handleCatalogueKey(const KeyEvent& key);
  void handleOnboardingKey(const KeyEvent& key);
  void handleSearchKey(const KeyEvent& key);
  void handleEditMenuKey(const KeyEvent& key);
  void handleEditValueKey(const KeyEvent& key);
  void handleRackValueKey(const KeyEvent& key);
  void handleStockFilterKey(const KeyEvent& key);

  ftxui::Element renderUi() const;
  ftxui::Element renderHeaderUi() const;
  std::string pageName() const;
  ftxui::Element renderDashboardUi() const;
  ftxui::Element renderStockUi() const;
  ftxui::Element renderRackManagementUi() const;
  ftxui::Element renderInventatoryScanSetupUi() const;
  ftxui::Element renderImportCsvUi() const;
  ftxui::Element renderSettingsUi() const;
  ftxui::Element renderCatalogueUi() const;
  ftxui::Element renderOnboardingUi() const;
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
  void processBackgroundWork();
  void beginUpdateCheckIfDue();
  void processUpdateCheck();
  void runBackgroundLoop();
  void runInteractiveLoop();
  void markDirty();
  void refreshPrinterState();
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
  void advanceOnboarding();
  void finishOnboarding();
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
  void adjustDeviceDebugScroll(int delta);
  std::string inventatoryScanDeviceSummary() const;
  ftxui::Element renderDeviceDebugConsoleUi() const;

  std::vector<size_t> filteredIndices() const;
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
  bool saveSettingsDraft();
  void cancelSettingsDraft();
  bool stageInventatoryFolder();
  bool testStagedPrinter();
  void beginSettingsFieldEdit(int field);
  void commitSettingsFieldEdit();
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
  void cancelInput();
  void beginEditCurrentItem(bool createNew);
  void beginEditImportCandidate();
  void openFieldMenu();
  void commitEditField(EditField field, const std::string& value);
  void saveWorkingCopy();
  void adjustQuantity(int delta);
  void captureUndoSnapshot();
  bool undoLastInventoryChange();
  void logActivity(const std::string& kind, const std::string& message);
  void pushScanCode(const DeviceScanRequest& request);
  void processScans();
  void beginCsvImport();
  void beginCatalogueDownload();
  void chooseCatalogueFile();
  void chooseManualCatalogueFile();
  void beginCatalogueImport();
  void pollCatalogueImport();
  void reEnrichInventoryFromCatalogue();
  void moveImportSelection(int delta);
  void acceptImportCandidate();
  void skipImportCandidate();
  void finishImportReview();
  void finishCsvImport();
  CsvImportCandidate* currentImportCandidate();
  const CsvImportCandidate* currentImportCandidate() const;
  std::string importCompletionMessage() const;
  void openCurrentUrl(const std::string& url, const std::string& label);
  std::string fieldLabel(EditField field) const;
  std::string currentFieldValue(EditField field) const;
  std::vector<FieldOption> fieldOptions() const;
  std::string softwareVersion() const;
  std::string itemDetailText(const InventoryItem& item, int width) const;
  std::string summaryLine() const;
  std::string activePrompt() const;

  InventoryStore store_;
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
  std::filesystem::path cataloguePath_;
  CatalogueDatabase catalogueDatabase_;
  CatalogueDownloadSession catalogueDownloadSession_;
  CatalogueFlow catalogueFlow_ = CatalogueFlow::Sources;
  size_t catalogueSourceSelection_ = 0;
  std::filesystem::path catalogueSelectedPath_;
  CatalogueImportPreview cataloguePreview_;
  CatalogueProfile catalogueManualProfile_;
  size_t catalogueManualCategorySelection_ = 0;
  size_t catalogueManualMappingStep_ = 0;
  size_t catalogueManualColumnSelection_ = 0;
  std::array<size_t, 10> catalogueManualColumns_{};
  bool catalogueRemovalConfirmation_ = false;
  std::atomic_bool catalogueImportCancelled_{false};
  std::future<CatalogueImportStats> catalogueImportFuture_;
  CatalogueImportStats catalogueImportResult_;
  std::vector<CatalogueWarning> catalogueWarnings_;
  struct CatalogueProgress { size_t completed = 0; size_t total = 0; std::string stage; };
  mutable std::mutex catalogueProgressMutex_;
  CatalogueProgress catalogueProgress_;
  Page page_ = Page::Home;
  OnboardingStep onboardingStep_ = OnboardingStep::Welcome;
  bool onboardingActive_ = false;
  bool returnToOnboardingAfterScan_ = false;
  InputMode inputMode_ = InputMode::None;
  std::string searchQuery_;
  std::string searchQueryBeforeEdit_;
  std::string inputBuffer_;
  StockDateFilter stockDateFilter_ = StockDateFilter::All;
  StockSortOrder stockSortOrder_ = StockSortOrder::Az;
  int stockFilterSelection_ = 0;
  bool stockDateFilterSubmenuOpen_ = false;
  std::string message_;
  std::string persistenceError_;
  time_t messageUntil_ = 0;
  size_t selectedPosition_ = 0;
  size_t stockScroll_ = 0;
  size_t detailScroll_ = 0;
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
  std::vector<InventoryHistoryPoint> inventoryHistory_;
  std::mutex scanMutex_;
  InventatoryScanConfig inventatoryScanConfig_;
  std::mutex deviceQueueMutex_;
  std::vector<std::shared_ptr<PendingDeviceQuantity>> deviceQuantityQueue_;
  std::vector<DeviceStatusReport> deviceStatusQueue_;
  std::vector<DeviceDebugReport> deviceDebugQueue_;
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
  bool running_ = true;
  std::atomic<bool> backgroundQuitRequested_{false};
  std::atomic<bool> foregroundRequested_{false};
  bool dirty_ = true;
  WorkingCopy workingCopy_;
  UndoSnapshot undoSnapshot_;
  bool editingImportCandidate_ = false;
  size_t importEditIndex_ = 0;
  size_t importSelection_ = 0;
  int importCreatedCount_ = 0;
  int importMergedCount_ = 0;
  int importSkippedCount_ = 0;
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
  ScanSetupStep scanSetupStep_ = ScanSetupStep::Introduction;
  std::string wireLabelText_;
  time_t scannerFlashUntil_ = 0;
  time_t printerFlashUntil_ = 0;
  bool autoPrintScannedLabels_ = true;
  std::filesystem::path settingsPath_;
  AppSettings settings_;
  AppSettings settingsDraft_;
  SettingsCategory settingsCategory_ = SettingsCategory::General;
  int settingsField_ = 0;
  bool settingsDirty_ = false;
  bool settingsEditingField_ = false;
  std::unordered_map<std::string, DeviceQuickLabelPrintResult> quickLabelPrintResults_;
  std::deque<std::string> quickLabelPrintOrder_;
  mutable std::mutex quickLabelMutex_;
  std::string settingsConfirmAction_;
  time_t settingsConfirmUntil_ = 0;
  std::future<UpdateCheckResult> updateCheckFuture_;
  mutable std::vector<UiTarget> uiTargets_;
  mutable std::string hoveredTargetId_;
  int focusedTarget_ = -1;
};

}  // namespace inventatory
