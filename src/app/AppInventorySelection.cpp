// Inventatory - inventory selection, search, messaging, and undo actions.

#include "App.h"
#include "app/AppActionSupport.h"

#include "core/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>
#include <string>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::captureUndoSnapshot() {
  undoSnapshot_.items = store_.items();
  undoSnapshot_.racks = store_.racks();
  undoSnapshot_.activities = activities_;
  undoSnapshot_.selectedPosition = selectedPosition_;
  undoSnapshot_.valid = true;
}

bool App::undoLastInventoryChange() {
  if (persistedStoreValid_ && !inventoryCommitDiff(persistedStore_, store_).empty()) {
    setMessage("Save the current inventory before using Ctrl+Z", 4);
    return false;
  }
  if (inventoryCommits_.empty()) refreshInventoryCommits();
  const auto latest = find_if(inventoryCommits_.begin(), inventoryCommits_.end(), [](const InventoryCommit& commit) {
    return !commit.checkpoint && (commit.changedItemCount > 0 || commit.changedRackCount > 0) &&
           !commit.parentId.empty();
  });
  if (latest == inventoryCommits_.end()) {
    setMessage("Nothing to undo", 3);
    return false;
  }

  InventoryCommitDetail detail;
  if (!loadInventoryCommit(inventoryPath_, latest->id, detail) || !detail.hasParent) {
    setMessage("The latest inventory commit could not be loaded", 5);
    return false;
  }
  store_ = detail.parentSnapshot;
  InventoryCommitDraft draft;
  draft.source = "undo";
  draft.reference = latest->id;
  draft.corrective = true;
  draft.revertedCommitId = latest->id;
  draft.message = "Undid commit #" + to_string(latest->sequence);
  const bool saved = saveInventoryState(draft);
  syncSelectionToFilter();
  syncRackSelection();
  if (saved) setMessage("Undid commit #" + to_string(latest->sequence), 4);
  return saved;
}

void App::setMessage(string text, int seconds) {
  message_ = move(text);
  messageUntil_ = time(nullptr) + seconds;
  messageFlashStartedAt_ = uiAnimationTicks();
  dirty_ = true;
}

bool App::messageVisible() const {
  return !message_.empty() && time(nullptr) <= messageUntil_;
}

void App::clearMessageIfExpired() {
  if (!messageVisible() && !message_.empty()) {
    message_.clear();
    dirty_ = true;
  }
}

void App::markDirty() {
  dirty_ = true;
}

string App::stockDateFilterName(StockDateFilter filter) const {
  switch (filter) {
    case StockDateFilter::All:
      return "All modification dates";
    case StockDateFilter::Today:
      return "Modified today";
    case StockDateFilter::Last7Days:
      return "Modified in the last 7 days";
    case StockDateFilter::Last30Days:
      return "Modified in the last 30 days";
    case StockDateFilter::OlderThan30Days:
      return "Modified over 30 days ago";
  }
  return "All modification dates";
}

bool App::stockDateFilterMatches(const InventoryItem& item) const {
  if (stockDateFilter_ == StockDateFilter::All) return true;
  if (item.lastUpdated == 0) return false;

  const auto age = difftime(time(nullptr), item.lastUpdated);
  constexpr double day = 24.0 * 60.0 * 60.0;
  switch (stockDateFilter_) {
    case StockDateFilter::Today:
      return age >= 0.0 && age < day;
    case StockDateFilter::Last7Days:
      return age >= 0.0 && age < 7.0 * day;
    case StockDateFilter::Last30Days:
      return age >= 0.0 && age < 30.0 * day;
    case StockDateFilter::OlderThan30Days:
      return age >= 30.0 * day;
    case StockDateFilter::All:
      return true;
  }
  return true;
}

vector<InventorySearchMatch> App::stockSearchMatches() const {
  auto matches = closestSearchActive_
                     ? findClosestPhysicalValues(store_.items(), closestSearchQuery_)
                     : rankedFilterItems(store_.items(), searchQuery_, store_.racks(), settings_.lowStockThreshold);

  if (closestSearchActive_) return matches;

  matches.erase(remove_if(matches.begin(), matches.end(), [&](const InventorySearchMatch& match) {
                  return !stockDateFilterMatches(store_.items()[match.itemIndex]);
                }),
                matches.end());

  const bool valueRanked = any_of(matches.begin(), matches.end(), [](const InventorySearchMatch& match) {
    return match.hasPhysicalComparison;
  });
  const auto bandRank = [](PhysicalValueMatchBand band) {
    switch (band) {
      case PhysicalValueMatchBand::Exact: return 0;
      case PhysicalValueMatchBand::Workable: return 1;
      case PhysicalValueMatchBand::Possible: return 2;
      case PhysicalValueMatchBand::None: return 3;
    }
    return 3;
  };
  const auto fallbackLess = [&](size_t lhs, size_t rhs) {
    const auto& left = store_.items()[lhs];
    const auto& right = store_.items()[rhs];
    if (stockSortOrder_ == StockSortOrder::Quantity && left.quantity != right.quantity) {
      return left.quantity > right.quantity;
    }
    if (stockSortOrder_ != StockSortOrder::Quantity) {
      const auto leftCategory = toLower(displayCategory(left.category));
      const auto rightCategory = toLower(displayCategory(right.category));
      if (leftCategory != rightCategory) {
        return stockSortOrder_ == StockSortOrder::Za ? leftCategory > rightCategory : leftCategory < rightCategory;
      }
    }
    const auto leftName = toLower(left.partName);
    const auto rightName = toLower(right.partName);
    if (leftName != rightName) {
      return stockSortOrder_ == StockSortOrder::Za ? leftName > rightName : leftName < rightName;
    }
    return left.id < right.id;
  };

  sort(matches.begin(), matches.end(), [&](const InventorySearchMatch& lhs, const InventorySearchMatch& rhs) {
    if (valueRanked) {
      const auto leftBand = bandRank(lhs.band);
      const auto rightBand = bandRank(rhs.band);
      if (leftBand != rightBand) return leftBand < rightBand;
      if (lhs.hasPhysicalComparison && rhs.hasPhysicalComparison &&
          lhs.relativeDifference != rhs.relativeDifference) {
        return lhs.relativeDifference < rhs.relativeDifference;
      }
    }
    return fallbackLess(lhs.itemIndex, rhs.itemIndex);
  });
  return matches;
}

vector<size_t> App::filteredIndices() const {
  vector<size_t> indices;
  const auto matches = stockSearchMatches();
  indices.reserve(matches.size());
  for (const auto& match : matches) indices.push_back(match.itemIndex);
  return indices;
}

size_t App::selectedIndex() const {
  const auto matches = stockSearchMatches();
  if (matches.empty()) {
    return numeric_limits<size_t>::max();
  }
  const auto position = closestSearchActive_ ? min(closestSelectedPosition_, matches.size() - 1)
                                             : min(selectedPosition_, matches.size() - 1);
  return matches[position].itemIndex;
}

InventoryItem* App::selectedItem() {
  const auto index = selectedIndex();
  if (index == numeric_limits<size_t>::max()) {
    return nullptr;
  }
  return &store_.items()[index];
}

const InventoryItem* App::selectedItem() const {
  const auto index = selectedIndex();
  if (index == numeric_limits<size_t>::max()) {
    return nullptr;
  }
  return &store_.items()[index];
}

void App::syncSelectionToFilter() {
  const auto matches = stockSearchMatches();
  if (matches.empty()) {
    if (closestSearchActive_) closestSelectedPosition_ = 0;
    else selectedPosition_ = 0;
    return;
  }
  if (closestSearchActive_) {
    if (closestSelectedPosition_ >= matches.size()) closestSelectedPosition_ = matches.size() - 1;
  } else if (selectedPosition_ >= matches.size()) {
    selectedPosition_ = matches.size() - 1;
  }
  dirty_ = true;
}

void App::moveSelection(int delta) {
  const auto matches = stockSearchMatches();
  if (matches.empty()) {
    if (closestSearchActive_) closestSelectedPosition_ = 0;
    else selectedPosition_ = 0;
    return;
  }

  const auto currentPosition = closestSearchActive_ ? closestSelectedPosition_ : selectedPosition_;
  const auto current = static_cast<int>(min(currentPosition, matches.size() - 1));
  const auto next = clamp(current + delta, 0, static_cast<int>(matches.size() - 1));
  if (closestSearchActive_) closestSelectedPosition_ = static_cast<size_t>(next);
  else selectedPosition_ = static_cast<size_t>(next);
  dirty_ = true;
}

bool App::deleteConfirmationActive() const {
  return !deleteConfirmationItemId_.empty();
}

bool App::deleteConfirmationReady() const {
  return deleteConfirmationActive() && time(nullptr) >= deleteConfirmationUntil_;
}

int App::deleteConfirmationSecondsLeft() const {
  if (!deleteConfirmationActive()) {
    return 0;
  }
  return max(0, static_cast<int>(deleteConfirmationUntil_ - time(nullptr)));
}

void App::armDeleteConfirmation() {
  const auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return;
  }

  deleteConfirmationItemId_ = item->id;
  deleteConfirmationUntil_ = time(nullptr) + 3;
  dirty_ = true;
}

void App::cancelDeleteConfirmation() {
  if (deleteConfirmationItemId_.empty()) {
    return;
  }

  deleteConfirmationItemId_.clear();
  deleteConfirmationUntil_ = 0;
  dirty_ = true;
}

void App::clearDeleteConfirmationIfExpired() {
  // Keep the confirmation popup visible after the countdown reaches zero.
}

void App::confirmDeleteSelectedItem() {
  if (!deleteConfirmationActive()) {
    setMessage("Press Ctrl+Backspace first to arm delete", 2);
    return;
  }

  if (!deleteConfirmationReady()) {
    setMessage("Wait " + to_string(deleteConfirmationSecondsLeft()) + " more second" +
                   (deleteConfirmationSecondsLeft() == 1 ? string() : string("s")) + " to confirm delete",
               2);
    return;
  }

  const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& item) {
    return item.id == deleteConfirmationItemId_;
  });
  if (it == store_.items().end()) {
    cancelDeleteConfirmation();
    setMessage("Item no longer available", 2);
    return;
  }

  const auto itemName = it->partName;
  store_.items().erase(it);
  cancelDeleteConfirmation();
  logActivity("delete", itemName + " deleted");
  saveState();
  syncSelectionToFilter();
  page_ = Page::Stock;
  setMessage(itemName + " deleted", 2);
}

}  // namespace inventatory
