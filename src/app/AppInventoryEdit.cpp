// Inventatory - inventory search, edit, quantity, and activity actions.

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
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>

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
