// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - TME credential test and inventory refresh.

#include "App.h"

#include "app/common/AppActionSupport.h"
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
  if (!tmeRefreshQueue_.empty() || tmeRefreshFuture_.valid()) {
    setMessage("TME inventory refresh is already running", 3);
    return;
  }
  if (settingsDirty_) {
    setMessage("Save TME settings before refreshing inventory data", 4);
    return;
  }
  const auto config = loadTmeConfig();
  if (!config.valid()) {
    setMessage("TME refresh unavailable: TME credentials are not configured", 5);
    return;
  }

  stopTmeRefresh();  // Clear the previous run's state; nothing is running here.
  tmeRefreshClient_ = make_unique<TmeApiClient>(config);
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    stopTmeRefresh();
    setMessage("TME refresh unavailable while the workspace is changing", 5);
    return;
  }
  tmeRefreshGeneration_ = context->generation;
  for (const auto& item : store_.items()) {
    const auto identifier = app_actions::tmeRefreshIdentifier(item);
    if (!identifier.value.empty()) {
      tmeRefreshQueue_.emplace_back(item.id, identifier);
    }
  }
  tmeRefreshTotal_ = tmeRefreshQueue_.size();
  if (tmeRefreshTotal_ == 0) {
    tmeRefreshClient_.reset();
    setMessage("No inventory items with a TME symbol or part number were found", 5);
    return;
  }

  setMessage("Refreshing TME data for " + to_string(tmeRefreshTotal_) + " inventory items", 8);
  dirty_ = true;
}

// Shares the refresh queue, so lookups run one at a time and merge on the UI thread.
bool App::enqueueTmeScanLookup(const string& itemId, const string& symbol) {
  if (tmeRefreshClient_ == nullptr) {
    const auto config = loadTmeConfig();
    if (!config.valid()) {
      setMessage("TME lookup skipped: TME credentials are not configured", 5, UiMessageSeverity::Warning);
      return false;
    }
    const auto context = currentWorkspaceContext();
    if (context == nullptr) return false;
    stopTmeRefresh();  // Clear the previous run's counters; no client is running here.
    tmeRefreshClient_ = make_unique<TmeApiClient>(config);
    tmeRefreshGeneration_ = context->generation;
  }
  tmeRefreshQueue_.emplace_back(itemId, TmeIdentifier{true, symbol, {}});
  ++tmeRefreshTotal_;
  dirty_ = true;
  return true;
}

void App::processTmeRefresh() {
  if (tmeRefreshFuture_.valid()) {
    if (tmeRefreshFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
      return;
    }

    auto result = tmeRefreshFuture_.get();
    if (!workspaceIsCurrent(result.workspaceGeneration)) {
      tmeRefreshQueue_.clear();
      tmeRefreshClient_.reset();
      return;
    }
    ++tmeRefreshCompleted_;

    if (result.details) {
      auto* item = store_.findById(result.itemId);
      if (item == nullptr) {
        ++tmeRefreshErrors_;
        tmeRefreshLastError_ = "An inventory item disappeared during refresh";
      } else if (const auto conflict = app_actions::tmeRecordConflict(*item, *result.details); !conflict.empty()) {
        // Record describes a different part; merge nothing from it.
        ++tmeRefreshErrors_;
        tmeRefreshLastError_ = "TME's " + conflict + " for " + item->partName + " disagrees with the stored part; not merged";
        logActivity("sync", tmeRefreshLastError_);
      } else {
        if (app_actions::mergeTmeMetadata(*item, *result.details)) tmeRefreshChanged_ = true;
        // Reconcile now so a label printed next shows the merged part's slot.
        if (reconcileRackAssignment(store_, *item)) tmeRefreshChanged_ = true;
        ++tmeRefreshSucceeded_;
      }
    } else {
      switch (result.outcome) {
        case TmeLookupOutcome::NoMatch:
          ++tmeRefreshNotFound_;
          break;
        case TmeLookupOutcome::Ambiguous:
          ++tmeRefreshAmbiguous_;
          break;
        default:
          ++tmeRefreshErrors_;
          break;
      }
      tmeRefreshLastError_ = result.error.empty() ? "TME returned no product details" : result.error;
    }
    finishScanAutoPrint(result.itemId, result.details.has_value());

    if (tmeRefreshQueue_.empty()) {
      tmeRefreshClient_.reset();
      if (tmeRefreshChanged_ && !saveState("tme", "inventory refresh", "TME refresh batch")) {
        tmeRefreshLastError_ = persistenceError_;
      }
      const auto summary = "TME refresh complete: " + to_string(tmeRefreshSucceeded_) + " updated, " +
                           to_string(tmeRefreshNotFound_) + " not found, " + to_string(tmeRefreshAmbiguous_) +
                           " ambiguous, " + to_string(tmeRefreshErrors_) + " errors";
      logActivity("sync", summary);
      setMessage(summary, 8);
      dirty_ = true;
      return;
    }
  }

  if (tmeRefreshQueue_.empty() || tmeRefreshClient_ == nullptr) {
    return;
  }

  const auto entry = tmeRefreshQueue_.front();
  tmeRefreshQueue_.pop_front();
  auto* client = tmeRefreshClient_.get();
  tmeRefreshFuture_ = async(launch::async, [client, itemId = entry.first, identifier = entry.second,
                                            generation = tmeRefreshGeneration_] {
    TmeRefreshResult result;
    result.itemId = itemId;
    result.workspaceGeneration = generation;
    TmeLookupOutcome outcome = TmeLookupOutcome::Failed;
    try {
      result.details = identifier.isSymbol ? client->lookupBySymbol(identifier.value, outcome, &result.error)
                                           : client->lookupByPartNumber(identifier.value, outcome, &result.error,
                                                                        identifier.manufacturer);
    } catch (...) {
      result.details.reset();
      result.error = "TME lookup failed unexpectedly";
    }
    result.outcome = outcome;
    return result;
  });
  dirty_ = true;
}

void App::stopTmeRefresh() {
  tmeRefreshQueue_.clear();
  if (tmeRefreshFuture_.valid()) {
    tmeRefreshFuture_.wait();
    tmeRefreshFuture_ = {};
  }
  tmeRefreshClient_.reset();
  tmeRefreshTotal_ = 0;
  tmeRefreshCompleted_ = 0;
  tmeRefreshSucceeded_ = 0;
  tmeRefreshNotFound_ = 0;
  tmeRefreshAmbiguous_ = 0;
  tmeRefreshErrors_ = 0;
  tmeRefreshChanged_ = false;
  tmeRefreshGeneration_ = 0;
  tmeRefreshLastError_.clear();
}

}  // namespace inventatory
