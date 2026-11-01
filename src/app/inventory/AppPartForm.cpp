// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - the part form: add a part by hand, copy one, or edit one.
//
// The form opens with the few fields every part has (name, quantity, storage, category, notes).
// A category the app knows, such as resistors, adds the two to four values that matter for it.
// Everything else waits behind "More details", so a part that is not electronic never sees it.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "core/inventory/InventoryMerge.h"
#include "core/inventory/VendorProvider.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <ctime>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace inventatory {

using namespace std;
using namespace app_actions;

namespace {

struct TypeFieldSpec {
  const char* label;
  vector<string> names;  // saved under the first, read from any
};

struct CategoryFields {
  vector<const char*> categoryWords;
  vector<TypeFieldSpec> fields;
};

// The names match the ones the detail panel and the closest-value search already read.
const vector<CategoryFields>& categoryFieldTable() {
  static const vector<string> package = {"Package / Case", "Package Case", "Case / Package", "Supplier Device Package",
                                         "Package"};
  static const vector<CategoryFields> table = {
      {{"resistor"},
       {{"Resistance", {"Resistance", "Value"}},
        {"Package", package},
        {"Tolerance", {"Tolerance"}},
        {"Power", {"Power (Watts)", "Power Dissipation", "Power Rating", "Power"}}}},
      {{"capacitor"},
       {{"Capacitance", {"Capacitance", "Value"}},
        {"Voltage", {"Voltage - Rated", "Rated Voltage", "Operating Voltage", "Voltage"}},
        {"Package", package},
        {"Tolerance", {"Tolerance"}}}},
      {{"inductor", "choke", "coil"},
       {{"Inductance", {"Inductance", "Value"}},
        {"Current", {"Current Rating (Amps)", "Current Rating", "Current"}},
        {"Package", package}}},
      {{"crystal", "oscillator", "resonator"}, {{"Frequency", {"Frequency"}}, {"Package", package}}},
      {{"led", "indicator"}, {{"Color", {"Color"}}, {"Package", package}}},
  };
  return table;
}

const CategoryFields* categoryFieldsFor(const string& category) {
  const auto lowered = toLower(category);
  for (const auto& group : categoryFieldTable()) {
    for (const auto* word : group.categoryWords) {
      if (lowered.find(word) != string::npos) return &group;
    }
  }
  return nullptr;
}

Parameter* findNamedParameter(vector<Parameter>& parameters, const vector<string>& names) {
  for (const auto& name : names) {
    for (auto& parameter : parameters) {
      if (toLower(trim(parameter.name)) == toLower(name)) return &parameter;
    }
  }
  return nullptr;
}

const Parameter* findNamedParameter(const vector<Parameter>& parameters, const vector<string>& names) {
  return findNamedParameter(const_cast<vector<Parameter>&>(parameters), names);
}

void setNamedParameter(vector<Parameter>& parameters, const vector<string>& names, const string& value) {
  auto* existing = findNamedParameter(parameters, names);
  if (value.empty()) {
    if (existing != nullptr) {
      parameters.erase(parameters.begin() + (existing - parameters.data()));
    }
    return;
  }
  if (existing != nullptr) {
    existing->value = value;
  } else {
    parameters.push_back({names.front(), value});
  }
}

bool sameParameters(const vector<Parameter>& lhs, const vector<Parameter>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t index = 0; index < lhs.size(); ++index) {
    if (lhs[index].name != rhs[index].name || lhs[index].value != rhs[index].value) return false;
  }
  return true;
}

// Fields a user can change in the form, compared to decide whether closing loses work.
bool sameFormFields(const InventoryItem& lhs, const InventoryItem& rhs) {
  return lhs.partName == rhs.partName && lhs.manufacturer == rhs.manufacturer && lhs.category == rhs.category &&
         lhs.quantity == rhs.quantity && lhs.location == rhs.location && lhs.rackId == rhs.rackId &&
         lhs.rackSlot == rhs.rackSlot && lhs.rackAssignment == rhs.rackAssignment && lhs.tags == rhs.tags &&
         sameParameters(lhs.parameters, rhs.parameters) && lhs.notes == rhs.notes &&
         lhs.labelOverride == rhs.labelOverride && lhs.digikeyPartNumber == rhs.digikeyPartNumber &&
         lhs.datasheetUrl == rhs.datasheetUrl && lhs.productUrl == rhs.productUrl && lhs.sku == rhs.sku &&
         lhs.vendorMetadata.manufacturerPartNumber == rhs.vendorMetadata.manufacturerPartNumber &&
         lhs.vendorMetadata.provider == rhs.vendorMetadata.provider &&
         lhs.vendorMetadata.providerProductNumber == rhs.vendorMetadata.providerProductNumber;
}

}  // namespace

vector<App::FieldOption> App::fieldOptions() const {
  vector<FieldOption> options = {
      {"Name", EditField::PartName},
      {"Quantity", EditField::Quantity},
      {"Storage", EditField::Storage},
      {"Category", EditField::Category},
  };
  if (const auto* group = categoryFieldsFor(workingCopy_.item.category)) {
    for (const auto& spec : group->fields) options.push_back({spec.label, EditField::TypeParameter, spec.names});
  }
  options.push_back({"Notes", EditField::Notes});
  options.push_back({editMoreDetails_ ? "Fewer details" : "More details", EditField::MoreDetails});
  if (editMoreDetails_) {
    options.push_back({"Manufacturer", EditField::Manufacturer});
    options.push_back({"Part number", EditField::PartNumber});
    options.push_back({"Reorder at", EditField::ReorderThreshold});
    options.push_back({"Vendor", EditField::VendorSource});
    options.push_back({"Parameters", EditField::Parameters});
    options.push_back({"Tags", EditField::Tags});
    options.push_back({"Label text", EditField::LabelOverride});
    options.push_back({"DigiKey part", EditField::DigiKeyPart});
    options.push_back({"TME part", EditField::TmeSymbol});
    options.push_back({"Datasheet URL", EditField::DatasheetUrl});
    options.push_back({"Product URL", EditField::ProductUrl});
    options.push_back({"SKU", EditField::Sku});
  }
  return options;
}

// Rebuilds the field list (a category or the details row changes it) and keeps the cursor on
// the same field.
void App::refreshFieldOptions() {
  const bool hadSelection = fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size());
  const auto previous = hadSelection ? menuOptions_[fieldMenuIndex_] : FieldOption{};
  menuOptions_ = fieldOptions();
  if (hadSelection) {
    for (size_t index = 0; index < menuOptions_.size(); ++index) {
      if (menuOptions_[index].field == previous.field && menuOptions_[index].parameterNames == previous.parameterNames) {
        fieldMenuIndex_ = static_cast<int>(index);
        return;
      }
    }
  }
  fieldMenuIndex_ = clamp(fieldMenuIndex_, 0, max(0, static_cast<int>(menuOptions_.size()) - 1));
}

string App::currentFieldValue(const FieldOption& option) const {
  const auto& item = workingCopy_.item;
  switch (option.field) {
    case EditField::PartName: return item.partName;
    case EditField::Manufacturer: return item.manufacturer;
    case EditField::Category: return item.category;
    case EditField::Quantity: return to_string(item.quantity);
    case EditField::ReorderThreshold: return to_string(item.reorderThreshold);
    case EditField::Location: return item.location;
    case EditField::Tags: return join(item.tags, ',');
    case EditField::Parameters: {
      ostringstream out;
      for (size_t index = 0; index < item.parameters.size(); ++index) {
        if (index > 0) out << "; ";
        out << item.parameters[index].name << '=' << item.parameters[index].value;
      }
      return out.str();
    }
    case EditField::Notes: return item.notes;
    case EditField::LabelOverride: return item.labelOverride;
    case EditField::DigiKeyPart: return item.digikeyPartNumber;
    case EditField::TmeSymbol: return tmeProductSymbol(item);
    case EditField::DatasheetUrl: return item.datasheetUrl;
    case EditField::ProductUrl: return item.productUrl;
    case EditField::Sku: return item.sku;
    case EditField::PartNumber: return item.vendorMetadata.manufacturerPartNumber;
    case EditField::VendorSource: {
      const auto vendorName = vendorDisplayName(originProvider(item));
      return vendorName.empty() ? string("look up") : vendorName;
    }
    case EditField::RackLocation:
    case EditField::Storage: {
      const auto location = storageLocation(item, store_.racks());
      if (!location.empty()) return location;
      return item.rackAssignment == RackAssignmentMode::Automatic ? string("AUTO") : string();
    }
    case EditField::TypeParameter: {
      const auto* parameter = findNamedParameter(item.parameters, option.parameterNames);
      return parameter == nullptr ? string() : parameter->value;
    }
    case EditField::MoreDetails: return {};
  }
  return {};
}

bool App::commitEditField(const FieldOption& option, const string& value) {
  const auto trimmed = trim(value);
  auto& item = workingCopy_.item;
  string notice;

  switch (option.field) {
    case EditField::PartName: item.partName = trimmed; break;
    case EditField::Manufacturer: item.manufacturer = trimmed; break;
    case EditField::Category: item.category = trimmed; break;
    case EditField::Quantity:
      if (trimmed.empty()) {
        item.quantity = 0;
      } else if (!parseIntegerInRange(trimmed, 0, (numeric_limits<int>::max)(), item.quantity)) {
        setMessage("Quantity is a whole number, such as 12", 4, UiMessageSeverity::Warning);
        return false;
      }
      break;
    case EditField::ReorderThreshold:
      if (trimmed.empty()) {
        item.reorderThreshold = 0;
      } else if (!parseIntegerInRange(trimmed, 0, (numeric_limits<int>::max)(), item.reorderThreshold)) {
        setMessage("Reorder at is a whole number.", 4,
                   UiMessageSeverity::Warning);
        return false;
      }
      break;
    case EditField::Location: item.location = trimmed; break;
    case EditField::Tags: item.tags = splitFlexible(trimmed); break;
    case EditField::Parameters: item.parameters = parseParameters(trimmed); break;
    case EditField::Notes: item.notes = trimmed; break;
    case EditField::LabelOverride: item.labelOverride = trimmed; break;
    case EditField::DigiKeyPart: item.digikeyPartNumber = trimmed; break;
    case EditField::TmeSymbol:
      if (!setTmeSymbol(item, trimmed)) {
        setMessage("A TME part number is letters, digits and - _ . / + # only, up to 64 characters", 4,
                   UiMessageSeverity::Warning);
        return false;
      }
      break;
    case EditField::DatasheetUrl: item.datasheetUrl = trimmed; break;
    case EditField::ProductUrl: item.productUrl = trimmed; break;
    case EditField::Sku: item.sku = trimmed; break;
    case EditField::PartNumber: item.vendorMetadata.manufacturerPartNumber = trimmed; break;
    case EditField::TypeParameter: setNamedParameter(item.parameters, option.parameterNames, trimmed); break;
    case EditField::RackLocation:
    case EditField::Storage: {
      StorageChoice choice;
      string error;
      if (!resolveStorageInput(store_, item, trimmed, choice, error)) {
        setMessage(error, 5, UiMessageSeverity::Warning);
        return false;
      }
      applyStorageChoice(item, choice);
      if (choice.pickedFreeSlot) notice = "Storage: " + choice.display + ", the first free slot";
      else if (choice.newPlace) notice = "New place: " + choice.place;
      break;
    }
    case EditField::VendorSource:
    case EditField::MoreDetails:
      return false;  // not typed
  }

  item.lastUpdated = time(nullptr);
  editDiscardArmed_ = false;
  if (option.field == EditField::Category) refreshFieldOptions();
  if (!notice.empty()) setMessage(notice, 4, UiMessageSeverity::Success);
  dirty_ = true;
  return true;
}

void App::beginEditFieldAt(int index) {
  if (index < 0 || index >= static_cast<int>(menuOptions_.size())) return;
  fieldMenuIndex_ = index;
  const auto option = menuOptions_[index];
  editDiscardArmed_ = false;
  if (option.field == EditField::VendorSource) {
    inputMode_ = InputMode::EditFieldMenu;
    beginVendorLookup();
    return;
  }
  if (option.field == EditField::MoreDetails) {
    editMoreDetails_ = !editMoreDetails_;
    inputMode_ = InputMode::EditFieldMenu;
    refreshFieldOptions();
    dirty_ = true;
    return;
  }
  inputBuffer_ = currentFieldValue(option);
  suggestionIndex_ = -1;
  // A zero quantity or an AUTO storage is a default, not something to edit around.
  if (((option.field == EditField::Quantity || option.field == EditField::ReorderThreshold) && inputBuffer_ == "0") ||
      (option.field == EditField::Storage && inputBuffer_ == "AUTO")) {
    inputBuffer_.clear();
  }
  // The old value is shown selected: typing replaces it, Backspace edits its end.
  inputReplaceOnType_ = !inputBuffer_.empty();
  inputMode_ = InputMode::EditValue;
  dirty_ = true;
}

namespace {

InventoryItem blankManualPart(const InventoryStore& store) {
  InventoryItem item;
  item.id = makeId();
  item.syncStatus = kManualSyncStatus;
  // With racks set up, a new part may go to one by itself; without racks there is nothing to pick.
  item.rackAssignment = store.racks().empty() ? RackAssignmentMode::Unassigned : RackAssignmentMode::Automatic;
  item.lastUpdated = time(nullptr);
  item.createdAt = item.lastUpdated;
  return item;
}

}  // namespace

void App::beginEditCurrentItem(bool createNew) {
  editingImportCandidate_ = false;
  partFormFromRacks_ = false;
  page_ = Page::Stock;
  workingCopy_ = {};
  editMoreDetails_ = false;
  editDiscardArmed_ = false;
  if (createNew) {
    workingCopy_.isNew = true;
    workingCopy_.item = blankManualPart(store_);
    workingCopy_.original = workingCopy_.item;
    workingCopy_.originalIndex = store_.items().size();
  } else {
    const auto* current = selectedItem();
    if (current == nullptr) {
      setMessage("No item selected", 2);
      return;
    }
    workingCopy_.item = *current;
    workingCopy_.original = *current;
    workingCopy_.originalIndex = selectedIndex();
  }

  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  if (createNew) {
    beginEditFieldAt(0);
    setMessage("New part: type its name.", 5);
  } else {
    inputMode_ = InputMode::EditFieldMenu;
    setMessage("Editing " + workingCopy_.item.partName, 3);
  }
}

void App::beginNewPartInRack() {
  const auto* rack = selectedRack();
  string where;
  if (rack != nullptr) {
    const auto slot = rackSlotLabel(rackRow_, rackColumn_);
    where = itemAtRackSlot(store_, rack->id, slot) == nullptr ? rack->code + " " + slot : rack->code;
  }
  beginEditCurrentItem(true);
  if (where.empty()) return;
  StorageChoice choice;
  string error;
  if (resolveStorageInput(store_, workingCopy_.item, where, choice, error)) {
    applyStorageChoice(workingCopy_.item, choice);
    workingCopy_.original = workingCopy_.item;
    menuOptions_ = fieldOptions();
  }
  partFormFromRacks_ = true;
}

// Ends the part form on the page it was opened from, showing the saved part's slot when it has one.
void App::leavePartFormPage(const InventoryItem* shown) {
  if (!partFormFromRacks_) {
    page_ = Page::Stock;
    return;
  }
  partFormFromRacks_ = false;
  page_ = Page::Racks;
  if (shown == nullptr || shown->rackId.empty()) return;
  const auto indices = sortedRackIndices();
  for (size_t position = 0; position < indices.size(); ++position) {
    if (store_.racks()[indices[position]].id != shown->rackId) continue;
    selectRackAt(position);
    const auto& slot = shown->rackSlot;
    if (!slot.empty() && isValidRackSlot(slot)) {
      jumpToRackSlot(slot[0] - 'A', max(0, atoi(slot.c_str() + 1) - 1));
    }
    return;
  }
}

// Starts a new part from the selected one: what it is carries over, where and how many do not,
// and nothing that names one vendor product does.
void App::beginCopyCurrentItem() {
  const auto* current = selectedItem();
  if (current == nullptr) {
    setMessage("No item selected", 2);
    return;
  }
  const auto source = *current;
  beginEditCurrentItem(true);
  auto& item = workingCopy_.item;
  item.partName = source.partName;
  item.manufacturer = source.manufacturer;
  item.category = source.category;
  item.tags = source.tags;
  item.parameters = source.parameters;
  item.notes = source.notes;
  if (source.rackId.empty() && !isPlaceholderLocation(source.location)) item.location = source.location;
  workingCopy_.copiedFrom = source.partName;
  refreshFieldOptions();
  beginEditFieldAt(0);
  setMessage("Copy of " + source.partName + ", change what differs then save", 5);
}

void App::beginEditImportCandidate() {
  auto* candidate = currentImportCandidate();
  if (candidate == nullptr) {
    setMessage("No import row selected", 2);
    return;
  }

  editingImportCandidate_ = true;
  editMoreDetails_ = false;
  editDiscardArmed_ = false;
  importEditIndex_ = importSelection_;
  workingCopy_ = {};
  workingCopy_.item = candidate->item;
  workingCopy_.original = candidate->item;
  workingCopy_.originalIndex = importSelection_;
  menuOptions_ = fieldOptions();
  fieldMenuIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
  page_ = Page::Import;
  setMessage("Editing import row: Up/Down field, Enter edit, s save, Esc cancel", 4);
}

bool App::workingCopyChanged() const {
  return !sameFormFields(workingCopy_.item, workingCopy_.original);
}

// Esc on the form. Unsaved changes need a second Esc, so one stray key never loses a typed part.
void App::closePartEditor() {
  if (workingCopyChanged() && !editDiscardArmed_) {
    editDiscardArmed_ = true;
    setMessage("Not saved yet: press s to save, or Esc again to discard", 6, UiMessageSeverity::Warning);
    dirty_ = true;
    return;
  }
  const bool discarded = workingCopyChanged();
  editDiscardArmed_ = false;
  editMoreDetails_ = false;
  cancelInput();
  if (partFormFromRacks_) leavePartFormPage(nullptr);
  if (discarded) setMessage(workingCopy_.isNew ? "New part discarded" : "Changes discarded", 3);
}

void App::saveWorkingCopy() {
  editDiscardArmed_ = false;
  if (editingImportCandidate_) {
    if (importEditIndex_ < importCandidates_.size()) {
      importCandidates_[importEditIndex_].item = workingCopy_.item;
    }

    editingImportCandidate_ = false;
    editMoreDetails_ = false;
    inputMode_ = InputMode::None;
    page_ = Page::Import;
    setMessage("Import row updated", 2, UiMessageSeverity::Success);
    dirty_ = true;
    return;
  }

  if (trim(workingCopy_.item.partName).empty()) {
    beginEditFieldAt(0);
    setMessage("Part needs a name before saving", 4, UiMessageSeverity::Warning);
    return;
  }

  // The scanner may change store_ while the form is open, so the item is found by id and only the
  // user's changed fields are replayed onto it.
  vector<string> mergeNotices;
  string addedSummary;
  if (workingCopy_.isNew) {
    if (!hasVendorRecord(workingCopy_.item)) workingCopy_.item.syncStatus = kManualSyncStatus;
    captureUndoSnapshot();
    store_.items().push_back(workingCopy_.item);
    reconcileRackAssignment(store_, store_.items().back());
    const auto where = storageLocation(store_.items().back(), store_.racks());
    addedSummary = "Added " + workingCopy_.item.partName +
                   (where.empty() ? string(".") : " to " + where + ". Press p to print its label");
    selectStockItemById(workingCopy_.item.id, true);
  } else if (auto* live = store_.findById(workingCopy_.item.id)) {
    captureUndoSnapshot();
    *live = mergeEditedItem(workingCopy_.original, workingCopy_.item, *live, QuantityMerge::PreferEdited,
                            &mergeNotices);
    releaseConflictingRackPlacement(store_, *live, &mergeNotices);
    reconcileRackAssignment(store_, *live);
  } else {
    inputMode_ = InputMode::None;
    page_ = Page::Stock;
    syncSelectionToFilter();
    setMessage(workingCopy_.item.partName + " no longer exists.", 5,
               UiMessageSeverity::Warning);
    return;
  }

  logActivity(workingCopy_.isNew ? "add" : "edit",
              workingCopy_.item.partName + (workingCopy_.isNew ? " added" : " updated"));
  for (const auto& notice : mergeNotices) logActivity("edit conflict", notice);
  saveState();
  inputMode_ = InputMode::None;
  editMoreDetails_ = false;
  leavePartFormPage(workingCopy_.isNew ? store_.findById(workingCopy_.item.id) : nullptr);
  syncSelectionToFilter();
  if (!addedSummary.empty()) {
    setMessage(addedSummary, 6, UiMessageSeverity::Success);
  } else if (mergeNotices.empty()) {
    setMessage("Changes saved", 2, UiMessageSeverity::Success);
  } else {
    setMessage("Changes saved. " + mergeNotices.front() +
                   (mergeNotices.size() > 1 ? " (+" + to_string(mergeNotices.size() - 1) + " more in Activity)" : string()),
               6, UiMessageSeverity::Warning);
  }
}

vector<string> App::editFieldSuggestions() const {
  if (inputMode_ != InputMode::EditValue || fieldMenuIndex_ < 0 ||
      fieldMenuIndex_ >= static_cast<int>(menuOptions_.size())) {
    return {};
  }
  const auto field = menuOptions_[fieldMenuIndex_].field;
  // While Tab steps through the list, the list stays the one for what was typed.
  const auto& base = suggestionIndex_ >= 0 ? suggestionBase_ : inputBuffer_;
  const auto typed = toLower(trim(base));
  // An empty field shows no list, so it never looks like a menu to pick from.
  if (typed.empty()) return {};
  vector<string> suggestions;
  if (field == EditField::Storage) {
    suggestions = storageSuggestions(store_, base, 6);
  } else if (field == EditField::Category) {
    vector<string> prefixHits;
    vector<string> innerHits;
    unordered_set<string> seen;
    for (const auto& item : store_.items()) {
      const auto category = trim(item.category);
      const auto key = toLower(category);
      if (key.empty() || key == "unsorted" || !seen.insert(key).second) continue;
      if (typed.empty() || key.rfind(typed, 0) == 0) prefixHits.push_back(category);
      else if (key.find(typed) != string::npos) innerHits.push_back(category);
    }
    sort(prefixHits.begin(), prefixHits.end());
    sort(innerHits.begin(), innerHits.end());
    prefixHits.insert(prefixHits.end(), innerHits.begin(), innerHits.end());
    suggestions = move(prefixHits);
    if (suggestions.size() > 6) suggestions.resize(6);
  }
  // What is already typed in full is not a suggestion.
  suggestions.erase(remove_if(suggestions.begin(), suggestions.end(),
                              [&](const string& value) { return toLower(value) == typed; }),
                    suggestions.end());
  return suggestions;
}

// While typing a rack or organizer name in Storage, its free slots.
string App::editFieldSlotHint() const {
  if (inputMode_ != InputMode::EditValue || fieldMenuIndex_ < 0 ||
      fieldMenuIndex_ >= static_cast<int>(menuOptions_.size()) ||
      menuOptions_[fieldMenuIndex_].field != EditField::Storage) {
    return {};
  }
  // Only once a space or dash follows an existing name, where a slot comes next. While a new
  // name is typed, the free slots of a storage it starts with would only confuse.
  const auto at = inputBuffer_.find_last_of("- ");
  if (at == string::npos) return {};
  const auto code = trim(inputBuffer_.substr(0, at));
  if (findStorageByCode(store_, code) == nullptr) return {};
  const auto free = freeStorageSlots(store_, code, &workingCopy_.item, 9);
  if (free.empty()) return code + " is full";
  string hint = "Free in " + code + ":";
  for (size_t index = 0; index < free.size() && index < 8; ++index) hint += " " + free[index];
  if (free.size() > 8) hint += " ...";
  return hint;
}

string App::partFormFooterHint() const {
  if (inputMode_ == InputMode::EditValue) {
    const bool lastNewField = workingCopy_.isNew && !editingImportCandidate_ && fieldMenuIndex_ + 1 < static_cast<int>(menuOptions_.size()) &&
                              menuOptions_[fieldMenuIndex_ + 1].field == EditField::MoreDetails;
    string hint = lastNewField ? " Enter save" : (workingCopy_.isNew && !editingImportCandidate_ ? " Enter next" : " Enter done");
    // The panel is about 40 columns wide at the smallest window, so the hint stays short.
    hint += editFieldSuggestions().empty() ? "  Up/Down field" : "  Tab suggestions";
    return hint + "  Esc undo";
  }
  if (inputMode_ == InputMode::StorageKindChoice) return " Enter place  g grid  Esc back";
  if (inputMode_ == InputMode::StorageGridSize) return " Enter next  Esc back";
  if (inputMode_ == InputMode::StorageGridType) {
    return organizerTypeSuggestions(suggestionIndex_ >= 0 ? suggestionBase_ : inputBuffer_).empty()
               ? " Enter create  Esc back"
               : " Enter create  Tab suggestions  Esc back";
  }
  if (editDiscardArmed_) return " s save  Esc discard";
  return " Enter edit  s save  Esc close";
}

void App::handleEditMenuKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (key.ch == 's' || key.ch == 'S') saveWorkingCopy();
    return;
  }

  if (key.type == KeyType::Up) {
    fieldMenuIndex_ = max(0, fieldMenuIndex_ - 1);
    editDiscardArmed_ = false;
    dirty_ = true;
  } else if (key.type == KeyType::Down) {
    fieldMenuIndex_ = min(fieldMenuIndex_ + 1, static_cast<int>(menuOptions_.size()) - 1);
    editDiscardArmed_ = false;
    dirty_ = true;
  } else if (key.type == KeyType::Enter) {
    beginEditFieldAt(fieldMenuIndex_);
  } else if (key.type == KeyType::Escape) {
    closePartEditor();
  }
}

void App::handleEditValueKey(const KeyEvent& key) {
  const bool replacePending = inputReplaceOnType_;
  inputReplaceOnType_ = false;
  if (key.type != KeyType::Tab) suggestionIndex_ = -1;
  if (key.type == KeyType::Character) {
    if (replacePending) inputBuffer_.clear();
    appendKeyText(inputBuffer_, key);
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      eraseLastCharacter(inputBuffer_);
      dirty_ = true;
    }
    return;
  }

  // Each Tab puts the next suggestion in the field.
  if (key.type == KeyType::Tab) {
    if (suggestionIndex_ < 0) suggestionBase_ = inputBuffer_;
    const auto suggestions = editFieldSuggestions();
    if (!suggestions.empty()) {
      suggestionIndex_ = suggestionIndex_ < 0 ? 0 : (suggestionIndex_ + 1) % static_cast<int>(suggestions.size());
      inputBuffer_ = suggestions[static_cast<size_t>(suggestionIndex_)];
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::EditFieldMenu;
    dirty_ = true;
    return;
  }

  const bool enter = key.type == KeyType::Enter;
  const bool up = key.type == KeyType::Up;
  const bool down = key.type == KeyType::Down;
  if (!enter && !up && !down) return;
  if (fieldMenuIndex_ < 0 || fieldMenuIndex_ >= static_cast<int>(menuOptions_.size())) {
    inputMode_ = InputMode::EditFieldMenu;
    return;
  }
  const int move = enter ? 0 : (up ? -1 : 1);

  // A storage name nobody uses yet could be a drawer or an organizer with slots, so ask.
  if (menuOptions_[fieldMenuIndex_].field == EditField::Storage) {
    StorageChoice choice;
    string error;
    if (resolveStorageInput(store_, workingCopy_.item, inputBuffer_, choice, error) &&
        choice.kind == StorageChoice::Kind::Place && choice.newPlace) {
      storagePendingName_ = choice.place;
      storagePendingSlot_.clear();
      // "Tray2 A2": a new name followed by a slot. The slot is where the part goes once the grid exists.
      string name;
      string slot;
      if (splitTrailingSlot(choice.place, name, slot) && findStorageByCode(store_, name) == nullptr) {
        storagePendingName_ = name;
        storagePendingSlot_ = slot;
      }
      storagePendingMove_ = move;
      inputMode_ = InputMode::StorageKindChoice;
      dirty_ = true;
      return;
    }
  }

  // A value that does not fit stays in the field so it can be corrected.
  if (!commitEditField(menuOptions_[fieldMenuIndex_], inputBuffer_)) return;
  finishEditValueMove(move);
}

// After a field is kept: Up/Down open the field above or below; Enter on a new part opens the
// next field, and on the last main field saves the part.
void App::finishEditValueMove(int move) {
  inputBuffer_.clear();
  inputMode_ = InputMode::EditFieldMenu;
  const int count = static_cast<int>(menuOptions_.size());
  // Fields typed as text. The vendor row starts a lookup and the details row toggles.
  const auto isTypedField = [](EditField field) {
    return field != EditField::VendorSource && field != EditField::MoreDetails;
  };
  if (move != 0) {
    const int next = clamp(fieldMenuIndex_ + move, 0, count - 1);
    if (isTypedField(menuOptions_[next].field)) beginEditFieldAt(next);
    else fieldMenuIndex_ = next;
    return;
  }

  if (!workingCopy_.isNew || editingImportCandidate_) return;
  const int next = fieldMenuIndex_ + 1;
  if (next >= count) return;
  if (menuOptions_[next].field == EditField::MoreDetails) {
    saveWorkingCopy();
    return;
  }
  if (isTypedField(menuOptions_[next].field)) beginEditFieldAt(next);
  else fieldMenuIndex_ = next;
}

// The question under Storage for a new name: Enter keeps it as a place; g asks for a grid size and
// makes it an organizer, then puts the part in its first slot.
void App::handleStorageChoiceKey(const KeyEvent& key) {
  if (key.type == KeyType::Escape && inputMode_ == InputMode::StorageGridType) {
    inputBuffer_ = storagePendingSize_;
    inputMode_ = InputMode::StorageGridSize;
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Escape) {
    // Back to the name as typed.
    inputBuffer_ = storagePendingSlot_.empty() ? storagePendingName_ : storagePendingName_ + " " + storagePendingSlot_;
    inputMode_ = InputMode::EditValue;
    dirty_ = true;
    return;
  }

  if (inputMode_ == InputMode::StorageKindChoice) {
    if (key.type == KeyType::Enter) {
      if (!commitEditField(menuOptions_[fieldMenuIndex_], storagePendingName_)) return;
      finishEditValueMove(storagePendingMove_);
    } else if (key.type == KeyType::Character && (key.ch == 'g' || key.ch == 'G')) {
      inputBuffer_.clear();
      inputMode_ = InputMode::StorageGridSize;
      dirty_ = true;
    }
    return;
  }

  // Typing in the size and category steps; Tab steps through categories already on racks.
  if (key.type == KeyType::Tab && inputMode_ == InputMode::StorageGridType) {
    if (suggestionIndex_ < 0) suggestionBase_ = inputBuffer_;
    const auto suggestions = organizerTypeSuggestions(suggestionBase_);
    if (!suggestions.empty()) {
      suggestionIndex_ = suggestionIndex_ < 0 ? 0 : (suggestionIndex_ + 1) % static_cast<int>(suggestions.size());
      inputBuffer_ = suggestions[static_cast<size_t>(suggestionIndex_)];
      dirty_ = true;
    }
    return;
  }
  suggestionIndex_ = -1;
  if (key.type == KeyType::Character) {
    appendKeyText(inputBuffer_, key);
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) eraseLastCharacter(inputBuffer_);
    dirty_ = true;
    return;
  }
  if (key.type != KeyType::Enter) return;

  if (inputMode_ == InputMode::StorageGridSize) {
    int rows = 0;
    int columns = 0;
    if (!parseOrganizerSize(inputBuffer_, rows, columns)) {
      setMessage("Type rows x columns, such as 4x6. Up to " + to_string(kOrganizerMaxColumns) + "x" +
                     to_string(kOrganizerMaxRows),
                 5, UiMessageSeverity::Warning);
      return;
    }
    if (!storagePendingSlot_.empty()) {
      InventatoryRack sized;
      sized.code = storagePendingName_;
      sized.rows = rows;
      sized.columns = columns;
      if (!storageHasSlot(sized, storagePendingSlot_)) {
        setMessage("A " + to_string(columns) + "x" + to_string(rows) + " grid has no slot " + storagePendingSlot_ + ".",
                   5, UiMessageSeverity::Warning);
        return;
      }
    }
    storagePendingRows_ = rows;
    storagePendingColumns_ = columns;
    storagePendingSize_ = inputBuffer_;
    inputBuffer_.clear();
    inputMode_ = InputMode::StorageGridType;
    dirty_ = true;
    return;
  }

  // StorageGridType: the organizer is storage in its own right, so it is saved now, even if this
  // part is not.
  captureUndoSnapshot();
  string error;
  if (!createOrganizer(store_, storagePendingName_, storagePendingRows_, storagePendingColumns_, error,
                       inputBuffer_)) {
    setMessage(error, 5, UiMessageSeverity::Warning);
    return;
  }
  logActivity("rack", storagePendingName_ + " organizer created, " + to_string(storagePendingColumns_) + "x" +
                          to_string(storagePendingRows_));
  saveState();
  const auto target = storagePendingSlot_.empty() ? storagePendingName_ : storagePendingName_ + "-" + storagePendingSlot_;
  if (!commitEditField(menuOptions_[fieldMenuIndex_], target)) return;
  setMessage(storagePendingName_ + " created with " + to_string(storagePendingColumns_) + " rows and " +
                 to_string(storagePendingRows_) + " columns. This part goes to " +
                 storageLocation(workingCopy_.item, store_.racks()),
             6, UiMessageSeverity::Success);
  finishEditValueMove(storagePendingMove_);
}

}  // namespace inventatory
