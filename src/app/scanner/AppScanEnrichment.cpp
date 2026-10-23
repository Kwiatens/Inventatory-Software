// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - scanner queueing and DigiKey enrichment workflows.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/storage/InventorySqlite.h"
#include "core/vendor/VendorRateLimits.h"
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

// Runs when a created part's lookup ends. A part still holding placeholder names is not printed.
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
        // Reconcile now, so a label printed next shows the merged part's slot.
        const bool merged = mergeDigiKeyMetadata(*item, *result.details);
        const bool placed = reconcileRackAssignment(store_, *item);
        if (merged) logActivity("scan", "Synced DigiKey metadata for " + item->partName);
        if (merged || placed) saveState("digikey", result.itemId, "DigiKey enrichment");
      }
    } else if (!result.error.empty()) {
      // Silent without DigiKey credentials; only an attempted, failed lookup is shown.
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
    // Credential reads can block on a locked keyring, so they run off the UI thread.
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
  beginSupplierRefresh(
      digiKeyRefresh_, "DigiKey",
      [this]() -> unique_ptr<DigiKeyApiClient> {
        auto api = createDigiKeyApi();
        if (api.client == nullptr) setMessage("DigiKey refresh unavailable: " + api.error, 5);
        return move(api.client);
      },
      [](const InventoryItem& item) -> optional<string> {
        auto lookup = digiKeyRefreshLookup(item);
        if (lookup.empty()) return nullopt;
        return lookup;
      },
      "No inventory items with a DigiKey identifier were found");
}

void App::processDigiKeyRefresh() {
  auto& run = digiKeyRefresh_;
  if (run.future.valid()) {
    const auto result = run.takeResult();
    if (!result) return;
    if (!workspaceIsCurrent(result->workspaceGeneration)) {
      run.queue.clear();
      run.client.reset();
      return;
    }
    ++run.completed;
    if (result->details) {
      auto* item = store_.findById(result->itemId);
      if (item == nullptr) {
        ++run.errors;
        run.lastError = "An inventory item disappeared during refresh";
      } else {
        if (mergeDigiKeyMetadata(*item, *result->details)) run.changed = true;
        ++run.succeeded;
      }
    } else {
      ++run.errors;
      run.lastError = result->error.empty() ? "DigiKey returned no product details" : result->error;
      // A reached limit would fail the rest the same way; drop the run.
      if (isVendorLimitMessage(result->error)) run.queue.clear();
    }
    if (run.queue.empty()) {
      run.client.reset();
      finishSupplierRefresh(run.changed, run.lastError, "digikey", "DigiKey",
                            "DigiKey refresh complete: " + to_string(run.succeeded) + " updated, " +
                                to_string(run.errors) + " failed");
      return;
    }
  }
  const bool started =
      run.startNext([](DigiKeyApiClient& client, const string& lookup, DigiKeyRefreshResult& result) {
        if (const auto details = client.fetchProductDetails(lookup, &result.error); details) {
          result.details = *details;
        }
      });
  if (started) dirty_ = true;
}

void App::finishSupplierRefresh(bool changed, string& lastError, const string& source, const string& supplier,
                                const string& summary) {
  if (changed && !saveState(source, "inventory refresh", supplier + " refresh batch")) {
    lastError = persistenceError_;
  }
  logActivity("sync", summary);
  setMessage(summary, 8);
  dirty_ = true;
}

}  // namespace inventatory
