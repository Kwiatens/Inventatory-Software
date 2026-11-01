// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - one Storage field for racks, organizers and named places.

#include "core/inventory/Inventory.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <unordered_set>

namespace inventatory {

using namespace std;

namespace {

size_t utf8Length(const string& text) {
  return static_cast<size_t>(count_if(text.begin(), text.end(), [](char ch) {
    return (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
  }));
}

bool slotTaken(const InventoryStore& store, const string& rackId, const string& slot, const InventoryItem* except) {
  const auto normalized = toUpper(trim(slot));
  return any_of(store.items().begin(), store.items().end(), [&](const InventoryItem& candidate) {
    if (except != nullptr && candidate.id == except->id) return false;
    return candidate.rackId == rackId && toUpper(trim(candidate.rackSlot)) == normalized;
  });
}

// "R12" and "r12-a1" look like rack codes; such a name cannot be a place or an organizer.
bool looksLikeRackCode(const string& text) {
  const auto code = trim(text.substr(0, text.find_first_of("- ")));
  return code.size() >= 2 && (code.front() == 'R' || code.front() == 'r') &&
         all_of(code.begin() + 1, code.end(), [](char ch) { return isdigit(static_cast<unsigned char>(ch)) != 0; });
}

// The spelling of an existing place that matches `text` regardless of case, else empty.
string existingPlace(const InventoryStore& store, const string& text, const InventoryItem* except) {
  const auto wanted = toLower(trim(text));
  for (const auto& item : store.items()) {
    if (except != nullptr && item.id == except->id) continue;
    if (isPlaceholderLocation(item.location)) continue;
    if (toLower(trim(item.location)) == wanted) return trim(item.location);
  }
  return {};
}

}  // namespace

bool isPlaceholderLocation(const string& location) {
  const auto value = toLower(trim(location));
  return value.empty() || value == "unassigned" || value == "scan inbox" || value == "import inbox";
}

string storageLocation(const InventoryItem& item, const vector<InventatoryRack>& racks) {
  const auto slot = rackLocation(item, racks);
  if (!slot.empty()) return slot;
  return isPlaceholderLocation(item.location) ? string() : trim(item.location);
}

const InventatoryRack* findStorageByCode(const InventoryStore& store, const string& code) {
  const auto wanted = toLower(trim(code));
  if (wanted.empty()) return nullptr;
  for (const auto& rack : store.racks()) {
    if (toLower(trim(rack.code)) == wanted) return &rack;
  }
  return nullptr;
}

bool parseOrganizerSize(const string& text, int& letters, int& numbers) {
  const auto size = toLower(trim(text));
  const auto times = size.find('x');
  if (times == string::npos) return false;
  const auto number = [](const string& part, int maximum, int& value) {
    const auto digits = trim(part);
    if (digits.empty() || digits.size() > 3 ||
        !all_of(digits.begin(), digits.end(), [](char ch) { return isdigit(static_cast<unsigned char>(ch)) != 0; })) {
      return false;
    }
    value = stoi(digits);
    return value >= 1 && value <= maximum;
  };
  return number(size.substr(0, times), kOrganizerMaxColumns, numbers) &&
         number(size.substr(times + 1), kOrganizerMaxRows, letters);
}

bool validateStorageName(const InventoryStore& store, const string& name, const string& exceptId, string& error) {
  error.clear();
  const auto code = trim(name);
  if (code.empty()) {
    error = "Give the organizer a name";
    return false;
  }
  if (utf8Length(code) > kStorageNameMaxLength) {
    error = "An organizer name is at most " + to_string(kStorageNameMaxLength) + " characters";
    return false;
  }
  if (looksLikeRackCode(code)) {
    error = "Names like R1 are reserved for racks, choose a different name.";
    return false;
  }
  if (toLower(code) == "auto") {
    error = "AUTO is kept for automatic placement, choose a different name";
    return false;
  }
  if (const auto* existing = findStorageByCode(store, code); existing != nullptr && existing->id != exceptId) {
    error = code + " already exists";
    return false;
  }
  return true;
}

bool createOrganizer(InventoryStore& store, const string& name, int rows, int columns, string& error,
                     const string& componentType) {
  const auto code = trim(name);
  if (!validateStorageName(store, code, string(), error)) return false;
  if (rows < 1 || rows > kOrganizerMaxRows || columns < 1 || columns > kOrganizerMaxColumns) {
    // Slot letters are columns on screen (A-Z) and slot numbers are rows (1-99).
    error = "An organizer has up to " + to_string(kOrganizerMaxColumns) + " rows (1-" +
            to_string(kOrganizerMaxColumns) + ") and " + to_string(kOrganizerMaxRows) + " columns (A-" +
            string(1, static_cast<char>('A' + kOrganizerMaxRows - 1)) + ")";
    return false;
  }
  InventatoryRack organizer;
  organizer.id = makeId();
  organizer.code = code;
  organizer.componentType = trim(componentType);
  organizer.rows = rows;
  organizer.columns = columns;
  organizer.createdAt = time(nullptr);
  store.racks().push_back(organizer);
  return true;
}

vector<string> freeStorageSlots(const InventoryStore& store, const string& code, const InventoryItem* except,
                                size_t limit) {
  vector<string> slots;
  const auto* rack = findStorageByCode(store, code);
  if (rack == nullptr) return slots;
  unordered_set<string> taken;
  for (const auto& item : store.items()) {
    if (except != nullptr && item.id == except->id) continue;
    if (item.rackId == rack->id) taken.insert(toUpper(trim(item.rackSlot)));
  }
  const int rows = storageGridRows(*rack);
  const int columns = storageGridColumns(*rack);
  for (int row = 0; row < rows && slots.size() < limit; ++row) {
    for (int column = 0; column < columns && slots.size() < limit; ++column) {
      auto slot = rackSlotLabel(row, column);
      if (taken.count(slot) == 0) slots.push_back(move(slot));
    }
  }
  return slots;
}

bool resolveStorageInput(const InventoryStore& store, const InventoryItem& item, const string& text,
                         StorageChoice& choice, string& error) {
  choice = {};
  error.clear();
  const auto typed = trim(text);
  if (typed.empty()) {
    choice.kind = StorageChoice::Kind::NotStored;
    return true;
  }
  if (toLower(typed) == "auto") {
    choice.kind = StorageChoice::Kind::Automatic;
    choice.display = "AUTO";
    return true;
  }

  const auto placeIn = [&](const InventatoryRack& rack, const string& requestedSlot) {
    auto slot = toUpper(trim(requestedSlot));
    if (slot.empty()) {
      // Keep the part's own slot when it is already in this storage.
      if (item.rackId == rack.id && !trim(item.rackSlot).empty()) {
        slot = toUpper(trim(item.rackSlot));
      } else {
        const auto free = freeStorageSlots(store, rack.code, &item, 1);
        if (free.empty()) {
          error = rack.code + " is full";
          return false;
        }
        slot = free.front();
        choice.pickedFreeSlot = true;
      }
    } else if (!isValidRackSlot(slot)) {
      error = "A slot is a letter and a number, such as B4";
      return false;
    } else if (!storageHasSlot(rack, slot)) {
      error = rack.code + " has no slot " + slot;
      return false;
    } else if (slotTaken(store, rack.id, slot, &item)) {
      error = rack.code + "-" + slot + " already holds another part";
      return false;
    }
    choice.kind = StorageChoice::Kind::Slot;
    choice.rackId = rack.id;
    choice.slot = slot;
    choice.display = rack.code + "-" + slot;
    return true;
  };

  if (const auto* rack = findStorageByCode(store, typed)) return placeIn(*rack, string());
  // The slot follows the name after a dash or a space: "R10-A2", "R10 A2", "Red case B4".
  for (const char separator : {'-', ' '}) {
    const auto at = typed.rfind(separator);
    if (at == string::npos) continue;
    if (const auto* rack = findStorageByCode(store, trim(typed.substr(0, at)))) {
      return placeIn(*rack, typed.substr(at + 1));
    }
  }
  // A rack code written together with its slot: "R10A2".
  if (typed.size() >= 4 && (typed.front() == 'R' || typed.front() == 'r')) {
    size_t digitsEnd = 1;
    while (digitsEnd < typed.size() && isdigit(static_cast<unsigned char>(typed[digitsEnd]))) ++digitsEnd;
    if (digitsEnd > 1 && digitsEnd < typed.size() && isalpha(static_cast<unsigned char>(typed[digitsEnd])) &&
        isValidRackSlot(typed.substr(digitsEnd))) {
      if (const auto* rack = findStorageByCode(store, typed.substr(0, digitsEnd))) {
        return placeIn(*rack, typed.substr(digitsEnd));
      }
      error = "Rack " + toUpper(typed.substr(0, digitsEnd)) + " does not exist";
      return false;
    }
  }
  if (looksLikeRackCode(typed)) {
    error = "Rack " + toUpper(trim(typed.substr(0, typed.find_first_of("- ")))) + " does not exist";
    return false;
  }
  if (utf8Length(typed) > kStorageNameMaxLength) {
    error = "A place name is at most " + to_string(kStorageNameMaxLength) + " characters";
    return false;
  }
  const auto known = existingPlace(store, typed, &item);
  choice.kind = StorageChoice::Kind::Place;
  choice.place = known.empty() ? typed : known;
  choice.display = choice.place;
  choice.newPlace = known.empty();
  return true;
}

void applyStorageChoice(InventoryItem& item, const StorageChoice& choice) {
  switch (choice.kind) {
    case StorageChoice::Kind::NotStored:
      item.rackId.clear();
      item.rackSlot.clear();
      item.rackAssignment = RackAssignmentMode::Unassigned;
      item.location.clear();
      return;
    case StorageChoice::Kind::Automatic:
      item.rackId.clear();
      item.rackSlot.clear();
      item.rackAssignment = RackAssignmentMode::Automatic;
      if (!isPlaceholderLocation(item.location)) item.location.clear();
      return;
    case StorageChoice::Kind::Slot:
      item.rackId = choice.rackId;
      item.rackSlot = choice.slot;
      item.rackAssignment = RackAssignmentMode::Manual;
      item.location.clear();
      return;
    case StorageChoice::Kind::Place:
      item.rackId.clear();
      item.rackSlot.clear();
      item.rackAssignment = RackAssignmentMode::Unassigned;
      item.location = choice.place;
      return;
  }
}

bool splitTrailingSlot(const string& text, string& name, string& slot) {
  const auto typed = trim(text);
  const auto at = typed.find_last_of("- ");
  if (at == string::npos || at == 0) return false;
  const auto tail = trim(typed.substr(at + 1));
  const auto head = trim(typed.substr(0, at));
  if (head.empty() || !isValidRackSlot(tail)) return false;
  name = head;
  slot = toUpper(tail);
  return true;
}

vector<string> storageSuggestions(const InventoryStore& store, const string& typed, size_t limit) {
  const auto wanted = toLower(trim(typed));
  vector<string> prefixHits;
  vector<string> innerHits;
  unordered_set<string> seen;
  const auto consider = [&](const string& name) {
    const auto key = toLower(trim(name));
    if (key.empty() || !seen.insert(key).second) return;
    if (wanted.empty() || key.rfind(wanted, 0) == 0) {
      prefixHits.push_back(trim(name));
    } else if (key.find(wanted) != string::npos) {
      innerHits.push_back(trim(name));
    }
  };
  // Organizers first: they are the named storage people make on purpose.
  for (const auto& rack : store.racks()) {
    if (isOrganizer(rack)) consider(rack.code);
  }
  for (const auto& item : store.items()) {
    if (!isPlaceholderLocation(item.location)) consider(item.location);
  }
  for (const auto& rack : store.racks()) {
    if (!isOrganizer(rack)) consider(rack.code);
  }
  prefixHits.insert(prefixHits.end(), innerHits.begin(), innerHits.end());
  if (prefixHits.size() > limit) prefixHits.resize(limit);
  return prefixHits;
}

}  // namespace inventatory
