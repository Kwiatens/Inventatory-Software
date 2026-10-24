// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - scanner device event, status, and request actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/storage/InventorySqlite.h"
#include "core/inventory/VendorBagCode.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
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
  // New events set deviceSyncEventsHint_; the 5 s poll is only a safety net.
  const auto now = chrono::steady_clock::now();
  // After a failed commit, wait out the back-off; the hint stays set so the retry runs then.
  if (now < deviceSyncRetryAfter_ && workspaceIsCurrent(deviceSyncRetryGeneration_)) return;
  if (!deviceSyncEventsHint_.exchange(false) && now - lastDeviceSyncEventPoll_ < chrono::seconds(5)) return;
  lastDeviceSyncEventPoll_ = now;
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;
  const auto pending = loadPendingDeviceSyncEvents(context->paths.inventory, 1);
  if (pending.empty()) return;
  // Do not apply events over unsaved edits: they would commit under a baseline lacking them and
  // lose the pending draft. They stay queued until the save succeeds or the change is discarded.
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
  string enrichmentLookup;  // queued for DigiKey after the commit is durable
  string tmeLookupSymbol;   // queued for TME after the commit is durable
  string mouserLookupPartNumber;  // queued for Mouser after the commit is durable
  string mouserLookupManufacturer;
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
                                app_actions::isScanPlaceholderName(item->partName);
      // Look up only at the vendor the part came from, by the part number the bag code names.
      if (const auto identity = recognizeVendorBagCode(event.code);
          identity.has_value() && shouldEnrich && originProvider(*item) == identity->provider) {
        if (identity->provider == "tme") {
          tmeLookupSymbol = identity->providerProductNumber;
        } else if (identity->provider == "mouser") {
          mouserLookupPartNumber = identity->manufacturerPartNumber;
          mouserLookupManufacturer = identity->manufacturer;
        } else {
          enrichmentLookup = identity->providerProductNumber;
        }
      }
      reconcileRackAssignment(candidate, *item);
      result.itemName = deviceItemName(*item);
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
    // The event stays in the inbox and is retried with a growing delay.
    ++deviceSyncCommitFailures_;
    deviceSyncRetryAfter_ = now + app_actions::deviceSyncRetryDelay(deviceSyncCommitFailures_);
    deviceSyncRetryGeneration_ = context->generation;
    setMessage("Inventatory Scan event could not be committed; it stays queued and will be retried", 5,
               UiMessageSeverity::Warning);
    dirty_ = true;
    return;
  }
  deviceSyncCommitFailures_ = 0;
  deviceSyncRetryAfter_ = {};
  deviceSyncEventsHint_.store(true);  // keep draining until the inbox is empty
  // A vendor that is off is not asked, and a new part's label must not wait for it.
  if (!settings_.digiKeyEnabled) enrichmentLookup.clear();
  if (!enrichmentLookup.empty()) scanDigiKeyEnrichmentQueue_.emplace_back(affectedItemId, enrichmentLookup);
  const bool tmeLookupQueued = !tmeLookupSymbol.empty() && enqueueTmeScanLookup(affectedItemId, tmeLookupSymbol);
  // A created part prints after its lookup settles, so the label has the merged details.
  const bool mouserLookupQueued = !mouserLookupPartNumber.empty() &&
                                  enqueueMouserScanLookup(affectedItemId, mouserLookupPartNumber,
                                                          mouserLookupManufacturer);
  if (created && (!enrichmentLookup.empty() || tmeLookupQueued || mouserLookupQueued)) {
    queueScanAutoPrint(affectedItemId);
  }
  store_ = move(candidate);
  persistedStore_ = store_;
  persistedStoreValid_ = true;
  refreshInventoryMovements();
  // Skip the full history check: validating it per scanned event would grow with each scan.
  refreshInventoryCommits(true);
  if (result.status == "failed") {
    logActivity("device error", result.message);
  } else {
    logActivity(created ? "scan receive" : "stock receive",
                result.itemName + " changed by " + to_string(result.appliedDelta) + " to " +
                    to_string(result.quantity));
  }
  saveActivitiesChecked();
  refreshDeviceEventRecords();
  dirty_ = true;
}

void App::processDeviceRequests() {
  vector<QueuedDeviceStatus> statuses;
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
    statuses.swap(deviceStatusQueue_);
  }

  for (const auto& queuedStatus : statuses) {
    if (!workspaceIsCurrent(queuedStatus.workspaceGeneration)) continue;
    const auto& status = queuedStatus.report;
    deviceLastSeen_ = time(nullptr);
    deviceFirmwareVersion_ = status.firmwareVersion;
    deviceRssi_ = status.rssi;
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
}

}  // namespace inventatory
