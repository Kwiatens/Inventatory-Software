// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - inventory search, edit, quantity, and activity actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "core/inventory/InventoryMerge.h"
#include "import/csv/CsvFormat.h"
#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <ctime>
#include <limits>
#include <memory>
#include <system_error>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::startSearch() {
  page_ = Page::Stock;
  closestSearchActive_ = false;
  closestSearchQuery_.clear();
  closestSelectedPosition_ = 0;
  inputMode_ = InputMode::Search;
  searchQueryBeforeEdit_ = searchQuery_;
  inputBuffer_ = searchQuery_;
  setMessage("Type to filter immediately; Enter keeps it, Esc restores the previous filter", 3);
}

void App::startClosestSearch() {
  page_ = Page::Stock;
  closestSearchActive_ = true;
  inputMode_ = InputMode::ClosestSearch;
  inputBuffer_ = closestSearchQuery_;
  closestSelectedPosition_ = 0;
  setMessage("Enter a physical value; results are ranked across the whole database", 4);
  dirty_ = true;
}

void App::clearClosestSearch() {
  closestSearchActive_ = false;
  closestSearchQuery_.clear();
  closestSelectedPosition_ = 0;
  if (inputMode_ == InputMode::ClosestSearch) inputMode_ = InputMode::None;
  inputBuffer_.clear();
  syncSelectionToFilter();
  dirty_ = true;
}

void App::cancelInput() {
  inputMode_ = InputMode::None;
  inputBuffer_.clear();
  dirty_ = true;
}

void App::adjustQuantity(int delta) {
  auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2, UiMessageSeverity::Warning);
    return;
  }

  captureUndoSnapshot();
  const auto candidate = static_cast<long long>(item->quantity) + delta;
  item->quantity = static_cast<int>(clamp<long long>(candidate, 0, numeric_limits<int>::max()));
  item->lastUpdated = time(nullptr);
  logActivity(delta > 0 ? "stock" : "usage", item->partName + " quantity changed to " + to_string(item->quantity));
  const bool saved = saveState();
  if (saved) {
    setMessage(item->partName + " quantity is now " + to_string(item->quantity), 2, UiMessageSeverity::Success);
  } else {
    setMessage("Quantity changed in memory; press R to retry saving", 5, UiMessageSeverity::Error);
  }
  dirty_ = true;
}

void App::setSelectedQuantityFromInput(const string& value) {
  auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2, UiMessageSeverity::Warning);
    return;
  }
  if (value.empty()) {
    setMessage("Enter a quantity from 0 to 2147483647", 4, UiMessageSeverity::Warning);
    return;
  }

  long long parsed = -1;
  try {
    size_t consumed = 0;
    parsed = stoll(value, &consumed);
    if (consumed != value.size() || parsed < 0 || parsed > numeric_limits<int>::max()) parsed = -1;
  } catch (...) {
    parsed = -1;
  }
  if (parsed < 0) {
    setMessage("Quantity must be a whole number from 0 to 2147483647", 4, UiMessageSeverity::Warning);
    return;
  }
  if (item->quantity == parsed) {
    setMessage("Quantity unchanged", 2);
    return;
  }

  captureUndoSnapshot();
  const auto previous = item->quantity;
  item->quantity = static_cast<int>(parsed);
  item->lastUpdated = time(nullptr);
  logActivity(parsed > previous ? "stock" : "usage",
              item->partName + " quantity set to " + to_string(item->quantity));
  const bool saved = saveState();
  if (saved) {
    setMessage(item->partName + " quantity set to " + to_string(item->quantity), 2, UiMessageSeverity::Success);
  } else {
    setMessage("Quantity changed in memory; press R to retry saving", 5, UiMessageSeverity::Error);
  }
  dirty_ = true;
}

void App::logActivity(const string& kind, const string& message) {
  appendActivity(activities_, makeActivity(kind, message));
  saveActivitiesChecked();
  dirty_ = true;
}

}  // namespace inventatory
