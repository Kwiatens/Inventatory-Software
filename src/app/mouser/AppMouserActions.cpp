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
  if (!mouserRefreshQueue_.empty() || mouserRefreshFuture_.valid()) {
    setMessage("Mouser inventory refresh is already running", 3);
    return;
  }
  if (settingsDirty_) {
    setMessage("Save Mouser settings before refreshing inventory data", 4);
    return;
  }
  const auto config = loadMouserConfig();
  if (!config.valid()) {
    setMessage("Mouser refresh unavailable: the Mouser API key is not configured", 5);
    return;
  }

  stopMouserRefresh();  // Resets the previous run's state.
  mouserRefreshClient_ = make_unique<MouserApiClient>(config);
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    stopMouserRefresh();
    setMessage("Mouser refresh unavailable while the workspace is changing", 5);
    return;
  }
  mouserRefreshGeneration_ = context->generation;
  for (const auto& item : store_.items()) {
    const auto partNumber = app_actions::mouserRefreshLookup(item);
    if (partNumber.empty()) continue;
    // An "Unknown" manufacturer does not help tell Mouser's products apart.
    const auto manufacturer = trim(item.manufacturer) == "Unknown" ? string() : trim(item.manufacturer);
    mouserRefreshQueue_.push_back({item.id, partNumber, manufacturer});
  }
  mouserRefreshTotal_ = mouserRefreshQueue_.size();
  if (mouserRefreshTotal_ == 0) {
    mouserRefreshClient_.reset();
    setMessage("No inventory items with a Mouser part number were found", 5);
    return;
  }

  setMessage("Refreshing Mouser data for " + to_string(mouserRefreshTotal_) + " inventory items", 8);
  dirty_ = true;
}

void App::processMouserRefresh() {
  if (mouserRefreshFuture_.valid()) {
    if (mouserRefreshFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
      return;
    }

    auto result = mouserRefreshFuture_.get();
    if (!workspaceIsCurrent(result.workspaceGeneration)) {
      mouserRefreshQueue_.clear();
      mouserRefreshClient_.reset();
      return;
    }
    ++mouserRefreshCompleted_;

    if (result.details) {
      auto* item = store_.findById(result.itemId);
      if (item == nullptr) {
        ++mouserRefreshErrors_;
        mouserRefreshLastError_ = "An inventory item disappeared during refresh";
      } else if (const auto conflict = app_actions::mouserRecordConflict(*item, *result.details); !conflict.empty()) {
        // A different part: nothing from this record is merged.
        ++mouserRefreshErrors_;
        mouserRefreshLastError_ =
            "Mouser's " + conflict + " for " + item->partName + " disagrees with the stored part; not merged";
        logActivity("sync", mouserRefreshLastError_);
      } else {
        if (app_actions::mergeMouserMetadata(*item, *result.details)) mouserRefreshChanged_ = true;
        // Reconcile now, so a label printed next shows the part's merged slot.
        if (reconcileRackAssignment(store_, *item)) mouserRefreshChanged_ = true;
        ++mouserRefreshSucceeded_;
      }
    } else {
      switch (result.outcome) {
        case MouserLookupOutcome::NoMatch:
          ++mouserRefreshNotFound_;
          break;
        case MouserLookupOutcome::Ambiguous:
          ++mouserRefreshAmbiguous_;
          break;
        default:
          ++mouserRefreshErrors_;
          break;
      }
      mouserRefreshLastError_ = result.error.empty() ? "Mouser returned no product details" : result.error;
      // A reached limit or rejected key would fail the rest the same way; drop the run.
      if (mouserLimitReached(result.error) || mouserKeyRejected(result.error)) mouserRefreshQueue_.clear();
    }

    if (mouserRefreshQueue_.empty()) {
      mouserRefreshClient_.reset();
      if (mouserRefreshChanged_ && !saveState("mouser", "inventory refresh", "Mouser refresh batch")) {
        mouserRefreshLastError_ = persistenceError_;
      }
      const auto summary = "Mouser refresh complete: " + to_string(mouserRefreshSucceeded_) + " updated, " +
                           to_string(mouserRefreshNotFound_) + " not found, " + to_string(mouserRefreshAmbiguous_) +
                           " ambiguous, " + to_string(mouserRefreshErrors_) + " errors";
      logActivity("sync", summary);
      setMessage(summary, 8);
      dirty_ = true;
      return;
    }
  }

  if (mouserRefreshQueue_.empty() || mouserRefreshClient_ == nullptr) {
    return;
  }

  const auto request = mouserRefreshQueue_.front();
  mouserRefreshQueue_.pop_front();
  auto* client = mouserRefreshClient_.get();
  mouserRefreshFuture_ = async(launch::async, [client, request, generation = mouserRefreshGeneration_] {
    MouserRefreshResult result;
    result.itemId = request.itemId;
    result.workspaceGeneration = generation;
    MouserLookupOutcome outcome = MouserLookupOutcome::Failed;
    try {
      result.details = client->lookupByPartNumber(request.partNumber, outcome, &result.error, request.manufacturer);
    } catch (...) {
      // Exception text could include the key from the request URL, so it is not passed on.
      result.details.reset();
      result.error = "Mouser lookup failed unexpectedly";
    }
    result.outcome = outcome;
    return result;
  });
  dirty_ = true;
}

void App::stopMouserRefresh() {
  mouserRefreshQueue_.clear();
  if (mouserRefreshFuture_.valid()) {
    mouserRefreshFuture_.wait();
    mouserRefreshFuture_ = {};
  }
  mouserRefreshClient_.reset();
  mouserRefreshTotal_ = 0;
  mouserRefreshCompleted_ = 0;
  mouserRefreshSucceeded_ = 0;
  mouserRefreshNotFound_ = 0;
  mouserRefreshAmbiguous_ = 0;
  mouserRefreshErrors_ = 0;
  mouserRefreshChanged_ = false;
  mouserRefreshGeneration_ = 0;
  mouserRefreshLastError_.clear();
}

}  // namespace inventatory
