// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Durable inbox and result delivery for the Inventatory Scan R1 protocol.

#include "core/scanner/InventatoryScanProtocol.h"

#include "core/inventory/InventoryInternals.h"
#include "core/storage/InventorySqlite.h"

#include <algorithm>
#include <ctime>
#include <limits>

namespace inventatory {

using namespace std;

namespace {

int boundedSqliteLimit(size_t limit) {
  return static_cast<int>(min(limit, static_cast<size_t>(numeric_limits<int>::max())));
}

enum class StoredEventIdentity { Unused, Same, Different, Failed };

// Compares an event with the stored row of its id. A different scan under that id is refused:
// the device numbers its events, so accepting it would ack and lose the scan.
StoredEventIdentity storedEventIdentity(SqliteConnection& connection, const string& deviceId,
                                        const DeviceSyncEvent& event) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT device_id, event_type, event_code, event_value FROM inventatory_device_events WHERE event_id=? LIMIT 1",
      -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return StoredEventIdentity::Failed;
  }
  sqliteApi().bind_text(statement.stmt, 1, event.eventId.c_str(), -1, SQLITE_TRANSIENT);
  const auto step = sqliteApi().step(statement.stmt);
  if (step == SQLITE_DONE) return StoredEventIdentity::Unused;
  if (step != SQLITE_ROW || sqliteApi().column_type(statement.stmt, 0) != SQLITE_TEXT) return StoredEventIdentity::Failed;
  int storedValue = 0;
  if (!sqliteInt32(statement.stmt, 3, storedValue)) return StoredEventIdentity::Failed;
  const bool same = sqliteText(statement.stmt, 0) == deviceId && sqliteText(statement.stmt, 1) == event.type &&
                    sqliteText(statement.stmt, 2) == event.code && storedValue == event.value;
  return same ? StoredEventIdentity::Same : StoredEventIdentity::Different;
}

bool insertEvent(SqliteConnection& connection, const string& deviceId, const DeviceSyncEvent& event) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT OR IGNORE INTO inventatory_device_events
      (event_id, device_id, event_type, event_code, event_value, received_at)
    VALUES (?, ?, ?, ?, ?, ?)
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, event.eventId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 3, event.type.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 4, event.code.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int(statement.stmt, 5, event.value);
  sqliteApi().bind_int64(statement.stmt, 6, static_cast<sqlite3_int64>(time(nullptr)));
  return sqliteApi().step(statement.stmt) == SQLITE_DONE;
}

bool acknowledgeResult(SqliteConnection& connection, const string& deviceId, const string& resultId) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "UPDATE inventatory_device_events SET result_acknowledged=1 WHERE device_id=? AND result_id=?",
      -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, resultId.c_str(), -1, SQLITE_TRANSIENT);
  return sqliteApi().step(statement.stmt) == SQLITE_DONE;
}

bool pruneAcknowledgedResults(SqliteConnection& connection, const string& deviceId) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    DELETE FROM inventatory_device_events
    WHERE device_id=? AND event_id IN (
      SELECT event_id FROM inventatory_device_events
      WHERE device_id=? AND state='completed' AND result_acknowledged=1
      ORDER BY completed_at DESC LIMIT -1 OFFSET 256
    )
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  return sqliteApi().step(statement.stmt) == SQLITE_DONE;
}

bool loadReceivedEventDeviceId(SqliteConnection& connection, const string& eventId, string& deviceId) {
  SqliteStatement statement;
  const char* sql = "SELECT device_id FROM inventatory_device_events "
                    "WHERE event_id=? AND state='received' LIMIT 1";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, eventId.c_str(), -1, SQLITE_TRANSIENT);
  if (sqliteApi().step(statement.stmt) != SQLITE_ROW ||
      sqliteApi().column_type(statement.stmt, 0) != SQLITE_TEXT) return false;
  deviceId = sqliteText(statement.stmt, 0);
  return !deviceId.empty();
}

vector<DeviceSyncResult> loadResults(SqliteConnection& connection, const string& deviceId, size_t limit) {
  vector<DeviceSyncResult> results;
  if (limit == 0) return results;
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT result_id, event_id, device_id, result_status, result_existing, result_item_name,
           result_requested_delta, result_applied_delta, result_quantity, result_location,
           result_code, result_message
    FROM inventatory_device_events
    WHERE device_id=? AND state='completed' AND result_acknowledged=0
    ORDER BY completed_at, received_at, rowid LIMIT ?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return results;
  sqliteApi().bind_text(statement.stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int(statement.stmt, 2, boundedSqliteLimit(limit));
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    DeviceSyncResult result;
    result.resultId = sqliteText(statement.stmt, 0);
    result.eventId = sqliteText(statement.stmt, 1);
    result.deviceId = sqliteText(statement.stmt, 2);
    result.status = sqliteText(statement.stmt, 3);
    int existing = 0;
    if (!sqliteInt32(statement.stmt, 4, existing) || !sqliteInt32(statement.stmt, 6, result.requestedDelta) ||
        !sqliteInt32(statement.stmt, 7, result.appliedDelta) || !sqliteInt32(statement.stmt, 8, result.quantity)) {
      return {};
    }
    result.existing = existing != 0;
    result.itemName = sqliteText(statement.stmt, 5);
    result.location = sqliteText(statement.stmt, 9);
    result.code = sqliteText(statement.stmt, 10);
    result.message = sqliteText(statement.stmt, 11);
    results.push_back(move(result));
  }
  if (stepResult != SQLITE_DONE) results.clear();
  return results;
}

// Counts the completed events whose result failed, then applies `sql` to them, in one transaction.
bool changeFailedEvents(const filesystem::path& databasePath, size_t& count, const char* sql) {
  count = 0;
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection) ||
      !execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) return false;
  const auto rollback = [&connection]() {
    execSql(connection, "ROLLBACK");
    return false;
  };

  SqliteStatement countStatement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT COUNT(*) FROM inventatory_device_events WHERE state='completed' AND result_status='failed'",
      -1, &countStatement.stmt, nullptr) != SQLITE_OK || sqliteApi().step(countStatement.stmt) != SQLITE_ROW ||
      !sqliteSize(countStatement.stmt, 0, count)) {
    return rollback();
  }
  if (!execSql(connection, sql) || !execSql(connection, "COMMIT")) return rollback();
  return true;
}

}  // namespace

bool acceptDeviceSyncEvents(const filesystem::path& databasePath, const DeviceSyncRequest& request,
                            DeviceSyncResponse& response, string& error) {
  response = {};
  response.requestId = request.requestId;
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection) ||
      !execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    error = "Unable to open the durable device inbox";
    return false;
  }

  bool ok = true;
  for (const auto& resultId : request.resultAcks) ok = acknowledgeResult(connection, request.deviceId, resultId) && ok;
  ok = pruneAcknowledgedResults(connection, request.deviceId) && ok;
  bool headConflict = false;
  for (size_t index = 0; index < request.events.size(); ++index) {
    const auto& event = request.events[index];
    const auto identity = storedEventIdentity(connection, request.deviceId, event);
    if (identity == StoredEventIdentity::Different) {
      // A clash on the head event refuses the request: nothing may be accepted ahead of it.
      // A clash further back is skipped, and the device raises it as its next head.
      if (index == 0) headConflict = true;
      continue;
    }
    if (identity == StoredEventIdentity::Failed) ok = false;
    if (identity == StoredEventIdentity::Unused) ok = insertEvent(connection, request.deviceId, event) && ok;
    if (ok) response.acceptedEventIds.push_back(event.eventId);
  }
  if (headConflict || !ok || !execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    error = headConflict ? "Device event id is already used by a different scan" : "Unable to persist device events";
    return false;
  }
  response.results = loadResults(connection, request.deviceId, 4);
  return true;
}

vector<DeviceSyncEvent> loadPendingDeviceSyncEvents(const filesystem::path& databasePath, size_t limit) {
  vector<DeviceSyncEvent> events;
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection)) return events;
  if (limit == 0) return events;
  SqliteStatement statement;
  // received_at has one-second resolution; ties go by rowid, never by the device's event id.
  const char* sql = R"SQL(
    SELECT event_id, device_id, event_type, event_code, event_value
    FROM inventatory_device_events WHERE state='received' ORDER BY received_at, rowid LIMIT ?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return events;
  sqliteApi().bind_int(statement.stmt, 1, boundedSqliteLimit(limit));
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    DeviceSyncEvent event;
    event.eventId = sqliteText(statement.stmt, 0);
    event.deviceId = sqliteText(statement.stmt, 1);
    event.type = sqliteText(statement.stmt, 2);
    event.code = sqliteText(statement.stmt, 3);
    if (!sqliteInt32(statement.stmt, 4, event.value)) return {};
    events.push_back(move(event));
  }
  if (stepResult != SQLITE_DONE) events.clear();
  return events;
}

vector<DeviceSyncEventRecord> loadDeviceSyncEventRecords(const filesystem::path& databasePath, size_t limit) {
  vector<DeviceSyncEventRecord> records;
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection)) return records;
  if (limit == 0) return records;
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT event_id, device_id, event_type, event_code, event_value, state,
           result_status, result_code, result_message, received_at, completed_at, result_acknowledged
    FROM inventatory_device_events
    ORDER BY CASE WHEN state='received' THEN 0 ELSE 1 END, received_at DESC, event_id DESC LIMIT ?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return records;
  sqliteApi().bind_int(statement.stmt, 1, boundedSqliteLimit(limit));
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    DeviceSyncEventRecord record;
    record.event.eventId = sqliteText(statement.stmt, 0);
    record.deviceId = sqliteText(statement.stmt, 1);
    record.event.type = sqliteText(statement.stmt, 2);
    record.event.code = sqliteText(statement.stmt, 3);
    if (!sqliteInt32(statement.stmt, 4, record.event.value) ||
        !sqliteTime(statement.stmt, 9, record.receivedAt) || !sqliteTime(statement.stmt, 10, record.completedAt)) return {};
    record.state = sqliteText(statement.stmt, 5);
    record.resultStatus = sqliteText(statement.stmt, 6);
    record.resultCode = sqliteText(statement.stmt, 7);
    record.resultMessage = sqliteText(statement.stmt, 8);
    int acknowledged = 0;
    if (!sqliteInt32(statement.stmt, 11, acknowledged)) return {};
    record.acknowledged = acknowledged != 0;
    records.push_back(move(record));
  }
  if (stepResult != SQLITE_DONE) records.clear();
  return records;
}

bool retryFailedDeviceSyncEvents(const filesystem::path& databasePath, size_t& retriedCount) {
  return changeFailedEvents(databasePath, retriedCount, R"SQL(
      UPDATE inventatory_device_events
      SET state='received', result_id='', result_status='', result_existing=0,
          result_item_name='', result_requested_delta=0, result_applied_delta=0,
          result_quantity=0, result_location='', result_code='', result_message='',
          result_acknowledged=0, completed_at=0
      WHERE state='completed' AND result_status='failed'
    )SQL");
}

bool discardFailedDeviceSyncEvents(const filesystem::path& databasePath, size_t& discardedCount) {
  return changeFailedEvents(databasePath, discardedCount,
                            "DELETE FROM inventatory_device_events WHERE state='completed' AND result_status='failed'");
}

DeviceLookupResult lookupDeviceItem(const filesystem::path& databasePath, const DeviceLookupRequest& request) {
  DeviceLookupResult result;
  result.lookupId = request.lookupId;
  const auto code = trim(request.code);
  if (code.empty() || code.size() > 64) {
    result.status = "not_found";
    return result;
  }

  SqliteConnection connection;
  if (!openDatabase(databasePath, connection)) {
    result.status = "unavailable";
    return result;
  }

  SqliteStatement statement;
  constexpr char kLookupSql[] =
      "SELECT id, part_name FROM inventatory_items "
      "WHERE machine_code=? COLLATE NOCASE OR digikey_part_number=? COLLATE NOCASE "
      "OR sku=? COLLATE NOCASE LIMIT 1";
  if (sqliteApi().prepare_v2(connection.db, kLookupSql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    result.status = "unavailable";
    return result;
  }
  sqliteApi().bind_text(statement.stmt, 1, code.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, code.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 3, code.c_str(), -1, SQLITE_TRANSIENT);
  if (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    result.status = "found";
    const auto itemId = sqliteText(statement.stmt, 0);
    result.itemName = sqliteText(statement.stmt, 1);
    // Finish the read before loading the item on its own connection.
    sqliteApi().reset(statement.stmt);
    if (result.itemName.size() > kDeviceItemNameLimit) {
      InventoryStore store;
      const auto* item = store.load(databasePath) ? store.findById(itemId) : nullptr;
      if (item != nullptr) result.itemName = deviceItemName(*item);
    }
    return result;
  }
  result.status = "not_found";
  return result;
}

bool completeDeviceSyncEvent(InventoryStore& store, const filesystem::path& databasePath,
                             const DeviceSyncResult& result, const InventoryStore* previousStore) {
  // Finalize IDs on the snapshot that is committed and returned to the app. saveWithCommit()
  // normalizes only a private copy, so the live item would otherwise lack its IDs for auto-label.
  auto finalized = store;
  ensureInventoryIdentifiers(finalized.items());

  if (result.eventId.empty()) return false;
  string deviceId = result.deviceId;
  if (deviceId.empty()) {
    SqliteConnection identityConnection;
    if (!openDatabaseReadOnly(databasePath, identityConnection) ||
        !validateInventoryDatabase(identityConnection) ||
        !loadReceivedEventDeviceId(identityConnection, result.eventId, deviceId)) {
      // Without an explicit device id, take it from the pending inbox row, so a completion
      // cannot match an arbitrary event id.
      return false;
    }
  }

  DeviceEventCommit commit;
  commit.eventId = result.eventId;
  commit.deviceId = deviceId;
  commit.resultId = result.resultId;
  commit.status = result.status;
  commit.existing = result.existing;
  commit.itemName = result.itemName;
  commit.requestedDelta = result.requestedDelta;
  commit.appliedDelta = result.appliedDelta;
  commit.quantity = result.quantity;
  commit.location = result.location;
  commit.code = result.code;
  commit.message = result.message;
  commit.completedAt = time(nullptr);
  InventoryStore persistedStore;
  if (previousStore == nullptr && !persistedStore.load(databasePath)) return false;
  const auto& movementBaseline = previousStore == nullptr ? persistedStore : *previousStore;
  const auto movements = inventoryMovementDiff(movementBaseline, finalized, "scanner", result.eventId,
                                               commit.completedAt);
  InventoryCommitDraft draft;
  draft.source = "scanner";
  draft.reference = result.eventId;
  draft.message = result.message.empty() ? "Scanner update" : result.message;
  if (!finalized.saveWithCommit(databasePath, movementBaseline, draft, movements, &commit, nullptr)) return false;
  store = move(finalized);
  return true;
}

}  // namespace inventatory
