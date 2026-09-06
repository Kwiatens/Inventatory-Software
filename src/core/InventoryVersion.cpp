// Inventatory - Hardware Inventory Management System
// Persistent inventory commit storage, snapshots, diffs, and safe reversal.

#include "core/InventoryVersionInternal.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

string serializeRackSnapshot(const InventatoryRack& rack) {
  ostringstream out;
  out << quoted(rack.id) << '\t' << quoted(rack.code) << '\t' << quoted(rack.componentType) << '\t' << rack.rows
      << '\t' << rack.columns << '\t' << rack.createdAt;
  return out.str();
}

bool deserializeRackSnapshot(const string& line, InventatoryRack& rack) {
  istringstream input(line);
  return static_cast<bool>(input >> quoted(rack.id) >> quoted(rack.code) >> quoted(rack.componentType) >> rack.rows >>
                           rack.columns >> rack.createdAt);
}

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

#ifdef _WIN32

bool ensureInventoryCommitSchema(SqliteConnection& connection) {
  return ensureInventoryDatabaseSchema(connection);
}

bool writeInventoryCommit(SqliteConnection& connection, const vector<InventoryItem>& items,
                          const vector<InventatoryRack>& racks, const InventoryCommitDraft& draft,
                          InventoryCommit& committed) {
  SqliteStatement latestStatement;
  if (sqliteApi().prepare_v2(connection.db,
                              "SELECT commit_id, sequence FROM inventatory_inventory_commits "
                              "ORDER BY sequence DESC LIMIT 1",
                              -1, &latestStatement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  string parentId;
  uint64_t sequence = 1;
  const int latestStep = sqliteApi().step(latestStatement.stmt);
  if (latestStep == SQLITE_ROW) {
    parentId = sqliteText(latestStatement.stmt, 0);
    if (!sqliteUInt64(latestStatement.stmt, 1, sequence) || sequence >= static_cast<uint64_t>(numeric_limits<sqlite3_int64>::max())) {
      return false;
    }
    ++sequence;
  } else if (latestStep != SQLITE_DONE) {
    return false;
  }

  InventoryCommit next;
  next.id = makeId();
  next.parentId = move(parentId);
  next.sequence = sequence;
  next.timestamp = time(nullptr);
  next.source = trim(draft.source).empty() ? "manual" : trim(draft.source);
  next.reference = draft.reference;
  next.message = trim(draft.message).empty() ? "Inventory updated" : trim(draft.message);
  next.checkpoint = draft.checkpoint;
  next.corrective = draft.corrective;
  next.revertedCommitId = draft.revertedCommitId;
  next.changedItemCount = draft.changedItemCount;
  next.changedRackCount = draft.changedRackCount;

  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT INTO inventatory_inventory_commits (
      commit_id, sequence, parent_id, committed_at, source, reference, message,
      checkpoint, corrective, reverted_commit_id, changed_item_count, changed_rack_count, snapshot_version
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, next.id.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int64(statement.stmt, 2, static_cast<sqlite3_int64>(next.sequence));
  sqliteApi().bind_text(statement.stmt, 3, next.parentId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int64(statement.stmt, 4, static_cast<sqlite3_int64>(next.timestamp));
  sqliteApi().bind_text(statement.stmt, 5, next.source.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 6, next.reference.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 7, next.message.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int(statement.stmt, 8, next.checkpoint ? 1 : 0);
  sqliteApi().bind_int(statement.stmt, 9, next.corrective ? 1 : 0);
  sqliteApi().bind_text(statement.stmt, 10, next.revertedCommitId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int64(statement.stmt, 11, static_cast<sqlite3_int64>(next.changedItemCount));
  sqliteApi().bind_int64(statement.stmt, 12, static_cast<sqlite3_int64>(next.changedRackCount));
  if (sqliteApi().step(statement.stmt) != SQLITE_DONE) return false;

  SqliteStatement itemStatement;
  if (sqliteApi().prepare_v2(connection.db,
                              "INSERT INTO inventatory_inventory_commit_items "
                              "(commit_id, item_id, item_data) VALUES (?, ?, ?)",
                              -1, &itemStatement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  for (const auto& item : items) {
    const auto data = serializeItem(item);
    sqliteApi().bind_text(itemStatement.stmt, 1, next.id.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(itemStatement.stmt, 2, item.id.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(itemStatement.stmt, 3, data.c_str(), -1, SQLITE_TRANSIENT);
    if (sqliteApi().step(itemStatement.stmt) != SQLITE_DONE) return false;
    sqliteApi().reset(itemStatement.stmt);
    sqliteApi().clear_bindings(itemStatement.stmt);
  }

  SqliteStatement rackStatement;
  if (sqliteApi().prepare_v2(connection.db,
                              "INSERT INTO inventatory_inventory_commit_racks "
                              "(commit_id, rack_id, rack_data) VALUES (?, ?, ?)",
                              -1, &rackStatement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  for (const auto& rack : racks) {
    const auto data = serializeRackSnapshot(rack);
    sqliteApi().bind_text(rackStatement.stmt, 1, next.id.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(rackStatement.stmt, 2, rack.id.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(rackStatement.stmt, 3, data.c_str(), -1, SQLITE_TRANSIENT);
    if (sqliteApi().step(rackStatement.stmt) != SQLITE_DONE) return false;
    sqliteApi().reset(rackStatement.stmt);
    sqliteApi().clear_bindings(rackStatement.stmt);
  }

  committed = move(next);
  return true;
}

namespace {

bool readCommitSummary(SqliteConnection& connection, const string& id, InventoryCommit& commit) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT commit_id, parent_id, sequence, committed_at, source, reference, message,
           checkpoint, corrective, reverted_commit_id, changed_item_count, changed_rack_count
    FROM inventatory_inventory_commits WHERE commit_id=?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqliteApi().step(statement.stmt) != SQLITE_ROW) return false;
  commit.id = sqliteText(statement.stmt, 0);
  commit.parentId = sqliteText(statement.stmt, 1);
  if (!sqliteUInt64(statement.stmt, 2, commit.sequence) || commit.sequence == 0 ||
      !sqliteTime(statement.stmt, 3, commit.timestamp)) return false;
  commit.source = sqliteText(statement.stmt, 4);
  commit.reference = sqliteText(statement.stmt, 5);
  commit.message = sqliteText(statement.stmt, 6);
  commit.checkpoint = sqliteApi().column_int(statement.stmt, 7) != 0;
  commit.corrective = sqliteApi().column_int(statement.stmt, 8) != 0;
  commit.revertedCommitId = sqliteText(statement.stmt, 9);
  if (!sqliteSize(statement.stmt, 10, commit.changedItemCount) ||
      !sqliteSize(statement.stmt, 11, commit.changedRackCount)) return false;
  return true;
}

bool readCommitSnapshot(SqliteConnection& connection, const string& id, InventoryStore& snapshot) {
  vector<InventoryItem> items;
  vector<InventatoryRack> racks;

  SqliteStatement itemStatement;
  if (sqliteApi().prepare_v2(connection.db,
                              "SELECT item_data FROM inventatory_inventory_commit_items "
                              "WHERE commit_id=? ORDER BY item_id",
                              -1, &itemStatement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqliteApi().bind_text(itemStatement.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  int itemStepResult = SQLITE_OK;
  while ((itemStepResult = sqliteApi().step(itemStatement.stmt)) == SQLITE_ROW) {
    InventoryItem item;
    if (!deserializeItem(sqliteText(itemStatement.stmt, 0), item)) return false;
    items.push_back(move(item));
  }
  if (itemStepResult != SQLITE_DONE) return false;

  SqliteStatement rackStatement;
  if (sqliteApi().prepare_v2(connection.db,
                              "SELECT rack_data FROM inventatory_inventory_commit_racks "
                              "WHERE commit_id=? ORDER BY rack_id",
                              -1, &rackStatement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqliteApi().bind_text(rackStatement.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  int rackStepResult = SQLITE_OK;
  while ((rackStepResult = sqliteApi().step(rackStatement.stmt)) == SQLITE_ROW) {
    InventatoryRack rack;
    if (!deserializeRackSnapshot(sqliteText(rackStatement.stmt, 0), rack)) return false;
    racks.push_back(move(rack));
  }
  if (rackStepResult != SQLITE_DONE) return false;

  snapshot.items() = move(items);
  snapshot.racks() = move(racks);
  reconcileRackAssignments(snapshot);
  return true;
}

}  // namespace

#endif

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

bool ensureInventoryCommitHistory(const filesystem::path& path, const InventoryStore& current) {
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection) || !ensureInventoryCommitSchema(connection)) return false;

  // Hold the write lock before checking and creating the baseline.  The old
  // check-then-BEGIN sequence allowed two first writers to both observe an
  // empty history and race their initial snapshots.
  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) return false;

  SqliteStatement countStatement;
  if (sqliteApi().prepare_v2(connection.db, "SELECT COUNT(*) FROM inventatory_inventory_commits", -1,
                             &countStatement.stmt, nullptr) != SQLITE_OK) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  if (sqliteApi().step(countStatement.stmt) != SQLITE_ROW || sqliteApi().column_type(countStatement.stmt, 0) != SQLITE_INTEGER) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  if (sqliteApi().column_int64(countStatement.stmt, 0) > 0) {
    if (!execSql(connection, "COMMIT")) {
      execSql(connection, "ROLLBACK");
      return false;
    }
    return true;
  }

  InventoryCommitDraft draft;
  draft.source = "system";
  draft.message = "Initial inventory";
  InventoryStore normalized = current;
  ensureInventoryIdentifiers(normalized.items());
  InventoryCommit committed;
  if (!writeInventoryCommit(connection, normalized.items(), normalized.racks(), draft, committed) ||
      !execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  return true;
#else
  (void)path;
  (void)current;
  return false;
#endif
}

bool loadInventoryCommits(const filesystem::path& path, vector<InventoryCommit>& commits) {
  commits.clear();
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection) || !ensureInventoryCommitSchema(connection)) return false;
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT commit_id, parent_id, sequence, committed_at, source, reference, message,
           checkpoint, corrective, reverted_commit_id, changed_item_count, changed_rack_count
    FROM inventatory_inventory_commits ORDER BY sequence DESC
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    InventoryCommit commit;
    commit.id = sqliteText(statement.stmt, 0);
    commit.parentId = sqliteText(statement.stmt, 1);
    if (!sqliteUInt64(statement.stmt, 2, commit.sequence) || commit.sequence == 0 ||
        !sqliteTime(statement.stmt, 3, commit.timestamp)) return false;
    commit.source = sqliteText(statement.stmt, 4);
    commit.reference = sqliteText(statement.stmt, 5);
    commit.message = sqliteText(statement.stmt, 6);
    commit.checkpoint = sqliteApi().column_int(statement.stmt, 7) != 0;
    commit.corrective = sqliteApi().column_int(statement.stmt, 8) != 0;
    commit.revertedCommitId = sqliteText(statement.stmt, 9);
    if (!sqliteSize(statement.stmt, 10, commit.changedItemCount) ||
        !sqliteSize(statement.stmt, 11, commit.changedRackCount)) return false;
    commits.push_back(move(commit));
  }
  return stepResult == SQLITE_DONE;
#else
  (void)path;
  return false;
#endif
}

bool loadInventoryCommit(const filesystem::path& path, const string& id, InventoryCommitDetail& detail) {
  detail = {};
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection) || !ensureInventoryCommitSchema(connection) ||
      !readCommitSummary(connection, id, detail.commit) || !readCommitSnapshot(connection, id, detail.snapshot)) {
    return false;
  }
  if (!detail.commit.parentId.empty()) {
    InventoryCommit parent;
    if (!readCommitSummary(connection, detail.commit.parentId, parent) ||
        !readCommitSnapshot(connection, detail.commit.parentId, detail.parentSnapshot)) {
      return false;
    }
    detail.hasParent = true;
  }
  detail.changes = detail.hasParent ? inventoryCommitDiff(detail.parentSnapshot, detail.snapshot)
                                    : inventoryCommitDiff(InventoryStore{}, detail.snapshot);
  return true;
#else
  (void)path;
  (void)id;
  return false;
#endif
}

}  // namespace inventatory
