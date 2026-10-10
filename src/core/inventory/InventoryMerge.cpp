// Inventatory - three-way merge of staged inventory edits onto the live inventory.

#include "core/inventory/InventoryMerge.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace inventatory {

using namespace std;

namespace {

bool sameValue(const vector<Parameter>& lhs, const vector<Parameter>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t index = 0; index < lhs.size(); ++index) {
    if (lhs[index].name != rhs[index].name || lhs[index].value != rhs[index].value) return false;
  }
  return true;
}

template <typename Value>
bool sameValue(const Value& lhs, const Value& rhs) {
  return lhs == rhs;
}

// One independently merged unit of an item. The table starts from the staged item and only pulls
// a live value in for a field the user did not touch, so a field missing from the table behaves
// like the former whole-record overwrite instead of dropping the user's edit. Add new
// InventoryItem fields here (identity `id`, `quantity` and `lastUpdated` are handled separately).
struct FieldRule {
  const char* label;
  bool (*same)(const InventoryItem&, const InventoryItem&);
  void (*assign)(InventoryItem& to, const InventoryItem& from);
};

#define INVENTATORY_MERGE_FIELD(label, member)                                                  \
  {                                                                                             \
    label, [](const InventoryItem& lhs, const InventoryItem& rhs) {                             \
      return sameValue(lhs.member, rhs.member);                                                 \
    },                                                                                          \
        [](InventoryItem& to, const InventoryItem& from) { to.member = from.member; }           \
  }

const FieldRule kFieldRules[] = {
    INVENTATORY_MERGE_FIELD("part name", partName),
    INVENTATORY_MERGE_FIELD("manufacturer", manufacturer),
    INVENTATORY_MERGE_FIELD("category", category),
    INVENTATORY_MERGE_FIELD("reorder threshold", reorderThreshold),
    INVENTATORY_MERGE_FIELD("location", location),
    INVENTATORY_MERGE_FIELD("tags", tags),
    INVENTATORY_MERGE_FIELD("parameters", parameters),
    INVENTATORY_MERGE_FIELD("notes", notes),
    INVENTATORY_MERGE_FIELD("DigiKey part number", digikeyPartNumber),
    INVENTATORY_MERGE_FIELD("datasheet URL", datasheetUrl),
    INVENTATORY_MERGE_FIELD("product URL", productUrl),
    INVENTATORY_MERGE_FIELD("sync status", syncStatus),
    INVENTATORY_MERGE_FIELD("SKU", sku),
    INVENTATORY_MERGE_FIELD("Inventatory ID", inventatoryId),
    INVENTATORY_MERGE_FIELD("created at", createdAt),
    INVENTATORY_MERGE_FIELD("machine code", machineCode),
    // Rack id, slot and mode describe one placement and are merged together.
    {"rack placement",
     [](const InventoryItem& lhs, const InventoryItem& rhs) {
       return lhs.rackId == rhs.rackId && lhs.rackSlot == rhs.rackSlot && lhs.rackAssignment == rhs.rackAssignment;
     },
     [](InventoryItem& to, const InventoryItem& from) {
       to.rackId = from.rackId;
       to.rackSlot = from.rackSlot;
       to.rackAssignment = from.rackAssignment;
     }},
    INVENTATORY_MERGE_FIELD("label override", labelOverride),
    INVENTATORY_MERGE_FIELD("vendor provider", vendorMetadata.provider),
    INVENTATORY_MERGE_FIELD("vendor product number", vendorMetadata.providerProductNumber),
    INVENTATORY_MERGE_FIELD("manufacturer part number", vendorMetadata.manufacturerPartNumber),
    INVENTATORY_MERGE_FIELD("vendor category ID", vendorMetadata.categoryId),
    INVENTATORY_MERGE_FIELD("vendor category path", vendorMetadata.categoryPath),
    INVENTATORY_MERGE_FIELD("vendor title", vendorMetadata.title),
    INVENTATORY_MERGE_FIELD("vendor description", vendorMetadata.detailedDescription),
    INVENTATORY_MERGE_FIELD("vendor parameters", vendorMetadata.parameters),
    INVENTATORY_MERGE_FIELD("vendor product URL", vendorMetadata.productUrl),
    INVENTATORY_MERGE_FIELD("vendor locale", vendorMetadata.locale),
};

#undef INVENTATORY_MERGE_FIELD

string displayName(const InventoryItem& item) {
  const auto name = trim(item.partName);
  return name.empty() ? item.id : name;
}

void addNotice(vector<string>* notices, string notice) {
  if (notices != nullptr) notices->push_back(move(notice));
}

bool sameItemRecord(const InventoryItem& lhs, const InventoryItem& rhs) {
  return serializeItem(lhs) == serializeItem(rhs);
}

unordered_map<string, const InventoryItem*> indexItems(const vector<InventoryItem>& items) {
  unordered_map<string, const InventoryItem*> indexed;
  indexed.reserve(items.size());
  for (const auto& item : items) indexed[item.id] = &item;
  return indexed;
}

string rackKey(const InventatoryRack& rack) {
  return toLower(trim(rack.code));
}

}  // namespace

InventoryItem mergeEditedItem(const InventoryItem& base, const InventoryItem& edited, const InventoryItem& current,
                              QuantityMerge quantity, vector<string>* notices) {
  InventoryItem merged = edited;
  const auto name = displayName(edited);
  for (const auto& rule : kFieldRules) {
    if (rule.same(edited, base)) {
      rule.assign(merged, current);  // untouched by the user: keep whatever is live now
    } else if (!rule.same(current, base) && !rule.same(current, edited)) {
      addNotice(notices, name + ": " + rule.label + " was also changed elsewhere; your value was kept");
    }
  }

  if (edited.quantity == base.quantity) {
    merged.quantity = current.quantity;
  } else if (quantity == QuantityMerge::ApplyDelta) {
    const long long delta = static_cast<long long>(edited.quantity) - base.quantity;
    const long long combined = static_cast<long long>(current.quantity) + delta;
    const long long bounded = clamp<long long>(combined, 0, (numeric_limits<int>::max)());
    if (bounded != combined) {
      const string change = (delta > 0 ? "+" : "") + to_string(delta);
      const string direction = combined < 0 ? "below zero" : "above the maximum";
      addNotice(notices, name + ": quantity change of " + change + " would take stock " + direction +
                             "; it was set to " + to_string(bounded));
    }
    merged.quantity = static_cast<int>(bounded);
  } else if (current.quantity != base.quantity && current.quantity != edited.quantity) {
    addNotice(notices, name + ": quantity was changed to " + to_string(current.quantity) +
                           " elsewhere; your value " + to_string(edited.quantity) + " was kept");
  }

  merged.lastUpdated = max(edited.lastUpdated, current.lastUpdated);
  return merged;
}

bool releaseConflictingRackPlacement(const InventoryStore& store, InventoryItem& item, vector<string>* notices) {
  if (item.rackId.empty() && item.rackSlot.empty()) return false;
  const bool rackExists = any_of(store.racks().begin(), store.racks().end(),
                                 [&](const InventatoryRack& rack) { return rack.id == item.rackId; });
  const auto slot = toUpper(trim(item.rackSlot));
  const bool slotTaken = rackExists && !slot.empty() &&
                         any_of(store.items().begin(), store.items().end(), [&](const InventoryItem& other) {
                           return other.id != item.id && other.rackId == item.rackId &&
                                  toUpper(trim(other.rackSlot)) == slot;
                         });
  if (rackExists && !slot.empty() && !slotTaken) return false;
  addNotice(notices, displayName(item) + ": rack slot " + (slot.empty() ? string("(none)") : slot) +
                         " is no longer available; the part will be placed again");
  item.rackId.clear();
  item.rackSlot.clear();
  item.rackAssignment = RackAssignmentMode::Automatic;
  return true;
}

InventoryStore mergeInventoryChanges(const InventoryStore& base, const InventoryStore& staged,
                                     const InventoryStore& current, QuantityMerge quantity,
                                     vector<string>* notices) {
  InventoryStore merged = current;
  const auto baseItems = indexItems(base.items());
  const auto stagedItems = indexItems(staged.items());
  const auto liveItems = indexItems(current.items());
  unordered_set<string> touchedItemIds;

  // Racks created while the work was staged. Code collisions keep the live rack.
  {
    unordered_set<string> baseRackIds;
    for (const auto& rack : base.racks()) baseRackIds.insert(rack.id);
    unordered_set<string> liveRackIds;
    unordered_set<string> liveRackCodes;
    for (const auto& rack : current.racks()) {
      liveRackIds.insert(rack.id);
      liveRackCodes.insert(rackKey(rack));
    }
    for (const auto& rack : staged.racks()) {
      if (baseRackIds.count(rack.id) != 0 || liveRackIds.count(rack.id) != 0) continue;
      if (liveRackCodes.count(rackKey(rack)) != 0) {
        addNotice(notices, "Rack " + rack.code + " was created elsewhere in the meantime; the existing rack was kept");
        continue;
      }
      merged.racks().push_back(rack);
      liveRackCodes.insert(rackKey(rack));
    }
  }

  // Items that existed when the work started. `merged` still lists the live items in their original
  // order here; the first item with an id is the one an edit applies to.
  unordered_map<string, size_t> mergedPosition;
  for (size_t index = 0; index < merged.items().size(); ++index) {
    mergedPosition.emplace(merged.items()[index].id, index);
  }
  unordered_set<string> removedIds;
  for (const auto& baseItem : base.items()) {
    const auto stagedFound = stagedItems.find(baseItem.id);
    const auto liveFound = liveItems.find(baseItem.id);
    const InventoryItem* stagedItem = stagedFound == stagedItems.end() ? nullptr : stagedFound->second;
    const InventoryItem* liveItem = liveFound == liveItems.end() ? nullptr : liveFound->second;
    if (stagedItem == nullptr) {
      if (liveItem == nullptr) continue;
      if (sameItemRecord(*liveItem, baseItem)) {
        removedIds.insert(baseItem.id);
      } else {
        addNotice(notices, displayName(*liveItem) + ": not removed because it changed in the meantime");
      }
      continue;
    }
    if (sameItemRecord(baseItem, *stagedItem)) continue;
    if (liveItem == nullptr) {
      addNotice(notices, displayName(baseItem) + ": changes were not applied because the part was removed in the meantime");
      continue;
    }
    if (const auto position = mergedPosition.find(baseItem.id); position != mergedPosition.end()) {
      auto& item = merged.items()[position->second];
      item = mergeEditedItem(baseItem, *stagedItem, *liveItem, quantity, notices);
      touchedItemIds.insert(item.id);
    }
  }
  if (!removedIds.empty()) {
    merged.items().erase(remove_if(merged.items().begin(), merged.items().end(),
                                   [&removedIds](const InventoryItem& item) { return removedIds.count(item.id) != 0; }),
                         merged.items().end());
  }

  // Items created while the work was staged.
  for (const auto& item : staged.items()) {
    if (baseItems.count(item.id) != 0) continue;
    if (liveItems.count(item.id) != 0) {
      addNotice(notices, displayName(item) + ": a part with the same id already exists; the existing part was kept");
      continue;
    }
    merged.items().push_back(item);
    touchedItemIds.insert(item.id);
  }

  // A staged part may point at a rack that could not be carried over, or at a slot the live
  // inventory filled in the meantime; let it be placed again instead of sharing the slot.
  unordered_set<string> rackIds;
  for (const auto& rack : merged.racks()) rackIds.insert(rack.id);
  const auto slotKey = [](const InventoryItem& item) { return item.rackId + '\x1f' + toUpper(trim(item.rackSlot)); };
  unordered_set<string> occupiedSlots;
  for (const auto& item : merged.items()) {
    if (touchedItemIds.count(item.id) == 0 && !item.rackId.empty() && !item.rackSlot.empty()) {
      occupiedSlots.insert(slotKey(item));
    }
  }
  for (auto& item : merged.items()) {
    if (touchedItemIds.count(item.id) == 0 || item.rackId.empty()) continue;
    const bool missingRack = rackIds.count(item.rackId) == 0;
    const bool sharedSlot = !missingRack && !item.rackSlot.empty() && !occupiedSlots.insert(slotKey(item)).second;
    if (!missingRack && !sharedSlot) continue;
    item.rackId.clear();
    item.rackSlot.clear();
    item.rackAssignment = RackAssignmentMode::Automatic;
  }
  return merged;
}

}  // namespace inventatory
