// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - scanner queueing and DigiKey enrichment workflows.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <memory>
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

void App::queueScanAutoPrint(const string& itemId) {
  if (!autoPrintScannedLabels_) return;
  scanAutoPrintPending_.push_back(itemId);
}

// Runs once the lookup for a created part has finished, whether or not it found the part. A part that is still
// a placeholder is not printed: its label would say "Unsorted" and "Scanned ... Item" and nothing else.
void App::finishScanAutoPrint(const string& itemId, bool detailsFound) {
  const auto pending = find(scanAutoPrintPending_.begin(), scanAutoPrintPending_.end(), itemId);
  if (pending == scanAutoPrintPending_.end()) return;
  scanAutoPrintPending_.erase(pending);
  if (!autoPrintScannedLabels_) return;

  const auto* item = store_.findById(itemId);
  if (item == nullptr) return;

  if (!detailsFound || app_actions::isScanPlaceholderName(item->partName)) {
    const string message = "Auto label skipped: supplier details were not found for the scanned part";
    setMessage(message, 6, UiMessageSeverity::Warning);
    logActivity("scan", message);
    dirty_ = true;
    return;
  }
  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("Auto label skipped: no printer configured", 4);
    dirty_ = true;
    return;
  }

  printLabelForItem(*item, "Auto-printed label for ", false);
}

void App::processScanDigiKeyEnrichment() {
  if (scanDigiKeyEnrichmentFuture_.valid()) {
    if (scanDigiKeyEnrichmentFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
    const auto result = scanDigiKeyEnrichmentFuture_.get();
    if (!workspaceIsCurrent(result.workspaceGeneration)) {
      return;
    }
    if (result.details) {
      if (auto* item = store_.findById(result.itemId); item != nullptr) {
        // A label printed next must show the slot the merged part is placed in; saving would place it too late.
        const bool merged = mergeDigiKeyMetadata(*item, *result.details);
        const bool placed = reconcileRackAssignment(store_, *item);
        if (merged) logActivity("scan", "Synced DigiKey metadata for " + item->partName);
        if (merged || placed) saveState("digikey", result.itemId, "DigiKey enrichment");
      }
    } else if (!result.error.empty()) {
      // A workspace without DigiKey credentials stays silent; only a lookup that was attempted and failed is shown.
      const auto shortError = result.error.size() > 80 ? result.error.substr(0, 77) + "..." : result.error;
      const auto message = "DigiKey lookup failed for a scanned part: " + shortError;
      setMessage(message, 5, UiMessageSeverity::Warning);
      logActivity("scan", message);
      dirty_ = true;
    }
    finishScanAutoPrint(result.itemId, result.details.has_value());
  }
  if (scanDigiKeyEnrichmentQueue_.empty()) return;
  const auto [itemId, lookup] = scanDigiKeyEnrichmentQueue_.front();
  scanDigiKeyEnrichmentQueue_.pop_front();
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;
  const auto generation = context->generation;
  scanDigiKeyEnrichmentFuture_ = async(launch::async, [itemId, lookup, generation] {
    ScanDigiKeyEnrichmentResult result;
    result.itemId = itemId;
    result.workspaceGeneration = generation;
    // Reading the credentials can block on a locked keyring, so it happens here and not on the UI thread.
    const auto config = loadDigiKeyConfig();
    if (!config.valid()) return result;
    DigiKeyApiClient client(config);
    result.details = client.fetchProductDetails(lookup, &result.error);
    if (!result.details && result.error.empty()) {
      result.error = "DigiKey lookup failed";
    }
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

  stopDigiKeyRefresh();  // no refresh is running here, so this only resets the previous run's state
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
      return;
    }
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

}  // namespace inventatory
