// Inventatory - Hardware Inventory Management System
// Inventory, rack, editing, selection, and local UI actions.

#include "App.h"
#include "app/AppActionSupport.h"

#include "import/CsvFormat.h"
#include "core/InventorySqlite.h"
#include "platform/DigiKeyApi.h"
#include "platform/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <memory>
#include <sstream>
#include <system_error>
#include <unordered_set>

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

void App::changePage(Page page) {
  if (page != page_ && page_ == Page::Stock && stocktakeActive_) {
    setMessage("Finish or cancel the stocktake before leaving Stock", 5);
    return;
  }
  if (page != page_ && page_ == Page::Import && importCommitPending_) {
    setMessage("Import is not saved yet. Press R to retry or Q to cancel it.", 6);
    return;
  }
  if (page != page_ && page_ == Page::Import && importStageActive_) {
    cancelImportSession();
  }
  if (page != page_ && page_ == Page::Settings && settingsDirty_ && inputMode_ != InputMode::ExitConfirmation) {
    pendingPageAfterSettings_ = page;
    inputMode_ = InputMode::ExitConfirmation;
    setMessage("Unsaved settings: press S to save, D to discard, or Esc to stay", 5);
    return;
  }
  page_ = page;
  inputMode_ = InputMode::None;
  if (page != Page::Stock) {
    closestSearchActive_ = false;
    closestSearchQuery_.clear();
    closestSelectedPosition_ = 0;
  }
  focusedTarget_ = -1;
  cancelDeleteConfirmation();
  if (page != Page::Racks) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
  }
  if (page != Page::Projects) {
    // Leaving the page abandons an in-progress walkthrough rather than letting
    // a half-finished pick resume out of context later.
    bomDeductPrompt_ = false;
    if (bomView_ == BomView::Build) {
      bomView_ = BomView::Split;
      bomBuildStep_ = 0;
    }
    bomDeleteConfirmationProjectId_.clear();
    bomDeleteConfirmationUntil_ = 0;
  }
  dirty_ = true;
}

void App::openSelectedDetail() {
  if (selectedItem() != nullptr) {
    page_ = Page::Stock;
    dirty_ = true;
  }
}

void App::openRackManagement() {
  syncRackSelection();
  changePage(Page::Racks);
  setMessage(store_.racks().empty() ? "No Inventatory racks exist yet; eligible parts create racks automatically"
                                    : "Rack management opened",
             3);
}

vector<size_t> App::sortedRackIndices() const {
  vector<size_t> indices;
  indices.reserve(store_.racks().size());
  for (size_t index = 0; index < store_.racks().size(); ++index) {
    if (!rackFilter_.empty()) {
      const auto& rack = store_.racks()[index];
      const auto filter = toLower(rackFilter_);
      const auto occupied = rackOccupiedSlotCount(store_, rack);
      const auto full = occupied >= static_cast<size_t>(rack.rows * rack.columns);
      const bool matchesSpecial = (filter == "free" && !full) || (filter == "full" && full) ||
                                  (filter == "empty" && occupied == 0);
      if (!matchesSpecial && !containsInsensitive(rack.code, rackFilter_) &&
          !containsInsensitive(rack.componentType, rackFilter_)) {
        continue;
      }
    }
    indices.push_back(index);
  }
  sort(indices.begin(), indices.end(), [&](size_t lhs, size_t rhs) {
    const auto lhsNumber = rackNumberFromCode(store_.racks()[lhs].code);
    const auto rhsNumber = rackNumberFromCode(store_.racks()[rhs].code);
    if (lhsNumber != rhsNumber) return lhsNumber < rhsNumber;
    return store_.racks()[lhs].code < store_.racks()[rhs].code;
  });
  return indices;
}

const InventatoryRack* App::selectedRack() const {
  const auto indices = sortedRackIndices();
  if (indices.empty()) return nullptr;
  const auto position = min(rackSelection_, indices.size() - 1);
  return &store_.racks()[indices[position]];
}

InventatoryRack* App::selectedRack() {
  const auto indices = sortedRackIndices();
  if (indices.empty()) return nullptr;
  const auto position = min(rackSelection_, indices.size() - 1);
  return &store_.racks()[indices[position]];
}

string App::selectedRackSlot() const {
  return rackSlotLabel(rackRow_, rackColumn_);
}

InventoryItem* App::selectedRackItem() {
  const auto* rack = selectedRack();
  if (rack == nullptr) return nullptr;
  return itemAtRackSlot(store_, rack->id, selectedRackSlot());
}

const InventoryItem* App::selectedRackItem() const {
  const auto* rack = selectedRack();
  if (rack == nullptr) return nullptr;
  return itemAtRackSlot(store_, rack->id, selectedRackSlot());
}

void App::syncRackSelection() {
  const auto indices = sortedRackIndices();
  if (indices.empty()) {
    rackSelection_ = 0;
    rackRow_ = 0;
    rackColumn_ = 0;
    return;
  }
  rackSelection_ = min(rackSelection_, indices.size() - 1);
  rackRow_ = clamp(rackRow_, 0, 4);
  rackColumn_ = clamp(rackColumn_, 0, 4);
  dirty_ = true;
}

void App::renameSelectedRack(const string& value) {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }
  auto code = trim(value);
  transform(code.begin(), code.end(), code.begin(), [](unsigned char ch) { return static_cast<char>(toupper(ch)); });
  if (rackNumberFromCode(code) <= 0 || code != "R" + to_string(rackNumberFromCode(code))) {
    setMessage("Rack code must look like R12", 3);
    return;
  }
  const auto duplicate = find_if(store_.racks().begin(), store_.racks().end(), [&](const InventatoryRack& candidate) {
    return candidate.id != rack->id && toLower(candidate.code) == toLower(code);
  });
  if (duplicate != store_.racks().end()) {
    setMessage(code + " already exists", 3);
    return;
  }
  captureUndoSnapshot();
  const auto previous = rack->code;
  rack->code = code;
  logActivity("rack", previous + " renamed to " + code);
  saveState();
  syncRackSelection();
  setMessage("Rack renamed to " + code, 2);
}

void App::changeSelectedRackType(const string& value) {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }
  const auto type = trim(value);
  if (type.empty()) {
    setMessage("Rack type cannot be empty", 3);
    return;
  }
  captureUndoSnapshot();
  const auto previous = rack->componentType;
  rack->componentType = type;
  logActivity("rack", rack->code + " type " + previous + " -> " + type);
  saveState();
  syncRackSelection();
  setMessage(rack->code + " type updated", 2);
}

void App::createRackWithType(const string& value) {
  const auto type = trim(value);
  if (type.empty()) {
    setMessage("Rack type cannot be empty", 3);
    return;
  }
  int nextNumber = 1;
  for (const auto& rack : store_.racks()) {
    nextNumber = max(nextNumber, rackNumberFromCode(rack.code) + 1);
  }
  InventatoryRack rack;
  rack.id = makeId();
  rack.code = "R" + to_string(nextNumber);
  rack.componentType = type;
  rack.createdAt = time(nullptr);
  captureUndoSnapshot();
  store_.racks().push_back(rack);
  rackFilter_.clear();
  rackSelection_ = sortedRackIndices().empty() ? 0 : sortedRackIndices().size() - 1;
  logActivity("rack", rack.code + " created for " + type);
  saveState();
  syncRackSelection();
  setMessage(rack.code + " created", 2);
}

void App::deleteSelectedRack() {
  const auto indices = sortedRackIndices();
  if (indices.empty()) {
    setMessage("No rack selected", 2);
    return;
  }
  const auto position = min(rackSelection_, indices.size() - 1);
  const auto rackIndex = indices[position];
  const auto& rack = store_.racks()[rackIndex];
  if (rackOccupiedSlotCount(store_, rack) != 0) {
    setMessage("Only empty racks can be deleted", 3);
    return;
  }
  const auto code = rack.code;
  captureUndoSnapshot();
  store_.racks().erase(store_.racks().begin() + static_cast<ptrdiff_t>(rackIndex));
  if (rackSelection_ > 0) --rackSelection_;
  movingRackItemId_.clear();
  movingRackSource_.clear();
  logActivity("rack", code + " deleted");
  saveState();
  syncRackSelection();
  setMessage(code + " deleted", 2);
}

void App::jumpToRack(const string& value) {
  const auto requested = toLower(trim(value));
  if (requested.empty()) {
    setMessage("Enter a rack code like R3", 2);
    return;
  }
  const auto indices = sortedRackIndices();
  for (size_t position = 0; position < indices.size(); ++position) {
    if (toLower(store_.racks()[indices[position]].code) == requested) {
      rackSelection_ = position;
      rackRow_ = 0;
      rackColumn_ = 0;
      setMessage("Jumped to " + store_.racks()[indices[position]].code, 2);
      dirty_ = true;
      return;
    }
  }
  setMessage("Rack not visible/found: " + value, 3);
}

void App::beginRackFilter() {
  inputBuffer_ = rackFilter_;
  inputMode_ = InputMode::RackFilter;
  setMessage("Filter by rack code, type, free, full, or empty", 4);
}

void App::adjustSelectedRackItemQuantity(int delta) {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }

  captureUndoSnapshot();
  item->quantity = max(0, item->quantity + delta);
  item->lastUpdated = time(nullptr);
  logActivity(delta > 0 ? "stock" : "usage", item->partName + " quantity changed to " + to_string(item->quantity));
  saveState();
  setMessage(item->partName + " quantity is now " + to_string(item->quantity), 2);
  dirty_ = true;
}

void App::openSelectedRackItemDetail() {
  const auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }

  const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& candidate) {
    return candidate.id == item->id;
  });
  if (it == store_.items().end()) {
    setMessage("Selected part is no longer available", 2);
    return;
  }

  // The stock selection is position-based, so reset the stock query before
  // selecting a rack part to ensure the detail panel can always show it.
  searchQuery_.clear();
  selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
  inputMode_ = InputMode::None;
  focusedTarget_ = -1;
  page_ = Page::Stock;
  syncSelectionToFilter();
  dirty_ = true;
}

bool App::printSelectedRackPartLabel() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return false;
  }
  const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& candidate) {
    return candidate.id == item->id;
  });
  if (it != store_.items().end()) {
    selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
  }
  return printSelectedLabel();
}

bool App::printSelectedRackLabel() {
  const auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return false;
  }

  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("No printer configured", 3);
    openPrinterSetup();
    return false;
  }

  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Printer unavailable while the workspace is changing", 4);
    return false;
  }
  if (printerWorkCompletion_ != nullptr) {
    setMessage("A printer job is already running; try again shortly", 3);
    return false;
  }

  PrinterWork work;
  work.kind = PrinterWorkKind::PrintRack;
  work.workspaceGeneration = context->generation;
  work.printerName = printerService_.configuredPrinter();
  work.rack = *rack;
  if (!enqueuePrinterWork(move(work))) {
    setMessage("Printer request queue is full; try again shortly", 4);
    return false;
  }
  setMessage("Printer job queued", 3);
  return true;
}

void App::moveRackSlot(int rowDelta, int columnDelta) {
  rackRow_ = clamp(rackRow_ + rowDelta, 0, 4);
  rackColumn_ = clamp(rackColumn_ + columnDelta, 0, 4);
  dirty_ = true;
}

void App::moveRackPage(int delta) {
  const auto indices = sortedRackIndices();
  if (indices.empty()) {
    rackSelection_ = 0;
    return;
  }
  const auto current = static_cast<int>(min(rackSelection_, indices.size() - 1));
  rackSelection_ = static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(indices.size() - 1)));
  movingRackItemId_.clear();
  movingRackSource_.clear();
  dirty_ = true;
}

void App::beginOrCompleteRackMove() {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }

  const auto slot = selectedRackSlot();
  auto* item = itemAtRackSlot(store_, rack->id, slot);
  if (movingRackItemId_.empty()) {
    if (item == nullptr) {
      setMessage("Select an occupied slot to move", 2);
      return;
    }
    movingRackItemId_ = item->id;
    movingRackSource_ = rack->code + "-" + slot;
    setMessage("Moving " + item->partName + "; choose an empty slot and press v", 4);
    dirty_ = true;
    return;
  }

  auto* movingItem = store_.findById(movingRackItemId_);
  if (movingItem == nullptr) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
    setMessage("Moving item no longer exists", 3);
    return;
  }
  if (rackLocation(*movingItem, store_.racks()) == rack->code + "-" + slot) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
    setMessage("Move cancelled", 2);
    dirty_ = true;
    return;
  }
  if (item != nullptr) {
    setMessage(rack->code + "-" + slot + " is already occupied", 3);
    return;
  }

  string error;
  captureUndoSnapshot();
  if (!moveItemToRackSlot(store_, *movingItem, *rack, slot, error)) {
    undoSnapshot_.valid = false;
    setMessage(error, 4);
    return;
  }
  movingItem->lastUpdated = time(nullptr);
  const auto target = rack->code + "-" + slot;
  logActivity("rack", movingItem->partName + " moved " + movingRackSource_ + " -> " + target);
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage("Moved to " + target, 2);
  dirty_ = true;
}

void App::unassignSelectedRackItem() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }
  const auto previous = rackLocation(*item, store_.racks());
  captureUndoSnapshot();
  unassignItemFromRack(*item);
  item->lastUpdated = time(nullptr);
  logActivity("rack", item->partName + " unassigned from " + previous);
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage("Rack location intentionally unassigned", 2);
}

void App::autoAssignSelectedRackItem() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }
  const auto previous = rackLocation(*item, store_.racks());
  captureUndoSnapshot();
  restoreAutomaticRackAssignment(store_, *item);
  item->lastUpdated = time(nullptr);
  const auto next = rackLocation(*item, store_.racks());
  logActivity("rack", item->partName + " AUTO " + previous + " -> " + (next.empty() ? "unassigned" : next));
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage(next.empty() ? "AUTO found no eligible rack placement" : "AUTO assigned " + next, 3);
}

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

void App::beginEditCurrentItem(bool createNew) {
  editingImportCandidate_ = false;
  page_ = Page::Stock;
  workingCopy_ = {};
  if (createNew) {
    workingCopy_.isNew = true;
    workingCopy_.item.id = makeId();
    workingCopy_.item.partName = "New Part";
    workingCopy_.item.manufacturer = "Unknown";
    workingCopy_.item.category = "Unsorted";
    workingCopy_.item.location = "Unassigned";
    workingCopy_.item.syncStatus = "needs_metadata";
    workingCopy_.item.lastUpdated = time(nullptr);
    workingCopy_.item.createdAt = workingCopy_.item.lastUpdated;
    workingCopy_.originalIndex = store_.items().size();
  } else {
    const auto* current = selectedItem();
    if (current == nullptr) {
      setMessage("No item selected", 2);
      return;
    }
    workingCopy_.isNew = false;
    workingCopy_.item = *current;
    workingCopy_.originalIndex = selectedIndex();
  }

  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
  setMessage(createNew ? "Editing new part" : "Editing " + workingCopy_.item.partName, 3);
}

void App::beginEditImportCandidate() {
  auto* candidate = currentImportCandidate();
  if (candidate == nullptr) {
    setMessage("No import row selected", 2);
    return;
  }

  editingImportCandidate_ = true;
  importEditIndex_ = importSelection_;
  workingCopy_ = {};
  workingCopy_.item = candidate->item;
  workingCopy_.originalIndex = importSelection_;
  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
  page_ = Page::Import;
  setMessage("Editing import row: \xE2\x86\x91\xE2\x86\x93 field, \xE2\x8F\x8E edit, s save, esc cancel", 4);
}

void App::openFieldMenu() {
  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
}

void App::commitEditField(EditField field, const string& value) {
  const auto trimmed = trim(value);
  bool valid = true;

  switch (field) {
    case EditField::PartName:
      workingCopy_.item.partName = trimmed;
      break;
    case EditField::Manufacturer:
      workingCopy_.item.manufacturer = trimmed;
      break;
    case EditField::Category:
      workingCopy_.item.category = trimmed;
      break;
    case EditField::Quantity:
      if (!parseIntegerInRange(trimmed, 0, (numeric_limits<int>::max)(), workingCopy_.item.quantity)) {
        valid = false;
      }
      break;
    case EditField::ReorderThreshold:
      if (!parseIntegerInRange(trimmed, 0, (numeric_limits<int>::max)(),
                               workingCopy_.item.reorderThreshold)) {
        valid = false;
      }
      break;
    case EditField::Location:
      workingCopy_.item.location = trimmed;
      break;
    case EditField::Tags:
      workingCopy_.item.tags = splitFlexible(trimmed);
      break;
    case EditField::Parameters:
      workingCopy_.item.parameters = parseParameters(trimmed);
      break;
    case EditField::Notes:
      workingCopy_.item.notes = trimmed;
      break;
    case EditField::LabelOverride:
      workingCopy_.item.labelOverride = trimmed;
      break;
    case EditField::DigiKeyPart:
      workingCopy_.item.digikeyPartNumber = trimmed;
      break;
    case EditField::DatasheetUrl:
      workingCopy_.item.datasheetUrl = trimmed;
      break;
    case EditField::ProductUrl:
      workingCopy_.item.productUrl = trimmed;
      break;
    case EditField::Sku:
      workingCopy_.item.sku = trimmed;
      break;
    case EditField::RackLocation: {
      string error;
      if (!setManualRackLocation(store_, workingCopy_.item, value, error)) {
        setMessage(error, 4);
        return;
      }
      break;
    }
  }

  if (!valid) {
    setMessage("Invalid numeric value", 3);
    return;
  }

  workingCopy_.item.lastUpdated = time(nullptr);
  setMessage(fieldLabel(field) + " updated", 2);
  inputBuffer_.clear();
  inputMode_ = InputMode::EditFieldMenu;
  dirty_ = true;
}

void App::saveWorkingCopy() {
  if (editingImportCandidate_) {
    if (importEditIndex_ < importCandidates_.size()) {
      importCandidates_[importEditIndex_].item = workingCopy_.item;
    }

    editingImportCandidate_ = false;
    inputMode_ = InputMode::None;
    page_ = Page::Import;
    setMessage("Import row updated", 2);
    dirty_ = true;
    return;
  }

  captureUndoSnapshot();
  if (workingCopy_.isNew) {
    store_.items().push_back(workingCopy_.item);
    reconcileRackAssignment(store_, store_.items().back());
    selectedPosition_ = store_.items().empty() ? 0 : store_.items().size() - 1;
  } else if (workingCopy_.originalIndex < store_.items().size()) {
    store_.items()[workingCopy_.originalIndex] = workingCopy_.item;
    reconcileRackAssignment(store_, store_.items()[workingCopy_.originalIndex]);
  }

  logActivity("edit", workingCopy_.item.partName + " updated");
  saveState();
  inputMode_ = InputMode::None;
  page_ = Page::Stock;
  syncSelectionToFilter();
  setMessage("Changes saved", 2);
}

void App::adjustQuantity(int delta) {
  auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return;
  }

  captureUndoSnapshot();
  const auto candidate = static_cast<long long>(item->quantity) + delta;
  item->quantity = static_cast<int>(clamp<long long>(candidate, 0, numeric_limits<int>::max()));
  item->lastUpdated = time(nullptr);
  logActivity(delta > 0 ? "stock" : "usage", item->partName + " quantity changed to " + to_string(item->quantity));
  const bool saved = saveState();
  setMessage(saved ? item->partName + " quantity is now " + to_string(item->quantity)
                  : "Quantity changed in memory; press R to retry saving",
             saved ? 2 : 5);
  dirty_ = true;
}

void App::setSelectedQuantityFromInput(const string& value) {
  auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return;
  }
  if (value.empty()) {
    setMessage("Enter a quantity from 0 to 2147483647", 4);
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
    setMessage("Quantity must be a whole number from 0 to 2147483647", 4);
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
  setMessage(saved ? item->partName + " quantity set to " + to_string(item->quantity)
                  : "Quantity changed in memory; press R to retry saving",
             saved ? 2 : 5);
  dirty_ = true;
}

void App::logActivity(const string& kind, const string& message) {
  appendActivity(activities_, makeActivity(kind, message));
  const auto now = time(nullptr);
  if (kind == "scan") {
    scannerFlashUntil_ = now + 3;
  } else if (kind == "print") {
    printerFlashUntil_ = now + 3;
  }
  saveActivitiesChecked();
  dirty_ = true;
}


}  // namespace inventatory
