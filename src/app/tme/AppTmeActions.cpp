// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - TME credential test and inventory refresh.

#include "App.h"
#include "core/inventory/VendorProvider.h"

#include "app/common/AppActionSupport.h"
#include "core/vendor/VendorRateLimits.h"
#include "platform/security/CredentialStore.h"
#include "platform/tme/TmeApi.h"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <utility>

namespace inventatory {
using namespace std;

bool App::testStagedTme() {
  if (tmeTestFuture_.valid()) {
    setMessage("TME test is already running", 3);
    return false;
  }
  TmeConfig config;
  config.country = settingsDraft_.tmeCountry;
  config.language = settingsDraft_.tmeLanguage;
  config.currency = settingsDraft_.tmeCurrency;
  if (!stagedOrStoredSecret(stagedTmeTokenChanged_, stagedTmeToken_, kTmeTokenCredentialName, "TME token",
                            config.token) ||
      !stagedOrStoredSecret(stagedTmeSecretChanged_, stagedTmeSecret_, kTmeSecretCredentialName,
                            "TME application secret", config.applicationSecret)) {
    return false;
  }
  if (!config.valid()) {
    setMessage("TME token and application secret are required", 4);
    return false;
  }
  return startSupplierTest(tmeTestFuture_, "TME", [config](string& error) {
    TmeApiClient client(config);
    return client.testConnection(&error);
  });
}

void App::beginTmeRefresh() {
  if (!settings_.tmeEnabled) {
    setMessage("TME is off; turn it on in Settings > Vendors", 4);
    return;
  }
  beginSupplierRefresh(
      tmeRefresh_, "TME",
      [this]() -> unique_ptr<TmeApiClient> {
        const auto config = loadTmeConfig();
        if (config.valid()) return make_unique<TmeApiClient>(config);
        setMessage("TME refresh unavailable: TME credentials are not configured", 5);
        return nullptr;
      },
      [](const InventoryItem& item) -> optional<TmeIdentifier> {
        auto identifier = app_actions::tmeRefreshIdentifier(item);
        if (identifier.value.empty()) return nullopt;
        return identifier;
      },
      "No inventory items with a TME symbol or part number were found");
}

// Shares the refresh queue, so lookups run one at a time and merge on the UI thread.
bool App::enqueueTmeScanLookup(const string& itemId, const string& symbol) {
  if (!settings_.tmeEnabled) return false;
  if (tmeRefresh_.client == nullptr) {
    const auto config = loadTmeConfig();
    if (!config.valid()) {
      setMessage("TME lookup skipped: TME credentials are not configured", 5, UiMessageSeverity::Warning);
      return false;
    }
    const auto context = currentWorkspaceContext();
    if (context == nullptr) return false;
    tmeRefresh_.stop();  // Clear the previous run's counters; no client is running here.
    tmeRefresh_.client = make_unique<TmeApiClient>(config);
    tmeRefresh_.generation = context->generation;
  }
  tmeRefresh_.queue.emplace_back(itemId, TmeIdentifier{true, symbol, {}});
  ++tmeRefresh_.total;
  dirty_ = true;
  return true;
}

void App::processTmeRefresh() {
  auto& run = tmeRefresh_;
  if (run.future.valid()) {
    const auto result = run.takeResult();
    if (!result) return;
    if (!workspaceIsCurrent(result->workspaceGeneration)) {
      run.queue.clear();
      run.client.reset();
      return;
    }
    ++run.completed;
    applyCatalogueResult(run, *result, "TME", app_actions::tmeRecordConflict, app_actions::mergeTmeMetadata);
    // The lookup already carried the price, so it is saved without another request.
    if (result->details) {
      if (const auto* item = store_.findById(result->itemId); item != nullptr && app_actions::tmeRecordConflict(*item, *result->details).empty()) {
        saveItemOffers(*item, kVendorProviderTme,
                       result->details->offer ? vector<VendorOffer>{*result->details->offer} : vector<VendorOffer>{});
      }
    }
    finishScanAutoPrint(result->itemId, result->details.has_value());
    // A reached limit would fail the rest the same way; drop the run.
    if (!result->details && isVendorLimitMessage(result->error)) run.queue.clear();
    if (run.queue.empty()) {
      run.client.reset();
      finishSupplierRefresh(run.changed, run.lastError, "tme", "TME",
                            "TME refresh complete: " + catalogueRefreshCounts(run));
      return;
    }
  }
  const bool started = run.startNext([](TmeApiClient& client, const TmeIdentifier& identifier,
                                         TmeRefreshResult& result) {
    TmeLookupOutcome outcome = TmeLookupOutcome::Failed;
    try {
      result.details = identifier.isSymbol ? client.lookupBySymbol(identifier.value, outcome, &result.error)
                                           : client.lookupByPartNumber(identifier.value, outcome, &result.error,
                                                                       identifier.manufacturer);
    } catch (...) {
      result.details.reset();
      result.error = "TME lookup failed unexpectedly";
    }
    result.outcome = outcome;
  });
  if (started) dirty_ = true;
}

}  // namespace inventatory
