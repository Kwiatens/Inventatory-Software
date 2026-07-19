// Inventatory - Hardware Inventory Management System
// Shared app state helpers and business actions.

#include "App.h"

#include "platform/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>
#include <mutex>
#include <memory>
#include <sstream>
#include <system_error>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kDeviceDebugWindowLines = 14;
constexpr const char* kInventatoryScanTokenCredential = "inventatory-scan-pairing-token";

filesystem::path resolveInventoryDatabasePath(const filesystem::path& selectedPath) {
  error_code error;
  if (selectedPath.empty()) {
    return {};
  }

  if (filesystem::is_regular_file(selectedPath, error) &&
      toLower(selectedPath.extension().string()) == ".db") {
    return selectedPath;
  }

  return selectedPath / "inventory.db";
}

vector<string> splitFlexible(const string& text) {
  vector<string> values;
  string current;
  for (char ch : text) {
    if (ch == ',' || ch == ';' || ch == '\n') {
      current = trim(current);
      if (!current.empty()) {
        values.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }

  current = trim(current);
  if (!current.empty()) {
    values.push_back(current);
  }

  return values;
}

vector<Parameter> parseParameters(const string& text) {
  vector<Parameter> values;
  for (const auto& entry : splitFlexible(text)) {
    const auto equalsPos = entry.find('=');
    if (equalsPos == string::npos) {
      continue;
    }
    values.push_back({trim(entry.substr(0, equalsPos)), trim(entry.substr(equalsPos + 1))});
  }
  return values;
}

}  // namespace

void App::loadState() {
  const bool inventoryLoaded = store_.load(inventoryPath_);
  bool enrichmentChanged = false;
  for (auto& item : store_.items()) {
    enrichmentChanged = applyIecdEnrichment(
                            item, iecdDatabase_.lookup(item.manufacturer, item.manufacturerPartNumber)) ||
                        enrichmentChanged;
  }
  loadActivities(activityPath_, activities_);
  printerService_.loadConfig(printerPath_);
  refreshPrinterState();
  if (activities_.empty()) {
    activities_.push_back(makeActivity("system", "Inventory loaded"));
    activities_.push_back(makeActivity("system", "Terminal dashboard initialized"));
  }
  if (inventoryLoaded && enrichmentChanged) store_.save(inventoryPath_);

  server_.setRecentActivity(activities_);
  if (trim(inventatoryScanConfig_.token).empty()) {
    if (const auto stored = CredentialStore::read(kInventatoryScanTokenCredential); stored.has_value()) {
      inventatoryScanConfig_.token = *stored;
    } else {
      inventatoryScanConfig_.token = generateInventatoryScanToken();
    }
  }
  if (!CredentialStore::write(kInventatoryScanTokenCredential, inventatoryScanConfig_.token)) {
    setMessage("Unable to save the scanner pairing token securely", 5);
  }

  error_code error;
  const bool inventoryFileExists = filesystem::exists(inventoryPath_, error);
  vector<string> saveFailures;
  if (inventoryLoaded || !inventoryFileExists) {
    if (!store_.save(inventoryPath_)) saveFailures.push_back("inventory");
  }
  if (!printerService_.saveConfig(printerPath_)) saveFailures.push_back("printer settings");
  if (!saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_)) saveFailures.push_back("scanner settings");
  if (!saveActivities(activityPath_, activities_)) saveFailures.push_back("activity history");
  persistenceError_ = saveFailures.empty()
                          ? string()
                          : "Could not save " + join(saveFailures, ',') + "; changes remain in memory.";
}

bool App::saveState() {
  ensureInventoryIdentifiers(store_.items());
  reconcileRackAssignments(store_);
  vector<string> saveFailures;
  if (!store_.save(inventoryPath_)) saveFailures.push_back("inventory");
  if (!printerService_.saveConfig(printerPath_)) saveFailures.push_back("printer settings");
  if (!saveActivities(activityPath_, activities_)) saveFailures.push_back("activity history");
  persistenceError_ = saveFailures.empty()
                          ? string()
                          : "Could not save " + join(saveFailures, ',') + "; changes remain in memory.";
  return saveFailures.empty();
}

bool App::chooseInventatoryFolder() {
  filesystem::path selectedPath;
  if (!openFolderDialog(selectedPath, "Select Inventatory folder")) {
    setMessage("Inventatory folder selection cancelled", 2);
    return false;
  }

  if (selectedPath.empty()) {
    setMessage("No folder selected", 2);
    return false;
  }

  saveState();

  const auto selectedInventoryPath = resolveInventoryDatabasePath(selectedPath);
  dataPath_ = selectedInventoryPath.parent_path();
  inventoryPath_ = selectedInventoryPath;
  printerPath_ = dataPath_ / "printer.conf";
  activityPath_ = dataPath_ / "activity.tsv";
  inventatoryScanConfigPath_ = dataPath_ / "inventatory_scan.conf";
  iecdPath_ = dataPath_ / "iecd.sqlite3";
  iecdDatabase_.open(iecdPath_);
  ensureInventoryDatabaseCopied(inventoryPath_);

  printerQueues_.clear();
  printerCheck_ = {};
  inventoryHistory_.clear();
  scanQueue_.clear();
  importCandidates_.clear();
  importAcceptedItemIds_.clear();
  importSourcePath_.clear();
  workingCopy_ = {};
  undoSnapshot_ = {};
  editingImportCandidate_ = false;
  importEditIndex_ = 0;
  importSelection_ = 0;
  selectedPosition_ = 0;
  searchQuery_.clear();
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  page_ = Page::Home;
  dirty_ = true;

  loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
  loadState();
  if (trim(inventatoryScanConfig_.token).empty()) {
    inventatoryScanConfig_.token = generateInventatoryScanToken();
    saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
  }
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token);
  setMessage("Loaded Inventatory folder: " + dataPath_.string(), 4);
  return true;
}

void App::captureUndoSnapshot() {
  undoSnapshot_.items = store_.items();
  undoSnapshot_.racks = store_.racks();
  undoSnapshot_.activities = activities_;
  undoSnapshot_.selectedPosition = selectedPosition_;
  undoSnapshot_.valid = true;
}

bool App::undoLastInventoryChange() {
  if (!undoSnapshot_.valid) {
    setMessage("Nothing to undo", 2);
    return false;
  }

  store_.items() = undoSnapshot_.items;
  store_.racks() = undoSnapshot_.racks;
  activities_ = undoSnapshot_.activities;
  selectedPosition_ = undoSnapshot_.selectedPosition;
  undoSnapshot_.valid = false;
  server_.setRecentActivity(activities_);
  saveState();
  syncSelectionToFilter();
  setMessage("Undid last change", 2);
  return true;
}

void App::setMessage(string text, int seconds) {
  message_ = move(text);
  messageUntil_ = time(nullptr) + seconds;
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

vector<size_t> App::filteredIndices() const {
  const auto queryMatches = filterItems(store_.items(), searchQuery_, store_.racks());
  vector<size_t> indices;
  indices.reserve(queryMatches.size());
  for (const auto index : queryMatches) {
    if (stockDateFilterMatches(store_.items()[index])) indices.push_back(index);
  }
  sort(indices.begin(), indices.end(), [&](size_t lhs, size_t rhs) {
    const auto& left = store_.items()[lhs];
    const auto& right = store_.items()[rhs];
    if (stockSortOrder_ == StockSortOrder::Quantity && left.quantity != right.quantity) {
      return left.quantity > right.quantity;
    }
    const auto leftName = toLower(left.partName);
    const auto rightName = toLower(right.partName);
    if (leftName != rightName) {
      return stockSortOrder_ == StockSortOrder::Za ? leftName > rightName : leftName < rightName;
    }
    return left.id < right.id;
  });
  return indices;
}

size_t App::selectedIndex() const {
  const auto filtered = filteredIndices();
  if (filtered.empty()) {
    return numeric_limits<size_t>::max();
  }
  const auto position = min(selectedPosition_, filtered.size() - 1);
  return filtered[position];
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
  const auto filtered = filteredIndices();
  if (filtered.empty()) {
    selectedPosition_ = 0;
    return;
  }
  if (selectedPosition_ >= filtered.size()) {
    selectedPosition_ = filtered.size() - 1;
  }
  dirty_ = true;
}

void App::moveSelection(int delta) {
  const auto filtered = filteredIndices();
  if (filtered.empty()) {
    selectedPosition_ = 0;
    return;
  }

  const auto current = static_cast<int>(min(selectedPosition_, filtered.size() - 1));
  const auto next = clamp(current + delta, 0, static_cast<int>(filtered.size() - 1));
  selectedPosition_ = static_cast<size_t>(next);
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
  page_ = page;
  inputMode_ = InputMode::None;
  focusedTarget_ = -1;
  cancelDeleteConfirmation();
  if (page != Page::Racks) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
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

  string error;
  if (!printerService_.printRackLabel(*rack, &error)) {
    setMessage("Print failed: " + error, 4);
    refreshPrinterState();
    return false;
  }

  const auto code = rack->code;
  logActivity("print", code + " rack label printed");
  saveState();
  refreshPrinterState();
  setMessage(code + " rack label sent", 2);
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
  inputMode_ = InputMode::Search;
  searchQueryBeforeEdit_ = searchQuery_;
  inputBuffer_ = searchQuery_;
  setMessage("Type to filter immediately; Enter keeps it, Esc restores the previous filter", 3);
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
    workingCopy_.item.enrichmentStatus = "needs_metadata";
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
      try {
        workingCopy_.item.quantity = max(0, stoi(trimmed));
      } catch (...) {
        valid = false;
      }
      break;
    case EditField::ReorderThreshold:
      try {
        workingCopy_.item.reorderThreshold = max(0, stoi(trimmed));
      } catch (...) {
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
    case EditField::DatasheetUrl:
      workingCopy_.item.datasheetUrl = trimmed;
      break;
    case EditField::ManufacturerPartNumber:
      workingCopy_.item.manufacturerPartNumber = trimmed;
      break;
    case EditField::EnrichmentStatus:
      workingCopy_.item.enrichmentStatus = toLower(trimmed);
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
    applyIecdEnrichment(store_.items().back(),
                        iecdDatabase_.lookup(store_.items().back().manufacturer,
                                             store_.items().back().manufacturerPartNumber));
    reconcileRackAssignment(store_, store_.items().back());
    selectedPosition_ = store_.items().empty() ? 0 : store_.items().size() - 1;
  } else if (workingCopy_.originalIndex < store_.items().size()) {
    store_.items()[workingCopy_.originalIndex] = workingCopy_.item;
    applyIecdEnrichment(store_.items()[workingCopy_.originalIndex],
                        iecdDatabase_.lookup(store_.items()[workingCopy_.originalIndex].manufacturer,
                                             store_.items()[workingCopy_.originalIndex].manufacturerPartNumber));
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
  item->quantity = max(0, item->quantity + delta);
  item->lastUpdated = time(nullptr);
  logActivity(delta > 0 ? "stock" : "usage", item->partName + " quantity changed to " + to_string(item->quantity));
  saveState();
  setMessage(item->partName + " quantity is now " + to_string(item->quantity), 2);
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
  server_.setRecentActivity(activities_);
  saveActivities(activityPath_, activities_);
  dirty_ = true;
}

void App::toggleAutoPrintScannedLabels() {
  autoPrintScannedLabels_ = !autoPrintScannedLabels_;
  settings_.autoPrintScannedLabels = autoPrintScannedLabels_;
  settingsDraft_.autoPrintScannedLabels = autoPrintScannedLabels_;
  saveAppSettings(settingsPath_, settings_);
  setMessage(autoPrintScannedLabels_ ? "Auto label printing enabled" : "Auto label printing disabled", 3);
}

bool App::autoPrintScannedLabel(const string& itemId) {
  if (!autoPrintScannedLabels_) {
    return false;
  }

  const auto* item = store_.findById(itemId);
  if (item == nullptr) {
    return false;
  }

  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("Auto label skipped: no printer configured", 4);
    return true;
  }

  printLabelForItem(*item, "Auto-printed label for ", false);
  return true;
}

void App::pushScanCode(const DeviceScanRequest& request) {
  lock_guard<mutex> lock(scanMutex_);
  scanQueue_.push_back(request);
}

void App::processScans() {
  vector<DeviceScanRequest> pending;
  {
    lock_guard<mutex> lock(scanMutex_);
    pending.swap(scanQueue_);
  }

  for (const auto& request : pending) {
    const auto code = trim(request.manufacturerPartNumber).empty() ? request.code : request.manufacturerPartNumber;
    const auto resolution = resolveScanCode(store_, code);
    if (resolution.matched) {
      if (auto* item = store_.findById(resolution.itemId)) {
        if (!trim(request.manufacturer).empty()) item->manufacturer = trim(request.manufacturer);
        if (!trim(request.manufacturerPartNumber).empty()) {
          item->manufacturerPartNumber = trim(request.manufacturerPartNumber);
        } else if (trim(request.encodedPartName).empty() && item->manufacturerPartNumber.empty()) {
          item->manufacturerPartNumber = code;
        } else if (resolution.created) {
          item->manufacturerPartNumber.clear();
        }
        if (!trim(request.encodedPartName).empty() && (resolution.created || item->partName.empty())) {
          item->partName = trim(request.encodedPartName);
        }
        applyIecdEnrichment(*item, iecdDatabase_.lookup(item->manufacturer, item->manufacturerPartNumber));
      }

      if (resolution.created) {
        if (auto* item = store_.findById(resolution.itemId)) {
          item->quantity = max(0, request.quantity);
          reconcileRackAssignment(store_, *item);
        }
        logActivity("scan", "Created item from code " + code + " qty " + to_string(max(0, request.quantity)));
      } else {
        if (auto* item = store_.findById(resolution.itemId)) {
          reconcileRackAssignment(store_, *item);
        }
        logActivity("scan", "Matched existing item with code " + code);
      }

      if (const auto* item = store_.findById(resolution.itemId)) {
        const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& entry) {
          return entry.id == item->id;
        });
        if (it != store_.items().end()) {
          selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
        }
      }

      changePage(Page::Stock);
      saveState();
      if (!resolution.created || !autoPrintScannedLabel(resolution.itemId)) {
        setMessage(resolution.message, 3);
      }
    } else {
      setMessage("Scan ignored: " + resolution.message, 3);
    }
    syncSelectionToFilter();
  }
}

DeviceQuantityResult App::enqueueDeviceQuantity(const DeviceQuantityRequest& request) {
  auto pending = make_shared<PendingDeviceQuantity>();
  pending->request = request;
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
    deviceQuantityQueue_.push_back(pending);
  }
  unique_lock<mutex> lock(pending->mutex);
  if (!pending->ready.wait_for(lock, chrono::seconds(3), [&] { return pending->complete; })) {
    DeviceQuantityResult timeout;
    timeout.httpStatus = 503;
    timeout.error = "Inventatory did not process the request in time";
    return timeout;
  }
  return pending->result;
}

bool App::printWireLabel(const string& text) {
  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("No printer configured", 3);
    openSettings(SettingsCategory::Printer);
    return false;
  }
  string error;
  if (!printerService_.printWireLabel(text, &error)) {
    setMessage(error.empty() ? "Wire label could not be printed" : error, 4);
    refreshPrinterState();
    return false;
  }
  printerFlashUntil_ = time(nullptr) + 3;
  logActivity("print", "wire label printed");
  setMessage("Wire label sent", 3);
  return true;
}

bool App::printDeviceQuickLabel(const DeviceQuickLabelPrintRequest& request, DeviceQuickLabelPrintResult& result) {
  result.requestId = request.requestId;
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    const auto known = quickLabelPrintResults_.find(request.requestId);
    if (known != quickLabelPrintResults_.end()) {
      result = known->second;
      return result.status == "completed";
    }
    if (request.revision != settings_.quickLabelRevision) {
      result = {request.requestId, "failed", "stale_presets", "Refresh quick labels"};
    } else if (request.presetIndex < 1 || request.presetIndex > static_cast<int>(settings_.quickLabelPresets.size())) {
      result = {request.requestId, "failed", "missing_preset", "Quick label not found"};
    }
  }

  if (result.status.empty()) {
    string text;
    {
      lock_guard<mutex> lock(quickLabelMutex_);
      text = settings_.quickLabelPresets[request.presetIndex - 1];
    }
    string error;
    if (!printerService_.hasConfiguredPrinter()) {
      result = {request.requestId, "failed", "printer_unconfigured", "No printer configured"};
    } else if (!printerService_.printWireLabel(text, &error)) {
      result = {request.requestId, "failed", "printer_failed", error.empty() ? "Printer failed" : error};
    } else {
      result = {request.requestId, "completed", "", "Label sent"};
    }
  }

  {
    lock_guard<mutex> lock(quickLabelMutex_);
    quickLabelPrintResults_[request.requestId] = result;
    quickLabelPrintOrder_.push_back(request.requestId);
    while (quickLabelPrintOrder_.size() > 64) {
      quickLabelPrintResults_.erase(quickLabelPrintOrder_.front());
      quickLabelPrintOrder_.pop_front();
    }
  }
  return result.status == "completed";
}

void App::addQuickLabelPreset() {
  if (settingsDraft_.quickLabelPresets.size() >= kQuickLabelPresetLimit) {
    setMessage("A maximum of 12 quick labels is supported", 3);
    return;
  }
  settingsDraft_.quickLabelPresets.push_back("New label");
  settingsField_ = static_cast<int>(settingsDraft_.quickLabelPresets.size() - 1);
  settingsDirty_ = true;
  beginSettingsFieldEdit(settingsField_);
}

void App::deleteQuickLabelPreset() {
  if (settingsField_ < 0 || settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) return;
  settingsDraft_.quickLabelPresets.erase(settingsDraft_.quickLabelPresets.begin() + settingsField_);
  if (settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) --settingsField_;
  settingsDirty_ = true;
  dirty_ = true;
}

void App::openStockFilterPanel() {
  inputMode_ = InputMode::StockFilter;
  stockDateFilterSubmenuOpen_ = false;
  stockFilterSelection_ = stockDateFilter_ != StockDateFilter::All ? 0
                          : stockSortOrder_ == StockSortOrder::Quantity ? 1
                          : stockSortOrder_ == StockSortOrder::Za ? 3 : 2;
  focusedTarget_ = -1;
  dirty_ = true;
}

void App::openStockDateFilterSubmenu() {
  stockDateFilterSubmenuOpen_ = true;
  stockFilterSelection_ = static_cast<int>(stockDateFilter_);
  dirty_ = true;
}

void App::applyStockDateFilter(StockDateFilter filter) {
  stockDateFilter_ = filter;
  stockFilterSelection_ = static_cast<int>(filter);
  stockDateFilterSubmenuOpen_ = false;
  inputMode_ = InputMode::None;
  syncSelectionToFilter();
  setMessage("Stock filter: " + stockDateFilterName(filter), 3);
  dirty_ = true;
}

void App::applyStockSortOrder(StockSortOrder order) {
  stockSortOrder_ = order;
  stockDateFilterSubmenuOpen_ = false;
  inputMode_ = InputMode::None;
  syncSelectionToFilter();
  const auto message = order == StockSortOrder::Az ? "Stock sorted A-Z"
                       : order == StockSortOrder::Za ? "Stock sorted Z-A"
                                                     : "Stock sorted by quantity";
  setMessage(message, 3);
  dirty_ = true;
}

void App::handleStockFilterKey(const KeyEvent& key) {
  const int optionCount = stockDateFilterSubmenuOpen_ ? 5 : 4;
  if (key.type == KeyType::Up || (key.type == KeyType::Character && key.ch == 'k')) {
    stockFilterSelection_ = max(0, stockFilterSelection_ - 1);
  } else if (key.type == KeyType::Down || (key.type == KeyType::Character && key.ch == 'j')) {
    stockFilterSelection_ = min(optionCount - 1, stockFilterSelection_ + 1);
  } else if (key.type == KeyType::Enter) {
    if (stockDateFilterSubmenuOpen_) {
      applyStockDateFilter(static_cast<StockDateFilter>(stockFilterSelection_));
    } else if (stockFilterSelection_ == 0) {
      openStockDateFilterSubmenu();
    } else if (stockFilterSelection_ == 1) {
      applyStockSortOrder(StockSortOrder::Quantity);
    } else if (stockFilterSelection_ == 2) {
      applyStockSortOrder(StockSortOrder::Az);
    } else {
      applyStockSortOrder(StockSortOrder::Za);
    }
    return;
  } else if (key.type == KeyType::Escape || (key.type == KeyType::Character && key.ch == 'f')) {
    if (stockDateFilterSubmenuOpen_) {
      stockDateFilterSubmenuOpen_ = false;
      stockFilterSelection_ = 0;
    } else {
      inputMode_ = InputMode::None;
    }
  } else if (key.type == KeyType::Left && stockDateFilterSubmenuOpen_) {
    stockDateFilterSubmenuOpen_ = false;
    stockFilterSelection_ = 0;
  } else if (key.type == KeyType::Character && key.ch >= '1' && key.ch <= '4' && !stockDateFilterSubmenuOpen_) {
    const auto choice = key.ch - '1';
    if (choice == 0) {
      openStockDateFilterSubmenu();
    } else if (choice == 1) {
      applyStockSortOrder(StockSortOrder::Quantity);
    } else if (choice == 2) {
      applyStockSortOrder(StockSortOrder::Az);
    } else {
      applyStockSortOrder(StockSortOrder::Za);
    }
    return;
  } else {
    return;
  }
  dirty_ = true;
}

void App::moveQuickLabelPreset(int direction) {
  const int destination = settingsField_ + direction;
  if (settingsField_ < 0 || destination < 0 || destination >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) return;
  swap(settingsDraft_.quickLabelPresets[settingsField_], settingsDraft_.quickLabelPresets[destination]);
  settingsField_ = destination;
  settingsDirty_ = true;
  dirty_ = true;
}

void App::testQuickLabelPreset() {
  if (settingsField_ < 0 || settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) {
    setMessage("Select a quick label first", 3);
    return;
  }
  const auto original = printerService_.configuredPrinter();
  if (!settingsDraft_.printerQueue.empty()) printerService_.setConfiguredPrinter(settingsDraft_.printerQueue);
  string error;
  const bool printed = printerService_.hasConfiguredPrinter() &&
                       printerService_.printWireLabel(settingsDraft_.quickLabelPresets[settingsField_], &error);
  printerService_.setConfiguredPrinter(original);
  setMessage(printed ? "Quick label sent" : (error.empty() ? "No printer configured" : error), 4);
}

void App::enqueueDeviceStatus(const DeviceStatusReport& report) {
  lock_guard<mutex> lock(deviceQueueMutex_);
  deviceStatusQueue_.push_back(report);
}

void App::enqueueDeviceDebug(const DeviceDebugReport& report) {
  lock_guard<mutex> lock(deviceQueueMutex_);
  deviceDebugQueue_.push_back(report);
}

bool App::handleDeviceSync(const DeviceSyncRequest& request, DeviceSyncResponse& response, string& error) {
  DeviceStatusReport status;
  status.deviceId = request.deviceId;
  status.firmwareVersion = request.firmwareVersion;
  status.rssi = request.rssi;
  status.debug = "protocol=v2 mode=" + request.mode + " queue=" + to_string(request.queueDepth);
  status.protocolVersion = request.protocolVersion;
  status.mode = request.mode;
  status.pendingEventCount = request.queueDepth;
  enqueueDeviceStatus(status);
  if (!acceptDeviceSyncEvents(inventoryPath_, request, response, error)) return false;
  if (request.hasLookup) {
    response.lookupResult = lookupDeviceItem(inventoryPath_, request.lookup);
    response.hasLookupResult = true;
  }
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    response.hasQuickLabels = true;
    response.quickLabelRevision = settings_.quickLabelRevision;
    response.quickLabelPresets = settings_.quickLabelPresets;
  }
  if (request.hasQuickLabelPrint) {
    response.hasQuickLabelPrintResult = true;
    printDeviceQuickLabel(request.quickLabelPrint, response.quickLabelPrintResult);
  }
  return true;
}

void App::processDeviceSyncEvents() {
  const auto pending = loadPendingDeviceSyncEvents(inventoryPath_, 1);
  if (pending.empty()) return;

  const auto& event = pending.front();
  auto candidate = store_;
  DeviceSyncResult result;
  result.resultId = event.eventId + "-result";
  result.eventId = event.eventId;
  result.status = "failed";
  result.code = "invalid_event";
  result.message = "Invalid inventory event";

  string affectedItemId;
  bool created = false;
  if (event.type == "inventory.adjust") {
    DeviceQuantityRequest request{"protocol-v2", event.eventId, event.code, event.value};
    const auto quantityResult = applyDeviceQuantity(candidate, request);
    result.requestedDelta = event.value;
    result.appliedDelta = quantityResult.appliedDelta;
    result.quantity = quantityResult.quantity;
    result.itemName = quantityResult.item;
    if (quantityResult.ok) {
      result.status = "completed";
      result.code.clear();
      result.message = "Quantity updated";
      if (const auto* item = candidate.findByMachineCode(event.code)) {
        affectedItemId = item->id;
        result.existing = true;
        result.itemName = item->iecdCanonicalName.empty() ? item->partName : item->iecdCanonicalName;
        result.purposeLabel = item->iecdPurposeLabel;
        result.location = rackLocation(*item, candidate.racks());
        if (result.location.empty()) result.location = item->location;
      }
    } else {
      result.code = quantityResult.httpStatus == 404 ? "unknown_item" : "invalid_quantity";
      result.message = quantityResult.error;
    }
  } else if (event.type == "inventory.receive" && event.value > 0) {
    const auto legacyMpn = trim(event.manufacturerPartNumber).empty() && trim(event.encodedPartName).empty()
                               ? event.code
                               : event.manufacturerPartNumber;
    const auto resolution = resolveDecodedComponent(candidate, event.manufacturer, legacyMpn,
                                                    event.encodedPartName);
    if (!resolution.matched) {
      result.code = "scan_unresolved";
      result.message = resolution.message;
    } else if (auto* item = candidate.findById(resolution.itemId)) {
      affectedItemId = item->id;
      created = resolution.created;
      result.existing = !created;
      const int oldQuantity = item->quantity;
      const long long requested = static_cast<long long>(oldQuantity) + event.value;
      item->quantity = static_cast<int>(min<long long>(requested, numeric_limits<int>::max()));
      item->lastUpdated = time(nullptr);
      result.requestedDelta = event.value;
      result.appliedDelta = item->quantity - oldQuantity;
      result.quantity = item->quantity;

      applyIecdEnrichment(*item, iecdDatabase_.lookup(item->manufacturer, item->manufacturerPartNumber));
      reconcileRackAssignment(candidate, *item);
      result.itemName = item->iecdCanonicalName.empty() ? item->partName : item->iecdCanonicalName;
      result.purposeLabel = item->iecdPurposeLabel;
      result.location = rackLocation(*item, candidate.racks());
      if (result.location.empty()) result.location = item->location.empty() ? "UNASSIGNED" : item->location;
      result.status = "completed";
      result.code.clear();
      result.message = created ? "New item received" : "Existing item updated";
    }
  } else if (event.type == "inventory.receive") {
    result.code = "invalid_quantity";
    result.message = "Received quantity must be positive";
  } else {
    result.code = "unsupported_event";
    result.message = "Unsupported inventory event type";
  }

  if (!completeDeviceSyncEvent(candidate, inventoryPath_, result)) {
    setMessage("Inventatory Scan event could not be committed", 4);
    return;
  }
  store_ = move(candidate);
  deviceLastResult_ = result.status == "failed"
                          ? "ERROR " + result.message
                          : (result.existing ? "EXISTING " : "NEW ") + result.itemName + " QTY " +
                                to_string(result.quantity);
  if (result.status == "failed") {
    logActivity("device error", result.message);
  } else {
    logActivity(created ? "scan receive" : "stock receive",
                result.itemName + " changed by " + to_string(result.appliedDelta) + " to " +
                    to_string(result.quantity));
    scannerFlashUntil_ = time(nullptr) + 3;
    if (created) autoPrintScannedLabel(affectedItemId);
  }
  saveActivities(activityPath_, activities_);
  server_.setRecentActivity(activities_);
  dirty_ = true;
}

void App::adjustDeviceDebugScroll(int delta) {
  const auto total = deviceDebugLog_.size();
  if (total == 0) {
    deviceDebugScroll_ = 0;
    deviceDebugFollow_ = true;
    return;
  }
  const size_t step = static_cast<size_t>(delta < 0 ? -delta : delta);
  const auto maxScroll = total > kDeviceDebugWindowLines ? total - kDeviceDebugWindowLines : 0;
  if (delta < 0) {
    deviceDebugFollow_ = false;
    deviceDebugScroll_ = min(deviceDebugScroll_ + step, maxScroll);
  } else if (delta > 0) {
    deviceDebugFollow_ = false;
    deviceDebugScroll_ = deviceDebugScroll_ > step ? deviceDebugScroll_ - step : 0;
  }
}

void App::processDeviceRequests() {
  vector<shared_ptr<PendingDeviceQuantity>> quantities;
  vector<DeviceStatusReport> statuses;
  vector<DeviceDebugReport> debugReports;
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
    quantities.swap(deviceQuantityQueue_);
    statuses.swap(deviceStatusQueue_);
    debugReports.swap(deviceDebugQueue_);
  }

  for (const auto& status : statuses) {
    deviceLastSeen_ = time(nullptr);
    deviceFirmwareVersion_ = status.firmwareVersion;
    deviceRssi_ = status.rssi;
    deviceDebug_ = status.debug;
    if (status.protocolVersion > 0) {
      deviceProtocolVersion_ = status.protocolVersion;
      deviceMode_ = status.mode;
      devicePendingEventCount_ = status.pendingEventCount;
      deviceLastSync_ = time(nullptr);
    }
    if (trim(inventatoryScanConfig_.deviceId).empty() && !trim(status.deviceId).empty()) {
      inventatoryScanConfig_.deviceId = trim(status.deviceId);
      saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
      server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token);
    }
    dirty_ = true;
  }

  for (const auto& debug : debugReports) {
    const auto now = time(nullptr);
    const auto level = trim(debug.level).empty() ? string("info") : trim(debug.level);
    ostringstream out;
    out << nowTimestampString(now) << " [" << level << "] " << debug.message;
    deviceDebugLog_.push_back(out.str());
    if (deviceDebugLog_.size() > 400U) {
      deviceDebugLog_.erase(deviceDebugLog_.begin(), deviceDebugLog_.begin() + 100);
    }
    if (deviceDebugFollow_) {
      deviceDebugScroll_ = deviceDebugLog_.size() > kDeviceDebugWindowLines
                               ? deviceDebugLog_.size() - kDeviceDebugWindowLines
                               : 0;
    }
    dirty_ = true;
  }

  for (const auto& pending : quantities) {
    bool pairingChanged = false;
    if (trim(inventatoryScanConfig_.deviceId).empty() && !trim(pending->request.deviceId).empty()) {
      inventatoryScanConfig_.deviceId = trim(pending->request.deviceId);
      pairingChanged = true;
    }
    const auto result = applyDeviceQuantityCached(store_, pending->request, deviceRequestCache_, deviceRequestOrder_);
    if (pairingChanged) {
      saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
      server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token);
    }
    if (result.ok) {
      logActivity(result.appliedDelta < 0 ? "usage scan" : "stock scan",
                  result.item + " quantity changed by " + to_string(result.appliedDelta) +
                      " to " + to_string(result.quantity));
      saveState();
      scannerFlashUntil_ = time(nullptr) + 3;
      deviceLastResult_ = (result.appliedDelta >= 0 ? "+" : "") + to_string(result.appliedDelta) +
                          " " + result.item + " QTY " + to_string(result.quantity);
    } else {
      deviceLastResult_ = "ERROR " + result.error;
    }
    deviceLastSeen_ = time(nullptr);
    dirty_ = true;
    {
      lock_guard<mutex> lock(pending->mutex);
      pending->result = result;
      pending->complete = true;
    }
    pending->ready.notify_one();
  }
}

string App::inventatoryScanDeviceSummary() const {
  if (trim(inventatoryScanConfig_.token).empty()) return "R1 UNPAIRED";
  if (trim(inventatoryScanConfig_.deviceId).empty()) return "R1 WAITING FOR DEVICE";
  if (deviceLastSeen_ == 0 || time(nullptr) - deviceLastSeen_ > 15) return "R1 OFFLINE";
  if (!deviceLastResult_.empty()) return "R1 ONLINE  " + deviceLastResult_;
  return "R1 ONLINE  RSSI " + to_string(deviceRssi_);
}

void App::beginCsvImport() {
  filesystem::path selectedPath;
  if (!openCsvFileDialog(selectedPath)) {
    setMessage("CSV import cancelled", 2);
    return;
  }

  const auto result = loadBomCsvFile(selectedPath, store_.items());
  if (!result.ok) {
    setMessage("CSV import failed: " + result.error, 6);
    return;
  }

  importCandidates_ = result.candidates;
  importAcceptedItemIds_.clear();
  importSourcePath_ = selectedPath;
  importSelection_ = 0;
  importCreatedCount_ = 0;
  importMergedCount_ = 0;
  importSkippedCount_ = 0;
  editingImportCandidate_ = false;
  inputMode_ = InputMode::None;
  page_ = Page::Import;

  setMessage("Loaded " + to_string(importCandidates_.size()) + " CSV rows for review", 4);
}

CsvImportCandidate* App::currentImportCandidate() {
  if (importCandidates_.empty()) {
    return nullptr;
  }
  importSelection_ = min(importSelection_, importCandidates_.size() - 1);
  return &importCandidates_[importSelection_];
}

const CsvImportCandidate* App::currentImportCandidate() const {
  if (importCandidates_.empty()) {
    return nullptr;
  }
  const auto index = min(importSelection_, importCandidates_.size() - 1);
  return &importCandidates_[index];
}

void App::moveImportSelection(int delta) {
  if (importCandidates_.empty()) {
    importSelection_ = 0;
    dirty_ = true;
    return;
  }

  const auto current = static_cast<int>(min(importSelection_, importCandidates_.size() - 1));
  importSelection_ = static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(importCandidates_.size() - 1)));
  dirty_ = true;
}

void App::acceptImportCandidate() {
  auto* candidate = currentImportCandidate();
  if (candidate == nullptr) {
    finishImportReview();
    return;
  }

  string acceptedId;
  if (candidate->hasConflict) {
    auto* existing = store_.findById(candidate->existingItemId);
    if (existing != nullptr) {
      captureUndoSnapshot();
      existing->quantity = max(0, existing->quantity + candidate->item.quantity);
      existing->lastUpdated = time(nullptr);
      mergeImportedMetadata(*existing, candidate->item);
      if (candidate->item.rackAssignment != RackAssignmentMode::Automatic) {
        existing->rackId = candidate->item.rackId;
        existing->rackSlot = candidate->item.rackSlot;
        existing->rackAssignment = candidate->item.rackAssignment;
      }
      reconcileRackAssignment(store_, *existing);
      acceptedId = existing->id;
      ++importMergedCount_;
    }
  }

  if (acceptedId.empty()) {
    captureUndoSnapshot();
    store_.items().push_back(candidate->item);
    reconcileRackAssignment(store_, store_.items().back());
    acceptedId = store_.items().back().id;
    ++importCreatedCount_;
  }

  if (auto* accepted = store_.findById(acceptedId)) {
    applyIecdEnrichment(*accepted,
                        iecdDatabase_.lookup(accepted->manufacturer, accepted->manufacturerPartNumber));
  }

  importAcceptedItemIds_.push_back(acceptedId);
  importCandidates_.erase(importCandidates_.begin() + static_cast<ptrdiff_t>(importSelection_));
  if (importSelection_ >= importCandidates_.size() && !importCandidates_.empty()) {
    importSelection_ = importCandidates_.size() - 1;
  }

  saveState();
  if (importCandidates_.empty()) {
    finishImportReview();
  } else {
    setMessage("Accepted import row", 2);
  }
  dirty_ = true;
}

void App::skipImportCandidate() {
  if (importCandidates_.empty()) {
    finishImportReview();
    return;
  }

  importCandidates_.erase(importCandidates_.begin() + static_cast<ptrdiff_t>(importSelection_));
  ++importSkippedCount_;
  if (importSelection_ >= importCandidates_.size() && !importCandidates_.empty()) {
    importSelection_ = importCandidates_.size() - 1;
  }

  if (importCandidates_.empty()) {
    finishImportReview();
  } else {
    setMessage("Skipped import row", 2);
  }
  dirty_ = true;
}

void App::finishImportReview() {
  finishCsvImport();
}

void App::finishCsvImport() {
  for (const auto& itemId : importAcceptedItemIds_) {
    if (auto* item = store_.findById(itemId)) {
      applyIecdEnrichment(*item, iecdDatabase_.lookup(item->manufacturer, item->manufacturerPartNumber));
    }
  }
  saveState();
  const auto summary = importCompletionMessage();
  logActivity("import", summary);
  importCandidates_.clear();
  importAcceptedItemIds_.clear();
  importSourcePath_.clear();
  importSelection_ = 0;
  editingImportCandidate_ = false;
  changePage(Page::Home);
  setMessage(summary, 8);
}

string App::importCompletionMessage() const {
  return "CSV import complete: " + to_string(importCreatedCount_) + " new, " +
         to_string(importMergedCount_) + " merged, " + to_string(importSkippedCount_) + " skipped";
}

void App::openCurrentUrl(const string& url, const string& label) {
  if (trim(url).empty()) {
    setMessage("No " + label + " link stored for this item", 3);
    return;
  }
  if (openUrl(url)) {
    setMessage("Opened " + label + " link", 2);
  } else {
    setMessage("Unable to open " + label + " link", 3);
  }
}

string App::fieldLabel(EditField field) const {
  switch (field) {
    case EditField::PartName:
      return "Part name";
    case EditField::Manufacturer:
      return "Manufacturer";
    case EditField::Category:
      return "Category";
    case EditField::Quantity:
      return "Quantity";
    case EditField::ReorderThreshold:
      return "Reorder threshold";
    case EditField::Location:
      return "Location";
    case EditField::Tags:
      return "Tags";
    case EditField::Parameters:
      return "Parameters";
    case EditField::Notes:
      return "Notes";
    case EditField::DatasheetUrl:
      return "Datasheet URL";
    case EditField::ManufacturerPartNumber:
      return "Manufacturer part number";
    case EditField::EnrichmentStatus:
      return "IECD status";
    case EditField::RackLocation:
      return "Rack location";
  }
  return "Field";
}

string App::currentFieldValue(EditField field) const {
  const auto* item = workingCopy_.item.id.empty() && !workingCopy_.isNew ? selectedItem() : &workingCopy_.item;
  if (item == nullptr) {
    return {};
  }

  switch (field) {
    case EditField::PartName:
      return item->partName;
    case EditField::Manufacturer:
      return item->manufacturer;
    case EditField::Category:
      return item->category;
    case EditField::Quantity:
      return to_string(item->quantity);
    case EditField::ReorderThreshold:
      return to_string(item->reorderThreshold);
    case EditField::Location:
      return item->location;
    case EditField::Tags:
      return join(item->tags, ',');
    case EditField::Parameters: {
      ostringstream out;
      for (size_t index = 0; index < item->parameters.size(); ++index) {
        if (index > 0) {
          out << "; ";
        }
        out << item->parameters[index].name << '=' << item->parameters[index].value;
      }
      return out.str();
    }
    case EditField::Notes:
      return item->notes;
    case EditField::DatasheetUrl:
      return item->datasheetUrl;
    case EditField::ManufacturerPartNumber:
      return item->manufacturerPartNumber;
    case EditField::EnrichmentStatus:
      return item->enrichmentStatus;
    case EditField::RackLocation: {
      const auto location = rackLocation(*item, store_.racks());
      return location.empty() ? (item->rackAssignment == RackAssignmentMode::Automatic ? "AUTO" : "") : location;
    }
  }

  return {};
}

vector<App::FieldOption> App::fieldOptions() const {
  return {
      {"Part name", EditField::PartName},
      {"Manufacturer", EditField::Manufacturer},
      {"Category", EditField::Category},
      {"Quantity", EditField::Quantity},
      {"Reorder threshold", EditField::ReorderThreshold},
      {"Location", EditField::Location},
      {"Rack location", EditField::RackLocation},
      {"Tags", EditField::Tags},
      {"Parameters", EditField::Parameters},
      {"Notes", EditField::Notes},
      {"Datasheet URL", EditField::DatasheetUrl},
      {"Manufacturer part number", EditField::ManufacturerPartNumber},
      {"IECD status", EditField::EnrichmentStatus},
  };
}

string App::softwareVersion() const {
#ifdef Inventatory_VERSION_STRING
  return Inventatory_VERSION_STRING;
#else
  return "dev";
#endif
}

string App::itemDetailText(const InventoryItem& item, int width) const {
  ostringstream out;
  const auto fields = stockPreviewFields(item, rackLocation(item, store_.racks()));
  for (const auto& field : fields) {
    const auto line = field.label + field.value;
    for (const auto& wrapped : wrapText(line, width)) {
      out << wrapped << '\n';
    }
  }

  return out.str();
}

string App::summaryLine() const {
  const auto summary = summarize(store_.items());
  ostringstream out;
  out << summary.itemCount << " items"
      << " | " << summary.totalUnits << " units"
      << " | " << summary.lowStockCount << " low"
      << " | " << summary.missingMetadataCount << " missing metadata"
      << " | " << summary.unenrichedCount << " not matched";
  return out.str();
}

string App::activePrompt() const {
  if (inputMode_ == InputMode::EditValue && fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
    return fieldLabel(menuOptions_[fieldMenuIndex_].field) + ": ";
  }
  if (inputMode_ == InputMode::RackRename) return "Rename rack to: ";
  if (inputMode_ == InputMode::RackType) return "Rack type: ";
  if (inputMode_ == InputMode::RackCreate) return "New rack type: ";
  if (inputMode_ == InputMode::RackJump) return "Jump to rack: ";
  if (inputMode_ == InputMode::RackFilter) return "Rack filter: ";
  return "";
}

}  // namespace inventatory
