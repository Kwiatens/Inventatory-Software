// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Mouser credential test.

#include "App.h"

#include "platform/mouser/MouserApi.h"
#include "platform/security/CredentialStore.h"

#include <chrono>
#include <exception>
#include <future>
#include <utility>

namespace inventatory {
using namespace std;

bool App::testStagedMouser() {
  if (mouserTestFuture_.valid()) {
    setMessage("Mouser test is already running", 3);
    return false;
  }
  // A key staged in this editor is tested; otherwise the saved key is read the way lookups read it.
  MouserConfig config;
  if (stagedMouserKeyChanged_) {
    config.apiKey = stagedMouserKey_;
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
  const auto context = currentWorkspaceContext();
  const auto generation = context != nullptr ? context->generation : 0;
  try {
    mouserTestFuture_ = std::async(
        std::launch::async,
        [config = std::move(config), generation]() -> MouserTestResult {
          MouserTestResult result;
          result.generation = generation;
          try {
            MouserApiClient client(config);
            result.ok = client.testConnection(&result.error);
          } catch (const std::exception& e) {
            result.ok = false;
            // The key travels in the request URL, so an exception text could carry it.
            result.error = redactMouserKey(e.what(), config.apiKey);
          } catch (...) {
            result.ok = false;
            result.error = "Connection test failed unexpectedly";
          }
          return result;
        });
  } catch (...) {
    setMessage("Unable to start Mouser test", 4, UiMessageSeverity::Error);
    return false;
  }
  setMessage("Testing Mouser credentials...", 10);
  dirty_ = true;
  return true;
}

void App::processMouserTest() {
  if (!mouserTestFuture_.valid() || mouserTestFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
    return;
  }
  const auto result = mouserTestFuture_.get();
  if (workspaceIsCurrent(result.generation)) {
    setMessage(result.ok ? "Mouser credentials are valid" : "Mouser test failed: " + result.error, 5,
               result.ok ? UiMessageSeverity::Success : UiMessageSeverity::Error);
    dirty_ = true;
  }
}

}  // namespace inventatory
