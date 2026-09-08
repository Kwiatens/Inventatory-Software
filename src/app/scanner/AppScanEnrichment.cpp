// Inventatory - scanner queueing and DigiKey enrichment workflows.

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
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

}  // namespace inventatory
