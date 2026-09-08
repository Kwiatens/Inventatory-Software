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

#ifdef _WIN32
namespace {

int boundedSqliteLimit(size_t limit) {
  return static_cast<int>(min(limit, static_cast<size_t>(numeric_limits<int>::max())));
}

bool ensureDeviceSyncSchema(SqliteConnection& connection) {
  return ensureInventoryDatabaseSchema(connection);
}

bool eventIdentityMatches(SqliteConnection& connection, const string& eventId, const string& deviceId,
                          bool& exists) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT device_id FROM inventatory_device_events WHERE event_id=? LIMIT 1", -1, &statement.stmt,
      nullptr) != SQLITE_OK) {
    return false;
  }
  sqliteApi().bind_text(statement.stmt, 1, eventId.c_str(), -1, SQLITE_TRANSIENT);
  const auto step = sqliteApi().step(statement.stmt);
  if (step == SQLITE_DONE) {
    exists = false;
    return true;
  }
  if (step != SQLITE_ROW || sqliteApi().column_type(statement.stmt, 0) != SQLITE_TEXT) return false;
  exists = true;
  return sqliteText(statement.stmt, 0) == deviceId;
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
    ORDER BY completed_at, received_at LIMIT ?
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

}  // namespace
#endif

bool acceptDeviceSyncEvents(const filesystem::path& databasePath, const DeviceSyncRequest& request,
                            DeviceSyncResponse& response, string& error) {
  response = {};
  response.requestId = request.requestId;
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureDeviceSyncSchema(connection) ||
      !execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    error = "Unable to open the durable device inbox";
    return false;
  }

  bool ok = true;
  for (const auto& resultId : request.resultAcks) ok = acknowledgeResult(connection, request.deviceId, resultId) && ok;
  ok = pruneAcknowledgedResults(connection, request.deviceId) && ok;
  for (const auto& event : request.events) {
    bool alreadyStored = false;
    if (!eventIdentityMatches(connection, event.eventId, request.deviceId, alreadyStored)) ok = false;
    if (!alreadyStored) ok = insertEvent(connection, request.deviceId, event) && ok;
    if (ok) response.acceptedEventIds.push_back(event.eventId);
  }
  if (!ok || !execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    error = "Unable to persist device events";
    return false;
  }
  response.results = loadResults(connection, request.deviceId, 4);
  return true;
#else
  (void)databasePath;
  (void)request;
  error = "Inventatory Scan persistence requires SQLite";
  return false;
#endif
}

vector<DeviceSyncEvent> loadPendingDeviceSyncEvents(const filesystem::path& databasePath, size_t limit) {
  vector<DeviceSyncEvent> events;
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureDeviceSyncSchema(connection)) return events;
  if (limit == 0) return events;
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT event_id, device_id, event_type, event_code, event_value
    FROM inventatory_device_events WHERE state='received' ORDER BY received_at, event_id LIMIT ?
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
#else
  (void)databasePath;
  (void)limit;
#endif
  return events;
}

vector<DeviceSyncEventRecord> loadDeviceSyncEventRecords(const filesystem::path& databasePath, size_t limit) {
  vector<DeviceSyncEventRecord> records;
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureDeviceSyncSchema(connection)) return records;
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
#else
  (void)databasePath;
  (void)limit;
#endif
  return records;
}

bool retryFailedDeviceSyncEvents(const filesystem::path& databasePath, size_t& retriedCount) {
  retriedCount = 0;
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureDeviceSyncSchema(connection) ||
      !execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) return false;

  SqliteStatement countStatement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT COUNT(*) FROM inventatory_device_events WHERE state='completed' AND result_status='failed'",
      -1, &countStatement.stmt, nullptr) != SQLITE_OK || sqliteApi().step(countStatement.stmt) != SQLITE_ROW) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  if (!sqliteSize(countStatement.stmt, 0, retriedCount)) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  if (!execSql(connection, R"SQL(
      UPDATE inventatory_device_events
      SET state='received', result_id='', result_status='', result_existing=0,
          result_item_name='', result_requested_delta=0, result_applied_delta=0,
          result_quantity=0, result_location='', result_code='', result_message='',
          result_acknowledged=0, completed_at=0
      WHERE state='completed' AND result_status='failed'
    )SQL")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  if (!execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  return true;
#else
  (void)databasePath;
  return false;
#endif
}

bool discardFailedDeviceSyncEvents(const filesystem::path& databasePath, size_t& discardedCount) {
  discardedCount = 0;
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureDeviceSyncSchema(connection) ||
      !execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) return false;

  SqliteStatement countStatement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT COUNT(*) FROM inventatory_device_events WHERE state='completed' AND result_status='failed'",
      -1, &countStatement.stmt, nullptr) != SQLITE_OK || sqliteApi().step(countStatement.stmt) != SQLITE_ROW) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  if (!sqliteSize(countStatement.stmt, 0, discardedCount)) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  if (!execSql(connection,
               "DELETE FROM inventatory_device_events WHERE state='completed' AND result_status='failed'")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  if (!execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  return true;
#else
  (void)databasePath;
  return false;
#endif
}

DeviceLookupResult lookupDeviceItem(const filesystem::path& databasePath, const DeviceLookupRequest& request) {
  DeviceLookupResult result;
  result.lookupId = request.lookupId;
  const auto code = trim(request.code);
  if (code.empty() || code.size() > 64) {
    result.status = "not_found";
    return result;
  }

#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection)) {
    result.status = "unavailable";
    return result;
  }

  SqliteStatement statement;
  constexpr char kLookupSql[] =
      "SELECT part_name FROM inventatory_items "
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
    result.itemName = sqliteText(statement.stmt, 0);
    return result;
  }
  result.status = "not_found";
#else
  (void)databasePath;
  result.status = "unavailable";
#endif
  return result;
}

bool completeDeviceSyncEvent(InventoryStore& store, const filesystem::path& databasePath,
                             const DeviceSyncResult& result, const InventoryStore* previousStore) {
  // Finalize identifiers in the same snapshot that is committed and returned
  // to the application. InventoryStore::saveWithDeviceEvent() normalizes a
  // private copy, which is sufficient for SQLite but would otherwise leave a
  // newly received item without its Inventatory ID/machine code in live memory. The
  // auto-label path prints from that live item immediately after this call.
  auto finalized = store;
  ensureInventoryIdentifiers(finalized.items());

#ifdef _WIN32
  if (result.eventId.empty()) return false;
  string deviceId = result.deviceId;
  if (deviceId.empty()) {
    SqliteConnection identityConnection;
    if (!openDatabaseReadOnly(databasePath, identityConnection) ||
        !validateInventoryDatabase(identityConnection) ||
        !loadReceivedEventDeviceId(identityConnection, result.eventId, deviceId)) {
      // A result without an explicit identity is accepted only when the
      // still-pending inbox row supplies it.  This keeps legacy callers safe
      // while preventing a completion from matching an arbitrary event ID.
      return false;
    }
  }
#else
  const string deviceId = result.deviceId;
#endif

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
