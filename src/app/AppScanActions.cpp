// Inventatory - Hardware Inventory Management System
// Scanner, quick-label, printer, and device-event actions.

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

void App::storeQuickLabelPrintResult(const DeviceQuickLabelPrintResult& result,
                                     const QuickLabelPrintCacheIdentity& identity) {
  if (result.requestId.empty() || identity.workspaceGeneration == 0 ||
      identity.request.requestId != result.requestId) {
    return;
  }
  lock_guard<mutex> lock(quickLabelMutex_);
  const auto known = quickLabelPrintResults_.find(result.requestId);
  const bool wasKnown = known != quickLabelPrintResults_.end();
  if (known != quickLabelPrintResults_.end()) {
    // A late completion from an older operation must not overwrite a newer
    // operation that reused the same request id.
    if (!quickLabelPrintCacheIdentityMatches(known->second.identity, identity)) return;
  }
  quickLabelPrintResults_[result.requestId] = {identity, result};
  if (!wasKnown &&
      find(quickLabelPrintOrder_.begin(), quickLabelPrintOrder_.end(), result.requestId) ==
          quickLabelPrintOrder_.end()) {
    quickLabelPrintOrder_.push_back(result.requestId);
  }
  while (quickLabelPrintOrder_.size() > 64) {
    const auto evict = find_if(quickLabelPrintOrder_.begin(), quickLabelPrintOrder_.end(), [&](const string& id) {
      const auto entry = quickLabelPrintResults_.find(id);
      return entry == quickLabelPrintResults_.end() || entry->second.result.status != "pending";
    });
    if (evict == quickLabelPrintOrder_.end()) break;
    quickLabelPrintResults_.erase(*evict);
    quickLabelPrintOrder_.erase(evict);
  }
}

void App::clearQuickLabelPrintCache() {
  lock_guard<mutex> lock(quickLabelMutex_);
  quickLabelPrintResults_.clear();
  quickLabelPrintOrder_.clear();
}

bool App::printDeviceQuickLabel(const DeviceQuickLabelPrintRequest& request, const string& deviceId,
                                WorkspaceGeneration workspaceGeneration,
                                DeviceQuickLabelPrintResult& result) {
  result.requestId = request.requestId;
  string text;
  QuickLabelPrintCacheIdentity identity;
  identity.request = request;
  identity.deviceId = deviceId;
  identity.workspaceGeneration = workspaceGeneration;
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    const auto known = quickLabelPrintResults_.find(request.requestId);
    if (request.presetIndex >= 1 && request.presetIndex <= static_cast<int>(settings_.quickLabelPresets.size())) {
      identity.labelText = settings_.quickLabelPresets[static_cast<size_t>(request.presetIndex - 1)];
    }
    if (known != quickLabelPrintResults_.end()) {
      if (quickLabelPrintCacheIdentityMatches(known->second.identity, identity)) {
        result = known->second.result;
        return result.status == "completed";
      }
      // This request id is being reused for a different operation.  Remove
      // the old entry while retaining its order slot for bounded eviction.
      quickLabelPrintResults_.erase(known);
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
    storeQuickLabelPrintResult(result, identity);
    return false;
  }

  // printer.conf belongs to the active workspace. The app-settings copy can
  // still describe the previously selected workspace during a switch.
  string printerName = printerService_.configuredPrinter();
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
      work.quickLabelIdentity = identity;
      if (!enqueuePrinterWork(move(work))) {
        result = {request.requestId, "failed", "printer_queue_full", "Printer queue is full"};
      }
    }
  }

  storeQuickLabelPrintResult(result, identity);
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
    printDeviceQuickLabel(request.quickLabelPrint, request.deviceId, context->generation,
                          response.quickLabelPrintResult);
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
      deviceRequestCache_.clear();
      deviceRequestOrder_.clear();
      clearQuickLabelPrintCache();
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
      deviceRequestCache_.clear();
      deviceRequestOrder_.clear();
      clearQuickLabelPrintCache();
    }
    const auto before = store_;
    auto result = applyDeviceQuantityCached(store_, pending->request, pending->workspaceGeneration,
                                             deviceRequestCache_, deviceRequestOrder_);
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


}  // namespace inventatory
