// Inventatory - scanner device event, status, and request actions.

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
#include <sstream>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;
using namespace app_actions;

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
  const bool accepted = acceptDeviceSyncEvents(context->paths.inventory, request, response, error);
  deviceSyncEventsHint_.store(true);
  if (!accepted) return false;
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
  deviceSyncEventsHint_.store(true);
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
  // New events announce themselves through deviceSyncEventsHint_; the periodic look is only a
  // safety net, so an idle service does not query SQLite ten times a second.
  const auto now = chrono::steady_clock::now();
  // After a failed commit the same oldest event would fail again immediately, so it is not retried
  // before the back-off expires. The hint is left alone so the retry happens as soon as it does.
  if (now < deviceSyncRetryAfter_ && workspaceIsCurrent(deviceSyncRetryGeneration_)) return;
  if (!deviceSyncEventsHint_.exchange(false) && now - lastDeviceSyncEventPoll_ < chrono::seconds(5)) return;
  lastDeviceSyncEventPoll_ = now;
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;
  const auto pending = loadPendingDeviceSyncEvents(context->paths.inventory, 1);
  if (pending.empty()) return;
  // After a failed save the unsaved edit stays in store_ for retry. An event applied to that store
  // would be committed together with the edit under a baseline that lacks it, and the pending
  // commit draft would be lost. The events stay in the durable inbox (the device already has its
  // acknowledgement, so nothing waits on this); they are processed when the save succeeds or the
  // change is discarded, and the periodic poll picks them up otherwise.
  if (!persistedStoreValid_ || inventoryHasChanges(persistedStore_, store_)) {
    setMessage("Scanner events are waiting for unsaved inventory changes; press R to retry saving", 4,
               UiMessageSeverity::Warning);
    return;
  }

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
  string enrichmentLookup;  // queued for DigiKey only after the event is durably committed
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
      enrichmentLookup = shouldEnrich ? trim(event.code) : string();
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
  if (!completeDeviceSyncEvent(candidate, context->paths.inventory, result, &persistedStore_)) {
    // The event stays in the durable inbox in the received state and is retried with a growing delay;
    // the status line keeps saying so instead of the loop re-running on every tick.
    ++deviceSyncCommitFailures_;
    deviceSyncRetryAfter_ = now + app_actions::deviceSyncRetryDelay(deviceSyncCommitFailures_);
    deviceSyncRetryGeneration_ = context->generation;
    deviceLastResult_ = "ERROR scanner event not saved; retrying";
    setMessage("Inventatory Scan event could not be committed; it stays queued and will be retried", 5,
               UiMessageSeverity::Warning);
    dirty_ = true;
    return;
  }
  deviceSyncCommitFailures_ = 0;
  deviceSyncRetryAfter_ = {};
  deviceSyncEventsHint_.store(true);  // keep draining until the inbox is empty
  if (!enrichmentLookup.empty()) scanDigiKeyEnrichmentQueue_.emplace_back(affectedItemId, enrichmentLookup);
  store_ = move(candidate);
  persistedStore_ = store_;
  persistedStoreValid_ = true;
  refreshInventoryMovements();
  // The commit was just appended through the same transaction as a normal save; validating the whole
  // history again for every scanned event would grow with each scan.
  refreshInventoryCommits(true);
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
  vector<QueuedDeviceStatus> statuses;
  vector<QueuedDeviceDebug> debugReports;
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
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
}

string App::inventatoryScanDeviceSummary() const {
  if (trim(inventatoryScanConfig_.token).empty()) return "R1 UNPAIRED";
  if (trim(inventatoryScanConfig_.deviceId).empty()) return "R1 WAITING FOR DEVICE";
  if (deviceLastSeen_ == 0 || time(nullptr) - deviceLastSeen_ > 15) return "R1 OFFLINE";
  if (!deviceLastResult_.empty()) return "R1 ONLINE  " + deviceLastResult_;
  return "R1 ONLINE  RSSI " + to_string(deviceRssi_);
}

}  // namespace inventatory
