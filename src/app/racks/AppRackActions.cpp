// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - rack navigation, editing, movement, and printing actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>
#include <memory>
#include <system_error>
#include <unordered_set>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::changePage(Page page) {
  hideVendorKeys();
  // The unsaved-changes prompt is modal and must be answered first, or the draft is stranded.
  // completeSettingsExit() closes it before navigating.
  if (inputMode_ == InputMode::ExitConfirmation) {
    setMessage("Answer the unsaved-changes prompt first: Save, Discard, or Stay", 4, UiMessageSeverity::Warning);
    return;
  }
  if (page != page_ && page_ == Page::Stock && stocktakeActive_) {
    setMessage("Finish or cancel the stocktake before leaving Stock", 5);
    return;
  }
  if (page != page_ && page_ == Page::Import && importCommitPending_) {
    setMessage("Import is not saved yet. Press R to retry or Q to cancel it.", 6);
    return;
  }
  if (page != page_ && page_ == Page::Import && importStageActive_) {
    if (!confirmImportDiscard()) return;
    cancelImportSession();
  }
  if (page != page_ && page_ == Page::Settings && settingsDirty_) {
    pendingPageAfterSettings_ = page;
    exitSavePending_ = false;
    inputMode_ = InputMode::ExitConfirmation;
    setMessage("Unsaved settings: press S to save, D to discard, or Esc to stay", 5);
    return;
  }
  if (page != page_) releasePageState(page_);
  page_ = page;
  inputMode_ = InputMode::None;
  if (page != Page::Stock) {
    closestSearchActive_ = false;
    closestSearchQuery_.clear();
    closestSelectedPosition_ = 0;
  }
  focusedTargetId_.clear();
  cancelDeleteConfirmation();
  cancelRackDeletion();
  if (page != Page::Racks) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
  }
  if (page != Page::Projects) {
    // Leaving the page abandons an unfinished BOM walkthrough.
    bomDeductPrompt_ = false;
    if (bomView_ == BomView::Build) {
      bomView_ = BomView::Overview;
      bomBuildStep_ = 0;
    }
    bomDeleteConfirmationProjectId_.clear();
    bomDeleteConfirmationUntil_ = 0;
  }
  dirty_ = true;
}

// Clears transient state (wizard secrets, Bluetooth discovery, half-edited fields) on every exit.
void App::releasePageState(Page leaving) {
  switch (leaving) {
    case Page::ScanSetup:
      bleProvisioning_.stopDiscovery();
      bleWifiSsid_.clear();
      bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
      bleWifiPassword_.clear();
      blePairingCode_.clear();
      inputBuffer_.assign(inputBuffer_.size(), '\0');
      inputBuffer_.clear();
      break;
    case Page::VendorSetup:
      settingsDraft_ = settings_;
      settingsDirty_ = false;
      settingsEditingField_ = false;
      stagedDigiKeySecret_.assign(stagedDigiKeySecret_.size(), '\0');
      stagedDigiKeySecret_.clear();
      stagedDigiKeySecretChanged_ = false;
      inputBuffer_.assign(inputBuffer_.size(), '\0');
      inputBuffer_.clear();
      break;
    case Page::Settings:
      if (settingsEditingField_ || appearancePickerOpen_) inputBuffer_.clear();
      settingsEditingField_ = false;
      appearancePickerOpen_ = false;
      break;
    default:
      break;
  }
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
  const auto* rack = selectedRack();
  if (rack == nullptr) return {};
  const int rows = storageGridRows(*rack);
  const int columns = storageGridColumns(*rack);
  if (rackRow_ >= rows || rackColumn_ >= columns) return {};
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
  const auto* rack = selectedRack();
  const int rows = rack == nullptr ? 0 : storageGridRows(*rack);
  const int columns = rack == nullptr ? 0 : storageGridColumns(*rack);
  rackRow_ = clamp(rackRow_, 0, max(0, rows - 1));
  rackColumn_ = clamp(rackColumn_, 0, max(0, columns - 1));
  dirty_ = true;
}

void App::renameSelectedRack(const string& value) {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }
  if (isOrganizer(*rack)) {
    const auto name = trim(value);
    string error;
    if (!validateStorageName(store_, name, rack->id, error)) {
      setMessage(error, 4, UiMessageSeverity::Warning);
      return;
    }
    captureUndoSnapshot();
    const auto previous = rack->code;
    rack->code = name;
    logActivity("rack", previous + " renamed to " + name);
    saveState();
    syncRackSelection();
    setMessage("Organizer renamed to " + name, 2);
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
  // An organizer without a type is filled by hand only.
  if (type.empty() && !isOrganizer(*rack)) {
    setMessage("Rack type cannot be empty", 3);
    return;
  }
  captureUndoSnapshot();
  const auto previous = rack->componentType;
  // saveState() can add racks and invalidate `rack`, so copy what is still needed.
  const auto code = rack->code;
  rack->componentType = type;
  logActivity("rack", code + " type " + previous + " -> " + type);
  saveState();
  syncRackSelection();
  setMessage(code + " type updated", 2);
}

void App::createRackWithType(const string& value) {
  const auto type = trim(value);
  if (type.empty()) {
    setMessage("Rack type cannot be empty", 3);
    return;
  }
  // A rack can be numbered INT_MAX, leaving no next code.
  long long nextNumber = 1;
  for (const auto& rack : store_.racks()) {
    nextNumber = max(nextNumber, static_cast<long long>(rackNumberFromCode(rack.code)) + 1);
  }
  if (nextNumber > numeric_limits<int>::max()) {
    setMessage("No further rack codes are available", 4, UiMessageSeverity::Warning);
    return;
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

// "Red case 4x6": the last word is rows x columns as seen on the Racks page, where numbered rows
// run down and lettered columns run across. Red case 4x6 has slots A1 to F4.
void App::createOrganizerFromInput(const string& value) {
  const auto text = trim(value);
  const auto space = text.find_last_of(' ');
  int rows = 0;     // stored rows: the slot letters
  int columns = 0;  // stored columns: the slot numbers
  if (space == string::npos || !parseOrganizerSize(text.substr(space + 1), rows, columns)) {
    return;
  }
  const auto name = trim(text.substr(0, space));
  string error;
  if (!validateStorageName(store_, name, string(), error)) {
    setMessage(error, 4, UiMessageSeverity::Warning);
    return;
  }
  // The category comes next, on the same prompt line.
  storagePendingName_ = name;
  storagePendingRows_ = rows;
  storagePendingColumns_ = columns;
  inputBuffer_.clear();
  inputMode_ = InputMode::OrganizerType;
  dirty_ = true;
}

void App::createPendingOrganizer(const string& componentType) {
  captureUndoSnapshot();
  string error;
  if (!createOrganizer(store_, storagePendingName_, storagePendingRows_, storagePendingColumns_, error,
                       componentType)) {
    setMessage(error, 4, UiMessageSeverity::Warning);
    return;
  }
  const auto created = store_.racks().back();
  rackFilter_.clear();
  const auto indices = sortedRackIndices();
  for (size_t position = 0; position < indices.size(); ++position) {
    if (store_.racks()[indices[position]].id == created.id) rackSelection_ = position;
  }
  rackRow_ = 0;
  rackColumn_ = 0;
  logActivity("rack", created.code + " organizer created, " + to_string(storagePendingColumns_) + "x" +
                          to_string(storagePendingRows_));
  saveState();
  syncRackSelection();
  setMessage(created.code + " created.", 5,
             UiMessageSeverity::Success);
}

vector<string> App::organizerTypeSuggestions(const string& typed) const {
  const auto wanted = toLower(trim(typed));
  vector<string> suggestions;
  if (wanted.empty()) return suggestions;
  unordered_set<string> seen;
  for (const auto& rack : store_.racks()) {
    const auto type = trim(rack.componentType);
    const auto key = toLower(type);
    if (key.empty() || !seen.insert(key).second || key.find(wanted) == string::npos) continue;
    suggestions.push_back(type);
  }
  sort(suggestions.begin(), suggestions.end(), [&](const string& lhs, const string& rhs) {
    const bool lhsPrefix = toLower(lhs).rfind(wanted, 0) == 0;
    if (lhsPrefix != (toLower(rhs).rfind(wanted, 0) == 0)) return lhsPrefix;
    return lhs < rhs;
  });
  if (suggestions.size() > 6) suggestions.resize(6);
  return suggestions;
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
  // Arm the confirmation popup; confirmRackDeletion performs the erase.
  rackDeleteConfirmationRackId_ = rack.id;
  rackDeleteConfirmationUntil_ = time(nullptr) + 3;
  dirty_ = true;
}

bool App::rackDeleteConfirmationActive() const {
  return !rackDeleteConfirmationRackId_.empty();
}

bool App::rackDeleteConfirmationReady() const {
  return rackDeleteConfirmationActive() && time(nullptr) >= rackDeleteConfirmationUntil_;
}

int App::rackDeleteConfirmationSecondsLeft() const {
  if (!rackDeleteConfirmationActive()) {
    return 0;
  }
  return max(0, static_cast<int>(rackDeleteConfirmationUntil_ - time(nullptr)));
}

void App::cancelRackDeletion() {
  if (rackDeleteConfirmationRackId_.empty()) {
    return;
  }

  rackDeleteConfirmationRackId_.clear();
  rackDeleteConfirmationUntil_ = 0;
  dirty_ = true;
}

void App::confirmRackDeletion() {
  if (!rackDeleteConfirmationActive()) {
    setMessage("Arm the deletion first with x or the Delete rack button", 3, UiMessageSeverity::Warning);
    return;
  }

  const auto secondsLeft = rackDeleteConfirmationSecondsLeft();
  if (!rackDeleteConfirmationReady()) {
    setMessage("Wait " + to_string(secondsLeft) + " more second" + (secondsLeft == 1 ? string() : string("s")) +
                   " to confirm delete",
               2, UiMessageSeverity::Warning);
    return;
  }

  const auto it = find_if(store_.racks().begin(), store_.racks().end(), [&](const InventatoryRack& candidate) {
    return candidate.id == rackDeleteConfirmationRackId_;
  });
  if (it == store_.racks().end()) {
    cancelRackDeletion();
    setMessage("Rack no longer available", 2, UiMessageSeverity::Warning);
    return;
  }

  if (rackOccupiedSlotCount(store_, *it) != 0) {
    cancelRackDeletion();
    setMessage("Only empty racks can be deleted", 3, UiMessageSeverity::Warning);
    return;
  }

  const auto code = it->code;
  captureUndoSnapshot();
  store_.racks().erase(it);
  if (rackSelection_ > 0) --rackSelection_;
  movingRackItemId_.clear();
  movingRackSource_.clear();
  cancelRackDeletion();
  logActivity("rack", code + " deleted");
  saveState();
  syncRackSelection();
  setMessage(code + " deleted", 2);
}

void App::jumpToRack(const string& value) {
  const auto typed = trim(value);
  if (typed.empty()) {
    return;
  }
  const auto indices = sortedRackIndices();
  const auto selectCode = [&](const string& code) {
    for (size_t position = 0; position < indices.size(); ++position) {
      if (toLower(store_.racks()[indices[position]].code) == toLower(trim(code))) {
        selectRackAt(position);
        return true;
      }
    }
    return false;
  };
  const auto goToSlot = [&](const string& slot) {
    const auto* rack = selectedRack();
    const auto wanted = toUpper(trim(slot));
    if (rack == nullptr || !isValidRackSlot(wanted) || !storageHasSlot(*rack, wanted)) return false;
    jumpToRackSlot(wanted.front() - 'A', stoi(wanted.substr(1)) - 1);
    return true;
  };
  // A storage name on its own, then a slot of the storage on screen, then a name followed by a slot.
  if (selectCode(typed)) {
    setMessage("Jumped to " + selectedRack()->code, 2);
    return;
  }
  if (goToSlot(typed)) {
    setMessage("Jumped to " + selectedRack()->code + "-" + selectedRackSlot(), 2);
    return;
  }
  string name;
  string slot;
  if (splitTrailingSlot(typed, name, slot) && findStorageByCode(store_, name) != nullptr) {
    const auto previous = rackSelection_;
    if (selectCode(name)) {
      if (goToSlot(slot)) {
        setMessage("Jumped to " + selectedRack()->code + "-" + selectedRackSlot(), 2);
        return;
      }
      setMessage(selectedRack()->code + " has no slot " + slot, 3);
      return;
    }
    rackSelection_ = previous;
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
  // Widen before adding: INT_MAX plus a positive delta must clamp, not overflow.
  item->quantity = static_cast<int>(
      clamp<long long>(static_cast<long long>(item->quantity) + delta, 0, numeric_limits<int>::max()));
  item->lastUpdated = time(nullptr);
  logActivity(delta > 0 ? "stock" : "usage", item->partName + " quantity changed to " + to_string(item->quantity));
  saveState();
  setMessage(item->partName + " quantity is now " + to_string(item->quantity), 2);
  dirty_ = true;
}

void App::openSelectedRackDatasheet() {
  const auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }
  openCurrentUrl(item->datasheetUrl, "datasheet");
}

void App::openSelectedRackItemDetail() {
  const auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }

  // Reset the stock filters if needed so the part shows in the detail panel.
  if (!selectStockItemById(item->id)) {
    setMessage("Selected part is no longer available", 2);
    return;
  }
  inputMode_ = InputMode::None;
  focusedTargetId_.clear();
  // As changePage does, drop a half-finished move and pending confirmations.
  movingRackItemId_.clear();
  movingRackSource_.clear();
  cancelDeleteConfirmation();
  cancelRackDeletion();
  page_ = Page::Stock;
  dirty_ = true;
}

// New storage from the Racks page: a button opens a short choice, then for a container with slots a form.
void App::beginStorageNew() {
  inputBuffer_.clear();
  inputMode_ = InputMode::StorageNew;
  dirty_ = true;
}

void App::beginStorageForm() {
  for (auto& value : storageFormValues_) value.clear();
  storageFormField_ = 0;
  suggestionIndex_ = -1;
  inputBuffer_.clear();
  inputMode_ = InputMode::StorageForm;
  dirty_ = true;
}

void App::handleStorageNewKey(const KeyEvent& key) {
  if (key.type == KeyType::Escape) {
    inputMode_ = InputMode::None;
    dirty_ = true;
  } else if (key.type == KeyType::Enter || (key.type == KeyType::Character && (key.ch == 'c' || key.ch == 'C'))) {
    beginStorageForm();
  } else if (key.type == KeyType::Character && (key.ch == 'r' || key.ch == 'R')) {
    inputBuffer_.clear();
    inputMode_ = InputMode::RackCreate;
    setMessage("What goes in the new rack?", 4);
  }
}

void App::handleStorageFormKey(const KeyEvent& key) {
  auto& field = storageFormField_;
  const auto move = [&](int step) {
    field = (field + step + 4) % 4;
    suggestionIndex_ = -1;
    dirty_ = true;
  };
  if (key.type == KeyType::Escape) {
    inputMode_ = InputMode::None;
    setMessage("New storage cancelled", 2);
    return;
  }
  if (key.type == KeyType::TabReverse || key.type == KeyType::Up) {
    move(-1);
    return;
  }
  if (key.type == KeyType::Down) {
    move(1);
    return;
  }
  if (key.type == KeyType::Tab) {
    // In the category, Tab steps through the categories racks already use; with none to offer it moves on.
    if (field == 3) {
      if (suggestionIndex_ < 0) suggestionBase_ = storageFormValues_[3];
      const auto suggestions = organizerTypeSuggestions(suggestionBase_);
      if (!suggestions.empty()) {
        suggestionIndex_ = suggestionIndex_ < 0 ? 0 : (suggestionIndex_ + 1) % static_cast<int>(suggestions.size());
        storageFormValues_[3] = suggestions[static_cast<size_t>(suggestionIndex_)];
        dirty_ = true;
        return;
      }
    }
    move(1);
    return;
  }
  if (key.type == KeyType::Enter) {
    if (field < 3) {
      move(1);
    } else {
      submitStorageForm();
    }
    return;
  }
  suggestionIndex_ = -1;
  auto& value = storageFormValues_[field];
  if (key.type == KeyType::Backspace) {
    if (!value.empty()) eraseLastCharacter(value);
    dirty_ = true;
  } else if (key.type == KeyType::Character) {
    // The two sizes are numbers of at most two digits.
    if (field == 1 || field == 2) {
      if (key.ch >= '0' && key.ch <= '9' && value.size() < 2) value += key.ch;
    } else {
      appendKeyText(value, key);
    }
    dirty_ = true;
  }
}

void App::submitStorageForm() {
  const auto name = trim(storageFormValues_[0]);
  string error;
  if (!validateStorageName(store_, name, string(), error)) {
    storageFormField_ = 0;
    setMessage(error, 4, UiMessageSeverity::Warning);
    return;
  }
  const auto number = [](const string& text, int maximum) {
    if (text.empty()) return 0;
    const int value = stoi(text);
    return value >= 1 && value <= maximum ? value : 0;
  };
  const int numbers = number(storageFormValues_[1], kOrganizerMaxColumns);  // rows down the screen
  const int letters = number(storageFormValues_[2], kOrganizerMaxRows);     // columns across the screen
  if (numbers == 0) {
    storageFormField_ = 1;
    setMessage("Rows: a number from 1 to " + to_string(kOrganizerMaxColumns), 4, UiMessageSeverity::Warning);
    return;
  }
  if (letters == 0) {
    storageFormField_ = 2;
    setMessage("Columns: a number from 1 to " + to_string(kOrganizerMaxRows), 4, UiMessageSeverity::Warning);
    return;
  }
  storagePendingName_ = name;
  storagePendingRows_ = letters;
  storagePendingColumns_ = numbers;
  createPendingOrganizer(storageFormValues_[3]);
  if (findStorageByCode(store_, name) != nullptr) inputMode_ = InputMode::None;
}

}  // namespace inventatory
