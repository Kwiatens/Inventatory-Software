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
  // A value staged in this editor is tested; otherwise the saved value is read from the credential store.
  if (stagedTmeTokenChanged_) {
    config.token = stagedTmeToken_;
  } else {
    const auto stored = CredentialStore::lookup(kTmeTokenCredentialName);
    if (stored.status == CredentialReadStatus::Found && stored.secret.has_value()) {
      config.token = *stored.secret;
    } else if (stored.status == CredentialReadStatus::Unavailable) {
      setMessage("Secure credential storage is unavailable; the saved TME token cannot be read", 5);
      return false;
    }
  }
  if (stagedTmeSecretChanged_) {
    config.applicationSecret = stagedTmeSecret_;
  } else {
    const auto stored = CredentialStore::lookup(kTmeSecretCredentialName);
    if (stored.status == CredentialReadStatus::Found && stored.secret.has_value()) {
      config.applicationSecret = *stored.secret;
    } else if (stored.status == CredentialReadStatus::Unavailable) {
      setMessage("Secure credential storage is unavailable; the saved TME application secret cannot be read", 5);
      return false;
    }
  }
  if (!config.valid()) {
    setMessage("TME token and application secret are required", 4);
    return false;
  }
  const auto context = currentWorkspaceContext();
  const auto generation = context != nullptr ? context->generation : 0;
  try {
    tmeTestFuture_ = std::async(
        std::launch::async,
        [config = std::move(config), generation]() -> TmeTestResult {
          TmeTestResult result;
          result.generation = generation;
          try {
            TmeApiClient client(std::move(config));
            result.ok = client.testConnection(&result.error);
          } catch (const std::exception& e) {
            result.ok = false;
            result.error = e.what();
          } catch (...) {
            result.ok = false;
            result.error = "Connection test failed unexpectedly";
          }
          return result;
        });
  } catch (...) {
    setMessage("Unable to start TME test", 4, UiMessageSeverity::Error);
    return false;
  }
  setMessage("Testing TME credentials...", 10);
  dirty_ = true;
  return true;
}

void App::processTmeTest() {
  if (!tmeTestFuture_.valid() || tmeTestFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
    return;
  }
  const auto result = tmeTestFuture_.get();
  if (workspaceIsCurrent(result.generation)) {
    setMessage(result.ok ? "TME credentials are valid" : "TME test failed: " + result.error, 5,
               result.ok ? UiMessageSeverity::Success : UiMessageSeverity::Error);
    dirty_ = true;
  }
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

  stopTmeRefresh();  // no refresh is running here, so this only resets the previous run's state
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

// Queues the TME symbol lookup for a part that a TME label scan created or matched. It shares the refresh queue,
// so the lookups run one at a time and merge on the UI thread like a manual refresh.
bool App::enqueueTmeScanLookup(const string& itemId, const string& symbol) {
  if (tmeRefreshClient_ == nullptr) {
    const auto config = loadTmeConfig();
    if (!config.valid()) {
      setMessage("TME lookup skipped: TME credentials are not configured", 5, UiMessageSeverity::Warning);
      return false;
    }
    const auto context = currentWorkspaceContext();
    if (context == nullptr) return false;
    stopTmeRefresh();  // no client is running here, so this only clears the previous run's counters
    tmeRefreshClient_ = make_unique<TmeApiClient>(config);
    tmeRefreshGeneration_ = context->generation;
  }
  tmeRefreshQueue_.emplace_back(itemId, TmeIdentifier{true, symbol});
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
      } else {
        if (app_actions::mergeTmeMetadata(*item, *result.details)) tmeRefreshChanged_ = true;
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
                                           : client->lookupByPartNumber(identifier.value, outcome, &result.error);
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
