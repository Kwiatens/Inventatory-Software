// Inventatory - Hardware Inventory Management System
// Durable inbox and result delivery for Inventatory Scan protocol v2.

#include "core/InventatoryScanProtocol.h"

#include "core/InventoryInternals.h"
#include "core/InventorySqlite.h"

#include <algorithm>
#include <ctime>

namespace inventatory {

using namespace std;

#ifdef _WIN32
namespace {

bool ensureDeviceSyncSchema(SqliteConnection& connection) {
  if (!execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS inventatory_device_events (
      event_id TEXT PRIMARY KEY,
      device_id TEXT NOT NULL,
      event_type TEXT NOT NULL,
      event_code TEXT NOT NULL,
      event_value INTEGER NOT NULL,
      component_manufacturer TEXT NOT NULL DEFAULT '',
      component_mpn TEXT NOT NULL DEFAULT '',
      component_name TEXT NOT NULL DEFAULT '',
      state TEXT NOT NULL DEFAULT 'received',
      result_id TEXT NOT NULL DEFAULT '',
      result_status TEXT NOT NULL DEFAULT '',
      result_existing INTEGER NOT NULL DEFAULT 0,
      result_item_name TEXT NOT NULL DEFAULT '',
      result_purpose_label TEXT NOT NULL DEFAULT '',
      result_requested_delta INTEGER NOT NULL DEFAULT 0,
      result_applied_delta INTEGER NOT NULL DEFAULT 0,
      result_quantity INTEGER NOT NULL DEFAULT 0,
      result_location TEXT NOT NULL DEFAULT '',
      result_code TEXT NOT NULL DEFAULT '',
      result_message TEXT NOT NULL DEFAULT '',
      result_acknowledged INTEGER NOT NULL DEFAULT 0,
      received_at INTEGER NOT NULL DEFAULT 0,
      completed_at INTEGER NOT NULL DEFAULT 0
    )
  )SQL")) return false;
  if (!tableColumnExists(connection, "inventatory_device_events", "component_manufacturer") &&
      !execSql(connection, "ALTER TABLE inventatory_device_events ADD COLUMN component_manufacturer TEXT NOT NULL DEFAULT ''")) return false;
  if (!tableColumnExists(connection, "inventatory_device_events", "component_mpn") &&
      !execSql(connection, "ALTER TABLE inventatory_device_events ADD COLUMN component_mpn TEXT NOT NULL DEFAULT ''")) return false;
  if (!tableColumnExists(connection, "inventatory_device_events", "component_name") &&
      !execSql(connection, "ALTER TABLE inventatory_device_events ADD COLUMN component_name TEXT NOT NULL DEFAULT ''")) return false;
  if (!tableColumnExists(connection, "inventatory_device_events", "result_purpose_label") &&
      !execSql(connection, "ALTER TABLE inventatory_device_events ADD COLUMN result_purpose_label TEXT NOT NULL DEFAULT ''")) return false;
  return execSql(connection,
      "CREATE INDEX IF NOT EXISTS idx_inventatory_device_events_delivery "
      "ON inventatory_device_events(device_id, state, result_acknowledged, completed_at)");
}

bool eventExists(SqliteConnection& connection, const string& eventId) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT 1 FROM inventatory_device_events WHERE event_id=? LIMIT 1", -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqliteApi().bind_text(statement.stmt, 1, eventId.c_str(), -1, SQLITE_TRANSIENT);
  return sqliteApi().step(statement.stmt) == SQLITE_ROW;
}

bool insertEvent(SqliteConnection& connection, const string& deviceId, const DeviceSyncEvent& event) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT OR IGNORE INTO inventatory_device_events
      (event_id, device_id, event_type, event_code, event_value,
       component_manufacturer, component_mpn, component_name, received_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  sqliteApi().bind_text(statement.stmt, 1, event.eventId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 3, event.type.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 4, event.code.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int(statement.stmt, 5, event.value);
  sqliteApi().bind_text(statement.stmt, 6, event.manufacturer.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 7, event.manufacturerPartNumber.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 8, event.encodedPartName.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int64(statement.stmt, 9, static_cast<sqlite3_int64>(time(nullptr)));
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

vector<DeviceSyncResult> loadResults(SqliteConnection& connection, const string& deviceId, size_t limit) {
  vector<DeviceSyncResult> results;
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT result_id, event_id, result_status, result_existing, result_item_name, result_purpose_label,
           result_requested_delta, result_applied_delta, result_quantity, result_location,
           result_code, result_message
    FROM inventatory_device_events
    WHERE device_id=? AND state='completed' AND result_acknowledged=0
    ORDER BY completed_at, received_at LIMIT ?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return results;
  sqliteApi().bind_text(statement.stmt, 1, deviceId.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int(statement.stmt, 2, static_cast<int>(limit));
  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    DeviceSyncResult result;
    result.resultId = sqliteText(statement.stmt, 0);
    result.eventId = sqliteText(statement.stmt, 1);
    result.status = sqliteText(statement.stmt, 2);
    result.existing = sqliteApi().column_int(statement.stmt, 3) != 0;
    result.itemName = sqliteText(statement.stmt, 4);
    result.purposeLabel = sqliteText(statement.stmt, 5);
    result.requestedDelta = sqliteApi().column_int(statement.stmt, 6);
    result.appliedDelta = sqliteApi().column_int(statement.stmt, 7);
    result.quantity = sqliteApi().column_int(statement.stmt, 8);
    result.location = sqliteText(statement.stmt, 9);
    result.code = sqliteText(statement.stmt, 10);
    result.message = sqliteText(statement.stmt, 11);
    results.push_back(move(result));
  }
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
    const bool alreadyStored = eventExists(connection, event.eventId);
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
  error = "Protocol v2 persistence requires SQLite";
  return false;
#endif
}

vector<DeviceSyncEvent> loadPendingDeviceSyncEvents(const filesystem::path& databasePath, size_t limit) {
  vector<DeviceSyncEvent> events;
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureDeviceSyncSchema(connection)) return events;
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT event_id, event_type, event_code, event_value,
           component_manufacturer, component_mpn, component_name
    FROM inventatory_device_events WHERE state='received' ORDER BY received_at, event_id LIMIT ?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return events;
  sqliteApi().bind_int(statement.stmt, 1, static_cast<int>(limit));
  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    events.push_back({sqliteText(statement.stmt, 0), sqliteText(statement.stmt, 1),
                      sqliteText(statement.stmt, 2), sqliteApi().column_int(statement.stmt, 3),
                      sqliteText(statement.stmt, 4), sqliteText(statement.stmt, 5),
                      sqliteText(statement.stmt, 6)});
  }
#else
  (void)databasePath;
  (void)limit;
#endif
  return events;
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
      "SELECT CASE WHEN iecd_canonical_name<>'' THEN iecd_canonical_name ELSE part_name END, iecd_purpose_label FROM inventatory_items "
      "WHERE machine_code=? COLLATE NOCASE OR manufacturer_part_number=? COLLATE NOCASE LIMIT 1";
  if (sqliteApi().prepare_v2(connection.db, kLookupSql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    result.status = "unavailable";
    return result;
  }
  sqliteApi().bind_text(statement.stmt, 1, code.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, code.c_str(), -1, SQLITE_TRANSIENT);
  if (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    result.status = "found";
    result.itemName = sqliteText(statement.stmt, 0);
    result.purposeLabel = sqliteText(statement.stmt, 1);
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
                             const DeviceSyncResult& result) {
  // Finalize identifiers in the same snapshot that is committed and returned
  // to the application. InventoryStore::saveWithDeviceEvent() normalizes a
  // private copy, which is sufficient for SQLite but would otherwise leave a
  // newly received item without its Inventatory ID/machine code in live memory. The
  // auto-label path prints from that live item immediately after this call.
  auto finalized = store;
  ensureInventoryIdentifiers(finalized.items());

  DeviceEventCommit commit;
  commit.eventId = result.eventId;
  commit.resultId = result.resultId;
  commit.status = result.status;
  commit.existing = result.existing;
  commit.itemName = result.itemName;
  commit.purposeLabel = result.purposeLabel;
  commit.requestedDelta = result.requestedDelta;
  commit.appliedDelta = result.appliedDelta;
  commit.quantity = result.quantity;
  commit.location = result.location;
  commit.code = result.code;
  commit.message = result.message;
  commit.completedAt = time(nullptr);
  if (!finalized.saveWithDeviceEvent(databasePath, commit)) return false;
  store = move(finalized);
  return true;
}

}  // namespace inventatory
