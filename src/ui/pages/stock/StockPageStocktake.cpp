// Inventatory - Stock page stocktake workflow.

#include "App.h"

#include "core/parts/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

void App::beginStocktake() {
  if (stocktakeActive_) {
    setMessage("Stocktake is already active", 2);
    return;
  }
  if (store_.items().empty()) {
    setMessage("Add inventory parts before starting a stocktake", 4);
    return;
  }
  closestSearchActive_ = false;
  closestSearchQuery_.clear();
  closestSelectedPosition_ = 0;
  searchQuery_.clear();
  stockDateFilter_ = StockDateFilter::All;
  stockSortOrder_ = StockSortOrder::Az;
  stocktakeCounts_.clear();
  stocktakeSessionId_ = makeId();
  stocktakeActive_ = true;
  stocktakeCommitPending_ = false;
  selectedPosition_ = 0;
  inputMode_ = InputMode::None;
  focusedTarget_ = -1;
  syncSelectionToFilter();
  setMessage("Stocktake started · count every part, then press S to finish", 5);
  dirty_ = true;
}

void App::beginStocktakeCount() {
  if (!stocktakeActive_) return;
  const auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No part selected for counting", 2);
    return;
  }
  inputBuffer_.clear();
  inputMode_ = InputMode::StocktakeCount;
  setMessage("Enter the physical count for " + item->partName, 4);
  dirty_ = true;
}

void App::handleStocktakeCountKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (isdigit(static_cast<unsigned char>(key.ch)) != 0 && inputBuffer_.size() < 10U) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) inputBuffer_.pop_back();
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Physical count cancelled", 2);
    return;
  }
  if (key.type != KeyType::Enter) return;

  if (inputBuffer_.empty()) {
    setMessage("Enter a whole number from 0 to 2147483647", 4);
    return;
  }
  long long parsed = -1;
  try {
    size_t consumed = 0;
    parsed = stoll(inputBuffer_, &consumed);
    if (consumed != inputBuffer_.size() || parsed < 0 || parsed > numeric_limits<int>::max()) parsed = -1;
  } catch (...) {
    parsed = -1;
  }
  if (parsed < 0) {
    setMessage("Count must be a whole number from 0 to 2147483647", 4);
    return;
  }

  auto* item = selectedItem();
  if (item == nullptr) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("The selected part is no longer available", 3);
    return;
  }
  stocktakeCounts_[item->id] = static_cast<int>(parsed);
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  const auto itemName = item->partName;
  moveSelection(1);
  setMessage(itemName + " counted as " + to_string(parsed), 3);
  dirty_ = true;
}

void App::cancelStocktake() {
  if (!stocktakeActive_) return;
  const bool hadPendingCommit = stocktakeCommitPending_;
  bool revertedUnsavedChanges = false;
  bool activitySaveFailed = false;
  if (stocktakeCommitPending_ && !pendingMovementSource_.empty() && undoSnapshot_.valid) {
    store_.items() = undoSnapshot_.items;
    store_.racks() = undoSnapshot_.racks;
    activities_ = undoSnapshot_.activities;
    const bool activitiesSaved = saveActivitiesChecked(false);
    activitySaveFailed = !activitiesSaved;
    undoSnapshot_.valid = false;
    pendingMovementSource_.clear();
    pendingMovementReference_.clear();
    if (activitiesSaved) persistenceError_.clear();
    revertedUnsavedChanges = true;
  }
  stocktakeActive_ = false;
  stocktakeCommitPending_ = false;
  stocktakeSessionId_.clear();
  stocktakeCounts_.clear();
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  setMessage(activitySaveFailed
                 ? "Stocktake cancelled; inventory changes were discarded, but activity history was not saved; press R to retry"
                 : revertedUnsavedChanges ? "Stocktake cancelled; unsaved inventory changes were discarded"
                                          : hadPendingCommit ? "Stocktake closed; inventory changes were already saved"
                                                             : "Stocktake cancelled; inventory was not changed",
             activitySaveFailed ? 6 : 4);
  dirty_ = true;
}

void App::finishStocktake() {
  if (!stocktakeActive_) return;
  if (stocktakeCountedItems() != store_.items().size()) {
    setMessage("Count every part before finishing the stocktake", 4);
    return;
  }

  if (!stocktakeCommitPending_) {
    captureUndoSnapshot();
    const auto now = time(nullptr);
    int changed = 0;
    for (auto& item : store_.items()) {
      const auto count = stocktakeCounts_.find(item.id);
      if (count == stocktakeCounts_.end() || count->second == item.quantity) continue;
      item.quantity = count->second;
      item.lastUpdated = now;
      reconcileRackAssignment(store_, item);
      ++changed;
    }
    logActivity("stocktake", "Physical count completed · " + to_string(changed) + " parts adjusted");
  }

  if (!saveState("stocktake", stocktakeSessionId_)) {
    stocktakeCommitPending_ = true;
    setMessage("Stocktake changes are in memory; press R to retry saving or Q to discard", 6);
    dirty_ = true;
    return;
  }
  const auto countedParts = stocktakeCountedItems();
  stocktakeActive_ = false;
  stocktakeCommitPending_ = false;
  stocktakeSessionId_.clear();
  stocktakeCounts_.clear();
  inputMode_ = InputMode::None;
  setMessage("Stocktake saved; " + to_string(countedParts) + " parts counted", 5);
  dirty_ = true;
}

int App::stocktakeCountFor(const InventoryItem& item) const {
  const auto it = stocktakeCounts_.find(item.id);
  return it == stocktakeCounts_.end() ? item.quantity : it->second;
}

size_t App::stocktakeCountedItems() const {
  return stocktakeCounts_.size();
}

}  // namespace inventatory
