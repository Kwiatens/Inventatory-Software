// Inventatory - Inventory commit history loading and validation.

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

#ifdef _WIN32

namespace {

bool deserializeRackSnapshot(const string& line, InventatoryRack& rack) {
  istringstream input(line);
  if (!(input >> quoted(rack.id) >> quoted(rack.code) >> quoted(rack.componentType) >> rack.rows >> rack.columns >>
        rack.createdAt)) return false;
  input >> ws;
  return input.eof();
}

bool validateSnapshotSemantics(const InventoryStore& snapshot, string* error) {
  const auto fail = [&](const string& message) {
    if (error != nullptr) *error = message;
    return false;
  };
  if (!validateInventoryIdentifiers(snapshot.items(), snapshot.racks())) {
    return fail("Inventory commit snapshot has duplicate or invalid identifiers");
  }

  unordered_set<string> rackIds;
  const auto validRackSlot = [](const InventatoryRack& rack, const string& value) {
    const auto slot = toUpper(trim(value));
    if (slot.size() < 2 || !isalpha(static_cast<unsigned char>(slot.front()))) return false;
    size_t column = 0;
    for (size_t index = 1; index < slot.size(); ++index) {
      const auto character = static_cast<unsigned char>(slot[index]);
      if (!isdigit(character)) return false;
      const auto digit = static_cast<size_t>(character - '0');
      if (column > (numeric_limits<size_t>::max() - digit) / 10) return false;
      column = column * 10 + digit;
    }
    const auto row = static_cast<size_t>(slot.front() - 'A');
    return row < static_cast<size_t>(rack.rows) && column > 0 && column <= static_cast<size_t>(rack.columns);
  };
  unordered_map<string, const InventatoryRack*> racksById;
  for (const auto& rack : snapshot.racks()) {
    if (rack.createdAt < 0) return fail("Inventory commit snapshot has an invalid rack timestamp");
    const auto rackId = toLower(trim(rack.id));
    rackIds.insert(rackId);
    racksById[rackId] = &rack;
  }
  unordered_set<string> occupiedSlots;
  for (const auto& item : snapshot.items()) {
    if (item.lastUpdated < 0 || item.createdAt < 0) {
      return fail("Inventory commit snapshot has an invalid item timestamp");
    }
    const bool hasRackId = !trim(item.rackId).empty();
    const bool hasRackSlot = !trim(item.rackSlot).empty();
    if (hasRackId != hasRackSlot) return fail("Inventory commit snapshot has a partial rack assignment");
    if (!hasRackId) continue;
    const auto rackIt = racksById.find(toLower(trim(item.rackId)));
    if (rackIt == racksById.end() || !validRackSlot(*rackIt->second, item.rackSlot)) {
      return fail("Inventory commit snapshot has an invalid rack assignment");
    }
    const auto slotKey = toLower(trim(item.rackId)) + "\x1f" + toLower(trim(item.rackSlot));
    if (!occupiedSlots.insert(slotKey).second) return fail("Inventory commit snapshot has an occupied rack slot twice");
  }
  return true;
}

bool readCommitSummary(SqliteConnection& connection, const string& id, InventoryCommit& commit) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT commit_id, parent_id, sequence, committed_at, source, reference, message,
           checkpoint, corrective, reverted_commit_id, changed_item_count, changed_rack_count, snapshot_version
    FROM inventatory_inventory_commits WHERE commit_id=?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqliteApi().step(statement.stmt) != SQLITE_ROW) return false;
  if (sqliteApi().column_type(statement.stmt, 0) != SQLITE_TEXT ||
      sqliteApi().column_type(statement.stmt, 1) != SQLITE_TEXT ||
      sqliteApi().column_type(statement.stmt, 4) != SQLITE_TEXT ||
      sqliteApi().column_type(statement.stmt, 5) != SQLITE_TEXT ||
      sqliteApi().column_type(statement.stmt, 6) != SQLITE_TEXT ||
      sqliteApi().column_type(statement.stmt, 9) != SQLITE_TEXT) return false;
  commit.id = sqliteText(statement.stmt, 0);
  commit.parentId = sqliteText(statement.stmt, 1);
  if (!sqliteUInt64(statement.stmt, 2, commit.sequence) || commit.sequence == 0 ||
      !sqliteTime(statement.stmt, 3, commit.timestamp)) return false;
  commit.source = sqliteText(statement.stmt, 4);
  commit.reference = sqliteText(statement.stmt, 5);
  commit.message = sqliteText(statement.stmt, 6);
  if (sqliteApi().column_type(statement.stmt, 7) != SQLITE_INTEGER ||
      sqliteApi().column_type(statement.stmt, 8) != SQLITE_INTEGER ||
      (sqliteApi().column_int64(statement.stmt, 7) != 0 && sqliteApi().column_int64(statement.stmt, 7) != 1) ||
      (sqliteApi().column_int64(statement.stmt, 8) != 0 && sqliteApi().column_int64(statement.stmt, 8) != 1) ||
      sqliteApi().column_type(statement.stmt, 12) != SQLITE_INTEGER ||
      sqliteApi().column_int64(statement.stmt, 12) != 1) return false;
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
                              "SELECT item_id, item_data FROM inventatory_inventory_commit_items "
                              "WHERE commit_id=? ORDER BY item_id",
                              -1, &itemStatement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqliteApi().bind_text(itemStatement.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  int itemStepResult = SQLITE_OK;
  while ((itemStepResult = sqliteApi().step(itemStatement.stmt)) == SQLITE_ROW) {
    if (sqliteApi().column_type(itemStatement.stmt, 0) != SQLITE_TEXT ||
        sqliteApi().column_type(itemStatement.stmt, 1) != SQLITE_TEXT) return false;
    InventoryItem item;
    const auto itemId = sqliteText(itemStatement.stmt, 0);
    const auto itemData = sqliteText(itemStatement.stmt, 1);
    if (!deserializeItemStrict(itemData, item)) return false;
    if (itemId.empty() || itemId != item.id) return false;
    items.push_back(move(item));
  }
  if (itemStepResult != SQLITE_DONE) return false;

  SqliteStatement rackStatement;
  if (sqliteApi().prepare_v2(connection.db,
                              "SELECT rack_id, rack_data FROM inventatory_inventory_commit_racks "
                              "WHERE commit_id=? ORDER BY rack_id",
                              -1, &rackStatement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqliteApi().bind_text(rackStatement.stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  int rackStepResult = SQLITE_OK;
  while ((rackStepResult = sqliteApi().step(rackStatement.stmt)) == SQLITE_ROW) {
    if (sqliteApi().column_type(rackStatement.stmt, 0) != SQLITE_TEXT ||
        sqliteApi().column_type(rackStatement.stmt, 1) != SQLITE_TEXT) return false;
    InventatoryRack rack;
    const auto rackId = sqliteText(rackStatement.stmt, 0);
    if (!deserializeRackSnapshot(sqliteText(rackStatement.stmt, 1), rack) || rackId.empty() || rackId != rack.id) return false;
    racks.push_back(move(rack));
  }
  if (rackStepResult != SQLITE_DONE) return false;

  snapshot.items() = move(items);
  snapshot.racks() = move(racks);
  return validateSnapshotSemantics(snapshot, nullptr);
}


}  // namespace

bool validateInventoryCommitHistory(SqliteConnection& connection, string* error) {
  const auto fail = [&](const string& message) {
    if (error != nullptr) *error = message;
    return false;
  };
  if (connection.db == nullptr) return fail("SQLite connection is not open");

  // Snapshot rows are intentionally checked for orphaned commit IDs as well;
  // otherwise an extra row could survive a restore and become visible after a
  // later commit reuses the same identifier.
  const char* const orphanQueries[] = {
      "SELECT 1 FROM inventatory_inventory_commit_items i LEFT JOIN inventatory_inventory_commits c "
      "ON c.commit_id=i.commit_id WHERE c.commit_id IS NULL LIMIT 1",
      "SELECT 1 FROM inventatory_inventory_commit_racks r LEFT JOIN inventatory_inventory_commits c "
      "ON c.commit_id=r.commit_id WHERE c.commit_id IS NULL LIMIT 1",
  };
  for (const auto* query : orphanQueries) {
    SqliteStatement orphanStatement;
    if (sqliteApi().prepare_v2(connection.db, query, -1, &orphanStatement.stmt, nullptr) != SQLITE_OK) {
      return fail("Unable to validate inventory commit relationships");
    }
    const auto orphanStep = sqliteApi().step(orphanStatement.stmt);
    if (orphanStep == SQLITE_ROW) {
      return fail("Inventory commit snapshot has an orphaned row");
    }
    if (orphanStep != SQLITE_DONE) return fail("Unable to validate inventory commit relationships");
  }

  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
                             "SELECT commit_id FROM inventatory_inventory_commits ORDER BY sequence ASC",
                             -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return fail("Unable to read inventory commit history");
  }

  vector<InventoryCommit> commits;
  vector<InventoryStore> snapshots;
  unordered_set<string> commitIds;
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    if (sqliteApi().column_type(statement.stmt, 0) != SQLITE_TEXT) {
      return fail("Inventory commit has a non-text identifier");
    }
    const auto id = sqliteText(statement.stmt, 0);
    if (id.empty() || !commitIds.insert(id).second) return fail("Inventory commit identifiers are not unique");
    InventoryCommit commit;
    if (!readCommitSummary(connection, id, commit) || commit.id != id) {
      return fail("Inventory commit summary is malformed");
    }
    InventoryStore snapshot;
    string snapshotError;
    if (!readCommitSnapshot(connection, id, snapshot) || !validateSnapshotSemantics(snapshot, &snapshotError)) {
      return fail(snapshotError.empty() ? "Inventory commit snapshot is malformed" : snapshotError);
    }
    commits.push_back(move(commit));
    snapshots.push_back(move(snapshot));
  }
  if (stepResult != SQLITE_DONE) return fail("Unable to read inventory commit history");

  for (size_t index = 0; index < commits.size(); ++index) {
    const auto& commit = commits[index];
    if (commit.sequence != index + 1) return fail("Inventory commit sequence is not contiguous");
    if (index == 0) {
      if (!commit.parentId.empty() || commit.changedItemCount != 0 || commit.changedRackCount != 0) {
        return fail("Initial inventory commit has an invalid parent or change count");
      }
    } else {
      if (commit.parentId != commits[index - 1].id) return fail("Inventory commit parent does not match sequence");
      const auto changes = inventoryCommitDiff(snapshots[index - 1], snapshots[index]);
      unordered_set<string> changedItems;
      unordered_set<string> changedRacks;
      for (const auto& change : changes) {
        if (change.entityType == "item") changedItems.insert(change.entityId);
        else if (change.entityType == "rack") changedRacks.insert(change.entityId);
        else return fail("Inventory commit contains an unknown change entity");
      }
      if (commit.changedItemCount != changedItems.size() || commit.changedRackCount != changedRacks.size()) {
        return fail("Inventory commit change counts do not match its snapshots");
      }
      if (changes.empty() && !commit.checkpoint) return fail("Inventory commit without changes is not a checkpoint");
    }
    if (commit.corrective && commit.revertedCommitId.empty()) {
      return fail("Corrective inventory commit is missing its reverted commit");
    }
    if (!commit.corrective && !commit.revertedCommitId.empty()) {
      return fail("Non-corrective inventory commit references a reverted commit");
    }
    if (!commit.revertedCommitId.empty() && commitIds.count(commit.revertedCommitId) == 0) {
      return fail("Inventory commit references a missing reverted commit");
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
  if (!validateInventoryCommitHistory(connection, nullptr)) return false;
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
    if (parent.sequence + 1 != detail.commit.sequence || parent.id != detail.commit.parentId) return false;
    detail.hasParent = true;
  }
  detail.changes = detail.hasParent ? inventoryCommitDiff(detail.parentSnapshot, detail.snapshot)
                                    : inventoryCommitDiff(InventoryStore{}, detail.snapshot);
  bool countsValid = true;
  if (detail.hasParent) {
    unordered_set<string> items;
    unordered_set<string> racks;
    for (const auto& change : detail.changes) {
      if (change.entityType == "item") items.insert(change.entityId);
      else if (change.entityType == "rack") racks.insert(change.entityId);
    }
    countsValid = detail.commit.changedItemCount == items.size() && detail.commit.changedRackCount == racks.size();
  }
  if ((!detail.hasParent && (detail.commit.sequence != 1 || detail.commit.changedItemCount != 0 ||
                             detail.commit.changedRackCount != 0)) ||
      (detail.hasParent && detail.changes.empty() && !detail.commit.checkpoint) || !countsValid ||
      (detail.commit.corrective && detail.commit.revertedCommitId.empty()) ||
      (!detail.commit.corrective && !detail.commit.revertedCommitId.empty())) {
    return false;
  }
  return true;
#else
  (void)path;
  (void)id;
  return false;
#endif
}


#endif

}  // namespace inventatory
