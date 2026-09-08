// Inventatory - inventory commit diff and reversal workflows.

#include "core/history/InventoryVersionInternal.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace inventatory {

using namespace std;

namespace {
string joinTags(const vector<string>& values) {
  return join(values, ',');
}

string joinParameters(const vector<Parameter>& values) {
  vector<string> rendered;
  rendered.reserve(values.size());
  for (const auto& value : values) {
    rendered.push_back(value.name + "=" + value.value);
  }
  return join(rendered, ';');
}
}  // namespace

string serializeRackSnapshot(const InventatoryRack& rack) {
  ostringstream out;
  out << quoted(rack.id) << '\t' << quoted(rack.code) << '\t' << quoted(rack.componentType) << '\t' << rack.rows
      << '\t' << rack.columns << '\t' << rack.createdAt;
  return out.str();
}

namespace {
vector<pair<string, string>> itemFields(const InventoryItem& item) {
  return {
      {"part name", item.partName},
      {"manufacturer", item.manufacturer},
      {"category", item.category},
      {"quantity", to_string(item.quantity)},
      {"reorder threshold", to_string(item.reorderThreshold)},
      {"location", item.location},
      {"tags", joinTags(item.tags)},
      {"parameters", joinParameters(item.parameters)},
      {"notes", item.notes},
      {"DigiKey part number", item.digikeyPartNumber},
      {"datasheet URL", item.datasheetUrl},
      {"product URL", item.productUrl},
      {"sync status", item.syncStatus},
      {"SKU", item.sku},
      {"last updated", to_string(item.lastUpdated)},
      {"Inventatory ID", item.inventatoryId},
      {"created at", to_string(item.createdAt)},
      {"machine code", item.machineCode},
      {"rack ID", item.rackId},
      {"rack slot", item.rackSlot},
      {"rack assignment", rackAssignmentModeName(item.rackAssignment)},
      {"label override", item.labelOverride},
      {"vendor provider", item.vendorMetadata.provider},
      {"vendor product number", item.vendorMetadata.providerProductNumber},
      {"manufacturer part number", item.vendorMetadata.manufacturerPartNumber},
      {"vendor category ID", item.vendorMetadata.categoryId},
      {"vendor category path", joinTags(item.vendorMetadata.categoryPath)},
      {"vendor title", item.vendorMetadata.title},
      {"vendor description", item.vendorMetadata.detailedDescription},
      {"vendor parameters", joinParameters(item.vendorMetadata.parameters)},
      {"vendor product URL", item.vendorMetadata.productUrl},
      {"vendor locale", item.vendorMetadata.locale},
  };
}

vector<pair<string, string>> rackFields(const InventatoryRack& rack) {
  return {
      {"code", rack.code},
      {"component type", rack.componentType},
      {"rows", to_string(rack.rows)},
      {"columns", to_string(rack.columns)},
      {"created at", to_string(rack.createdAt)},
  };
}

template <typename Value>
const Value* findValue(const unordered_map<string, const Value*>& values, const string& id) {
  const auto it = values.find(id);
  return it == values.end() ? nullptr : it->second;
}

void appendRecordChange(vector<InventoryFieldChange>& changes, const string& entityType, const string& entityId,
                        const string& label, const string& before, const string& after) {
  changes.push_back({entityType, entityId, label, "record", before, after});
}

void appendFieldChanges(vector<InventoryFieldChange>& changes, const string& entityType, const string& entityId,
                        const string& label, const vector<pair<string, string>>& before,
                        const vector<pair<string, string>>& after) {
  const auto count = min(before.size(), after.size());
  for (size_t index = 0; index < count; ++index) {
    if (before[index].second == after[index].second) continue;
    changes.push_back({entityType, entityId, label, before[index].first, before[index].second, after[index].second});
  }
}

string entityKey(const string& type, const string& id) {
  return type + '\x1f' + id;
}

template <typename Value>
unordered_map<string, const Value*> indexValues(const vector<Value>& values) {
  unordered_map<string, const Value*> indexed;
  indexed.reserve(values.size());
  for (const auto& value : values) {
    if (!value.id.empty()) indexed[value.id] = &value;
  }
  return indexed;
}

bool sameItem(const InventoryItem& lhs, const InventoryItem& rhs) {
  return serializeItem(lhs) == serializeItem(rhs);
}

bool sameRack(const InventatoryRack& lhs, const InventatoryRack& rhs) {
  return serializeRackSnapshot(lhs) == serializeRackSnapshot(rhs);
}
}  // namespace

vector<InventoryFieldChange> inventoryCommitDiff(const InventoryStore& before, const InventoryStore& after) {
  const auto beforeItems = indexValues(before.items());
  const auto afterItems = indexValues(after.items());
  const auto beforeRacks = indexValues(before.racks());
  const auto afterRacks = indexValues(after.racks());
  vector<InventoryFieldChange> changes;

  unordered_set<string> itemIds;
  itemIds.reserve(beforeItems.size() + afterItems.size());
  for (const auto& entry : beforeItems) itemIds.insert(entry.first);
  for (const auto& entry : afterItems) itemIds.insert(entry.first);
  for (const auto& id : itemIds) {
    const auto* oldItem = findValue(beforeItems, id);
    const auto* newItem = findValue(afterItems, id);
    if (oldItem == nullptr && newItem != nullptr) {
      appendRecordChange(changes, "item", id, newItem->partName, "absent", "added");
    } else if (oldItem != nullptr && newItem == nullptr) {
      appendRecordChange(changes, "item", id, oldItem->partName, "present", "deleted");
    } else if (oldItem != nullptr && newItem != nullptr) {
      appendFieldChanges(changes, "item", id, newItem->partName, itemFields(*oldItem), itemFields(*newItem));
    }
  }

  unordered_set<string> rackIds;
  rackIds.reserve(beforeRacks.size() + afterRacks.size());
  for (const auto& entry : beforeRacks) rackIds.insert(entry.first);
  for (const auto& entry : afterRacks) rackIds.insert(entry.first);
  for (const auto& id : rackIds) {
    const auto* oldRack = findValue(beforeRacks, id);
    const auto* newRack = findValue(afterRacks, id);
    if (oldRack == nullptr && newRack != nullptr) {
      appendRecordChange(changes, "rack", id, newRack->code, "absent", "added");
    } else if (oldRack != nullptr && newRack == nullptr) {
      appendRecordChange(changes, "rack", id, oldRack->code, "present", "deleted");
    } else if (oldRack != nullptr && newRack != nullptr) {
      appendFieldChanges(changes, "rack", id, newRack->code, rackFields(*oldRack), rackFields(*newRack));
    }
  }
  return changes;
}

bool prepareInventoryCommitReverse(const InventoryCommitDetail& detail, const InventoryStore& current,
                                   InventoryStore& reversed, string& conflict) {
  if (!detail.hasParent || detail.changes.empty()) {
    conflict = detail.hasParent ? "The selected commit has no inventory changes" : "The initial inventory cannot be reversed";
    return false;
  }

  reversed = current;
  const auto beforeItems = indexValues(detail.parentSnapshot.items());
  const auto afterItems = indexValues(detail.snapshot.items());
  const auto currentItems = indexValues(current.items());
  const auto beforeRacks = indexValues(detail.parentSnapshot.racks());
  const auto afterRacks = indexValues(detail.snapshot.racks());
  const auto currentRacks = indexValues(current.racks());

  unordered_set<string> entities;
  for (const auto& change : detail.changes) entities.insert(entityKey(change.entityType, change.entityId));

  for (const auto& key : entities) {
    const auto separator = key.find('\x1f');
    const auto type = key.substr(0, separator);
    const auto id = key.substr(separator + 1);
    if (type == "item") {
      const auto* before = findValue(beforeItems, id);
      const auto* after = findValue(afterItems, id);
      const auto* now = findValue(currentItems, id);
      if (before == nullptr && after != nullptr) {
        if (now == nullptr || !sameItem(*now, *after)) {
          conflict = "Part " + after->partName + " changed after the selected commit";
          return false;
        }
        reversed.items().erase(remove_if(reversed.items().begin(), reversed.items().end(),
                                         [&](const InventoryItem& item) { return item.id == id; }),
                              reversed.items().end());
      } else if (before != nullptr && after == nullptr) {
        if (now != nullptr) {
          conflict = "Part " + before->partName + " changed after the selected commit";
          return false;
        }
        reversed.items().push_back(*before);
      } else if (before != nullptr && after != nullptr) {
        if (now == nullptr || !sameItem(*now, *after)) {
          conflict = "Part " + after->partName + " changed after the selected commit";
          return false;
        }
        for (auto& item : reversed.items()) {
          if (item.id == id) item = *before;
        }
      }
    } else if (type == "rack") {
      const auto* before = findValue(beforeRacks, id);
      const auto* after = findValue(afterRacks, id);
      const auto* now = findValue(currentRacks, id);
      if (before == nullptr && after != nullptr) {
        if (now == nullptr || !sameRack(*now, *after)) {
          conflict = "Rack " + after->code + " changed after the selected commit";
          return false;
        }
        reversed.racks().erase(remove_if(reversed.racks().begin(), reversed.racks().end(),
                                         [&](const InventatoryRack& rack) { return rack.id == id; }),
                              reversed.racks().end());
      } else if (before != nullptr && after == nullptr) {
        if (now != nullptr) {
          conflict = "Rack " + before->code + " changed after the selected commit";
          return false;
        }
        reversed.racks().push_back(*before);
      } else if (before != nullptr && after != nullptr) {
        if (now == nullptr || !sameRack(*now, *after)) {
          conflict = "Rack " + after->code + " changed after the selected commit";
          return false;
        }
        for (auto& rack : reversed.racks()) {
          if (rack.id == id) rack = *before;
        }
      }
    }
  }
  return true;
}

}  // namespace inventatory
