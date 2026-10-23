// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Mouser credential test and inventory refresh.

#include "App.h"

#include "app/common/AppActionSupport.h"
#include "platform/mouser/MouserApi.h"
#include "platform/security/CredentialStore.h"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <utility>

namespace inventatory {
using namespace std;

bool App::testStagedMouser() {
  if (mouserTestFuture_.valid()) {
    setMessage("Mouser test is already running", 3);
    return false;
  }
  // Test the key staged in the editor; otherwise the saved key, as lookups read it.
  MouserConfig config;
  if (stagedMouserKeyChanged_) {
    config.apiKey = trim(stagedMouserKey_);
  } else {
    config = loadMouserConfig();
    if (!config.valid() &&
        CredentialStore::lookup(kMouserApiKeyCredentialName).status == CredentialReadStatus::Unavailable) {
      setMessage("Secure credential storage is unavailable; the saved Mouser API key cannot be read", 5);
      return false;
    }
  }
  if (!config.valid()) {
    setMessage("Mouser API key is required", 4);
    return false;
  }
  // Exception text could include the key from the request URL, so it is redacted.
  const auto apiKey = config.apiKey;
  return startSupplierTest(
      mouserTestFuture_, "Mouser",
      [config](string& error) {
        MouserApiClient client(config);
        return client.testConnection(&error);
      },
      [apiKey](const string& text) { return redactMouserKey(text, apiKey); });
}

void App::beginMouserRefresh() {
  if (!settings_.mouserEnabled) {
    setMessage("Mouser is off; turn it on in Settings > Vendors", 4);
    return;
  }
  beginSupplierRefresh(
      mouserRefresh_, "Mouser",
      [this]() -> unique_ptr<MouserApiClient> {
        const auto config = loadMouserConfig();
        if (config.valid()) return make_unique<MouserApiClient>(config);
        setMessage("Mouser refresh unavailable: the Mouser API key is not configured", 5);
        return nullptr;
      },
      [](const InventoryItem& item) -> optional<MouserRefreshRequest> {
        const auto partNumber = app_actions::mouserRefreshLookup(item);
        if (partNumber.empty()) return nullopt;
        // An "Unknown" manufacturer does not help tell Mouser's products apart.
        const auto manufacturer = trim(item.manufacturer) == "Unknown" ? string() : trim(item.manufacturer);
        return MouserRefreshRequest{partNumber, manufacturer};
      },
      "No inventory items with a Mouser part number were found");
}

// Shares the refresh queue, so lookups run one at a time and merge on the UI thread. A Mouser bag names no
// Mouser number, so the lookup is by manufacturer part number and the manufacturer on the bag.
bool App::enqueueMouserScanLookup(const string& itemId, const string& partNumber, const string& manufacturer) {
  if (mouserRefresh_.client == nullptr) {
    const auto config = loadMouserConfig();
    if (!config.valid()) {
      setMessage("Mouser lookup skipped: the Mouser API key is not configured", 5, UiMessageSeverity::Warning);
      return false;
    }
    const auto context = currentWorkspaceContext();
    if (context == nullptr) return false;
    mouserRefresh_.stop();  // Clear the previous run's counters; no client is running here.
    mouserRefresh_.client = make_unique<MouserApiClient>(config);
    mouserRefresh_.generation = context->generation;
  }
  mouserRefresh_.queue.emplace_back(itemId, MouserRefreshRequest{partNumber, manufacturer});
  ++mouserRefresh_.total;
  dirty_ = true;
  return true;
}

void App::processMouserRefresh() {
  auto& run = mouserRefresh_;
  if (run.future.valid()) {
    const auto result = run.takeResult();
    if (!result) return;
    if (!workspaceIsCurrent(result->workspaceGeneration)) {
      run.queue.clear();
      run.client.reset();
      return;
    }
    ++run.completed;
    applyCatalogueResult(run, *result, "Mouser", app_actions::mouserRecordConflict,
                         app_actions::mergeMouserMetadata);
    finishScanAutoPrint(result->itemId, result->details.has_value());
    // A reached limit or rejected key would fail the rest the same way; drop the run.
    if (!result->details && (mouserLimitReached(result->error) || mouserKeyRejected(result->error))) {
      run.queue.clear();
    }
    if (run.queue.empty()) {
      run.client.reset();
      finishSupplierRefresh(run.changed, run.lastError, "mouser", "Mouser",
                            "Mouser refresh complete: " + catalogueRefreshCounts(run));
      return;
    }
  }
  const bool started = run.startNext([](MouserApiClient& client, const MouserRefreshRequest& request,
                                         MouserRefreshResult& result) {
    MouserLookupOutcome outcome = MouserLookupOutcome::Failed;
    try {
      result.details = client.lookupByPartNumber(request.partNumber, outcome, &result.error, request.manufacturer);
    } catch (...) {
      // Exception text could include the key from the request URL, so it is not passed on.
      result.details.reset();
      result.error = "Mouser lookup failed unexpectedly";
    }
    result.outcome = outcome;
  });
  if (started) dirty_ = true;
}

}  // namespace inventatory
