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

#endif

}  // namespace inventatory
