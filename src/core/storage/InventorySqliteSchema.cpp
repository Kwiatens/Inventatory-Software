// Inventatory - SQLite schema creation and integrity validation.

#include "core/storage/InventorySqlite.h"
#include "core/history/InventoryVersionInternal.h"

#include <initializer_list>
#include <limits>
#include <sstream>

namespace inventatory {

using namespace std;

#ifdef _WIN32

namespace {
void setSqlError(SqliteConnection& connection, string* error, const string& fallback) {
  if (error == nullptr) return;
  const char* message = connection.db == nullptr ? nullptr : sqliteApi().errmsg(connection.db);
  // sqlite3_errmsg() may still report "not an error" after a failed helper
  // query has finalized its statement.  Keep the operation-specific context
  // instead of replacing it with that non-diagnostic status.
  *error = message == nullptr || *message == '\0' || string(message) == "not an error" ? fallback : message;
}

bool queryInt64(SqliteConnection& connection, const string& sql, sqlite3_int64& value) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db, sql.c_str(), -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  if (sqliteApi().step(statement.stmt) != SQLITE_ROW || sqliteApi().column_type(statement.stmt, 0) != SQLITE_INTEGER) {
    return false;
  }
  value = sqliteApi().column_int64(statement.stmt, 0);
  return true;
}

bool queryString(SqliteConnection& connection, const string& sql, string& value) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db, sql.c_str(), -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  if (sqliteApi().step(statement.stmt) != SQLITE_ROW || sqliteApi().column_type(statement.stmt, 0) != SQLITE_TEXT) {
    return false;
  }
  value = sqliteText(statement.stmt, 0);
  return true;
}

bool queryHasRows(SqliteConnection& connection, const string& sql, bool& hasRows) {
  SqliteStatement statement;
  hasRows = false;
  if (sqliteApi().prepare_v2(connection.db, sql.c_str(), -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  const int result = sqliteApi().step(statement.stmt);
  if (result == SQLITE_ROW) {
    hasRows = true;
    return true;
  }
  return result == SQLITE_DONE;
}

bool requireColumns(SqliteConnection& connection, const string& table, initializer_list<const char*> columns,
                    string* error) {
  if (!tableExists(connection, table)) {
    setSqlError(connection, error, "Missing SQLite table: " + table);
    return false;
  }
  for (const char* column : columns) {
    if (!tableColumnExists(connection, table, column)) {
      setSqlError(connection, error, "Missing SQLite column: " + table + "." + column);
      return false;
    }
  }
  return true;
}
}  // namespace

namespace {
bool createSchemaObjects(SqliteConnection& connection, string* error) {
  const char* const statements[] = {
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_items (
          id TEXT PRIMARY KEY, part_name TEXT NOT NULL, manufacturer TEXT NOT NULL, category TEXT NOT NULL,
          quantity INTEGER NOT NULL, reorder_threshold INTEGER NOT NULL, location TEXT NOT NULL,
          tags TEXT NOT NULL, parameters TEXT NOT NULL, notes TEXT NOT NULL,
          label_override TEXT NOT NULL DEFAULT '', vendor_provider TEXT NOT NULL DEFAULT '',
          vendor_product_number TEXT NOT NULL DEFAULT '', vendor_manufacturer_part_number TEXT NOT NULL DEFAULT '',
          vendor_category_id TEXT NOT NULL DEFAULT '', vendor_category_path TEXT NOT NULL DEFAULT '',
          vendor_title TEXT NOT NULL DEFAULT '', vendor_detailed_description TEXT NOT NULL DEFAULT '',
          vendor_parameters TEXT NOT NULL DEFAULT '', vendor_product_url TEXT NOT NULL DEFAULT '',
          vendor_locale TEXT NOT NULL DEFAULT '', digikey_part_number TEXT NOT NULL, datasheet_url TEXT NOT NULL,
          product_url TEXT NOT NULL, sync_status TEXT NOT NULL, sku TEXT NOT NULL, last_updated INTEGER NOT NULL,
          inventatory_id TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL DEFAULT 0,
          machine_code TEXT NOT NULL DEFAULT '', rack_id TEXT NOT NULL DEFAULT '', rack_slot TEXT NOT NULL DEFAULT '',
          rack_assignment TEXT NOT NULL DEFAULT 'automatic'
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_racks (
          id TEXT PRIMARY KEY, code TEXT NOT NULL UNIQUE, component_type TEXT NOT NULL,
          rows_count INTEGER NOT NULL DEFAULT 5, columns_count INTEGER NOT NULL DEFAULT 5,
          created_at INTEGER NOT NULL DEFAULT 0
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_stock_movements (
          movement_id TEXT PRIMARY KEY, item_id TEXT NOT NULL, item_name TEXT NOT NULL,
          source TEXT NOT NULL, reference TEXT NOT NULL DEFAULT '', quantity_before INTEGER NOT NULL,
          delta INTEGER NOT NULL, quantity_after INTEGER NOT NULL, occurred_at INTEGER NOT NULL
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_device_events (
          event_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, event_type TEXT NOT NULL, event_code TEXT NOT NULL,
          event_value INTEGER NOT NULL, state TEXT NOT NULL DEFAULT 'received', result_id TEXT NOT NULL DEFAULT '',
          result_status TEXT NOT NULL DEFAULT '', result_existing INTEGER NOT NULL DEFAULT 0,
          result_item_name TEXT NOT NULL DEFAULT '', result_requested_delta INTEGER NOT NULL DEFAULT 0,
          result_applied_delta INTEGER NOT NULL DEFAULT 0, result_quantity INTEGER NOT NULL DEFAULT 0,
          result_location TEXT NOT NULL DEFAULT '', result_code TEXT NOT NULL DEFAULT '',
          result_message TEXT NOT NULL DEFAULT '', result_acknowledged INTEGER NOT NULL DEFAULT 0,
          received_at INTEGER NOT NULL DEFAULT 0, completed_at INTEGER NOT NULL DEFAULT 0
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_inventory_history (
          timestamp INTEGER NOT NULL, item_count INTEGER NOT NULL, total_units INTEGER NOT NULL,
          low_stock_count INTEGER NOT NULL, out_of_stock_count INTEGER NOT NULL, data_error_count INTEGER NOT NULL
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_inventory_commits (
          commit_id TEXT PRIMARY KEY, sequence INTEGER NOT NULL UNIQUE, parent_id TEXT NOT NULL DEFAULT '',
          committed_at INTEGER NOT NULL, source TEXT NOT NULL, reference TEXT NOT NULL DEFAULT '', message TEXT NOT NULL,
          checkpoint INTEGER NOT NULL DEFAULT 0, corrective INTEGER NOT NULL DEFAULT 0,
          reverted_commit_id TEXT NOT NULL DEFAULT '', changed_item_count INTEGER NOT NULL DEFAULT 0,
          changed_rack_count INTEGER NOT NULL DEFAULT 0, snapshot_version INTEGER NOT NULL DEFAULT 1
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_inventory_commit_items (
          commit_id TEXT NOT NULL, item_id TEXT NOT NULL, item_data TEXT NOT NULL, PRIMARY KEY (commit_id, item_id)
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_inventory_commit_racks (
          commit_id TEXT NOT NULL, rack_id TEXT NOT NULL, rack_data TEXT NOT NULL, PRIMARY KEY (commit_id, rack_id)
        )
      )SQL",
      R"SQL(
        CREATE TABLE IF NOT EXISTS inventatory_bom_projects (
          id TEXT PRIMARY KEY, name TEXT NOT NULL, source_path TEXT NOT NULL, boards INTEGER NOT NULL DEFAULT 1,
          created_at INTEGER NOT NULL DEFAULT 0, last_opened INTEGER NOT NULL DEFAULT 0,
          last_built INTEGER NOT NULL DEFAULT 0, bom_text TEXT NOT NULL,
          overrides TEXT NOT NULL DEFAULT '', enrichment TEXT NOT NULL DEFAULT ''
        )
      )SQL",
  };
  for (const char* statement : statements) {
    if (!execSql(connection, statement)) {
      setSqlError(connection, error, "Unable to create the Inventatory SQLite schema");
      return false;
    }
  }
  return true;
}

bool createSchemaIndexes(SqliteConnection& connection, string* error) {
  const char* const statements[] = {
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_inventatory_items_rack_slot ON inventatory_items(rack_id, rack_slot) WHERE rack_id <> '' AND rack_slot <> ''",
      "CREATE INDEX IF NOT EXISTS idx_inventatory_stock_movements_item_time ON inventatory_stock_movements(item_id, occurred_at DESC, movement_id DESC)",
      "CREATE INDEX IF NOT EXISTS idx_inventatory_device_events_delivery ON inventatory_device_events(device_id, state, result_acknowledged, completed_at)",
      "CREATE INDEX IF NOT EXISTS idx_inventatory_inventory_commits_sequence ON inventatory_inventory_commits(sequence DESC)",
  };
  for (const char* statement : statements) {
    if (!execSql(connection, statement)) {
      setSqlError(connection, error, "Unable to create Inventatory SQLite indexes");
      return false;
    }
  }
  return true;
}

bool validateExistingColumns(SqliteConnection& connection, string* error) {
  if (!requireColumns(connection, "inventatory_items",
                     {"id", "part_name", "manufacturer", "category", "quantity", "reorder_threshold", "location",
                      "tags", "parameters", "notes", "digikey_part_number", "datasheet_url", "product_url",
                      "sync_status", "sku", "last_updated"},
                     error)) return false;
  if (!requireColumns(connection, "inventatory_racks", {"id", "code", "component_type"}, error)) return false;
  if (!requireColumns(connection, "inventatory_stock_movements",
                     {"movement_id", "item_id", "item_name", "source", "quantity_before", "delta", "quantity_after",
                      "occurred_at"},
                     error)) return false;
  if (!requireColumns(connection, "inventatory_device_events",
                     {"event_id", "device_id", "event_type", "event_code", "event_value", "state"}, error)) return false;
  if (!requireColumns(connection, "inventatory_inventory_history",
                     {"timestamp", "item_count", "total_units", "low_stock_count", "out_of_stock_count", "data_error_count"},
                     error)) return false;
  if (!requireColumns(connection, "inventatory_inventory_commits",
                     {"commit_id", "sequence", "parent_id", "committed_at", "source", "message"}, error)) return false;
  if (!requireColumns(connection, "inventatory_inventory_commit_items", {"commit_id", "item_id", "item_data"}, error)) {
    return false;
  }
  if (!requireColumns(connection, "inventatory_inventory_commit_racks", {"commit_id", "rack_id", "rack_data"}, error)) {
    return false;
  }
  return requireColumns(connection, "inventatory_bom_projects", {"id", "name", "source_path", "bom_text"}, error);
}

bool validateDataValues(SqliteConnection& connection, string* error) {
  const auto intMin = to_string(numeric_limits<int>::min());
  const auto intMax = to_string(numeric_limits<int>::max());
  const string checks[] = {
      "SELECT 1 FROM inventatory_items WHERE typeof(id)<>'text' OR trim(id)='' OR "
      "typeof(quantity)<>'integer' OR quantity < " + intMin + " OR quantity > " + intMax + " OR "
      "typeof(reorder_threshold)<>'integer' OR reorder_threshold < " + intMin + " OR reorder_threshold > " + intMax + " OR "
      "typeof(last_updated)<>'integer' OR last_updated < 0 OR typeof(created_at)<>'integer' OR created_at < 0 LIMIT 1",
      "SELECT 1 FROM inventatory_racks WHERE typeof(id)<>'text' OR trim(id)='' OR typeof(code)<>'text' OR trim(code)='' OR "
      "typeof(rows_count)<>'integer' OR typeof(columns_count)<>'integer' OR rows_count <= 0 OR columns_count <= 0 OR "
      "rows_count > 10000 OR columns_count > 10000 OR typeof(created_at)<>'integer' OR created_at < 0 LIMIT 1",
      "SELECT 1 FROM inventatory_stock_movements WHERE typeof(movement_id)<>'text' OR trim(movement_id)='' OR "
      "typeof(quantity_before)<>'integer' OR quantity_before < " + intMin + " OR quantity_before > " + intMax + " OR "
      "typeof(delta)<>'integer' OR delta < " + intMin + " OR delta > " + intMax + " OR "
      "typeof(quantity_after)<>'integer' OR quantity_after < " + intMin + " OR quantity_after > " + intMax + " OR "
      "typeof(occurred_at)<>'integer' OR occurred_at < 0 LIMIT 1",
      "SELECT 1 FROM inventatory_device_events WHERE typeof(event_id)<>'text' OR trim(event_id)='' OR "
      "typeof(device_id)<>'text' OR trim(device_id)='' OR typeof(event_value)<>'integer' OR event_value < " + intMin + " OR event_value > " + intMax + " OR "
      "typeof(result_existing)<>'integer' OR result_existing NOT IN (0, 1) OR "
      "typeof(result_requested_delta)<>'integer' OR result_requested_delta < " + intMin + " OR result_requested_delta > " + intMax + " OR "
      "typeof(result_applied_delta)<>'integer' OR result_applied_delta < " + intMin + " OR result_applied_delta > " + intMax + " OR "
      "typeof(result_quantity)<>'integer' OR result_quantity < " + intMin + " OR result_quantity > " + intMax + " OR "
      "typeof(result_acknowledged)<>'integer' OR result_acknowledged NOT IN (0, 1) OR "
      "typeof(received_at)<>'integer' OR received_at < 0 OR typeof(completed_at)<>'integer' OR completed_at < 0 LIMIT 1",
      "SELECT 1 FROM inventatory_inventory_commits WHERE typeof(commit_id)<>'text' OR trim(commit_id)='' OR "
      "typeof(sequence)<>'integer' OR sequence <= 0 OR typeof(committed_at)<>'integer' OR committed_at < 0 OR "
      "typeof(changed_item_count)<>'integer' OR changed_item_count < 0 OR "
      "typeof(changed_rack_count)<>'integer' OR changed_rack_count < 0 LIMIT 1",
      "SELECT 1 FROM inventatory_bom_projects WHERE typeof(id)<>'text' OR trim(id)='' OR "
      "typeof(boards)<>'integer' OR boards <= 0 OR typeof(created_at)<>'integer' OR created_at < 0 OR "
      "typeof(last_opened)<>'integer' OR last_opened < 0 OR typeof(last_built)<>'integer' OR last_built < 0 LIMIT 1",
      "SELECT 1 FROM inventatory_inventory_history WHERE typeof(timestamp)<>'integer' OR timestamp < 0 OR "
      "typeof(item_count)<>'integer' OR item_count < 0 OR typeof(total_units)<>'integer' OR total_units < 0 OR "
      "typeof(low_stock_count)<>'integer' OR low_stock_count < 0 OR typeof(out_of_stock_count)<>'integer' OR out_of_stock_count < 0 OR "
      "typeof(data_error_count)<>'integer' OR data_error_count < 0 LIMIT 1",
  };
  for (const auto& check : checks) {
    bool hasRows = false;
    if (!queryHasRows(connection, check, hasRows)) {
      setSqlError(connection, error, "Unable to validate Inventatory SQLite data");
      return false;
    }
    if (hasRows) {
      setSqlError(connection, error, "Invalid value in the Inventatory SQLite database");
      return false;
    }
  }
  const string duplicateChecks[] = {
      "SELECT 1 FROM inventatory_items WHERE trim(inventatory_id)<>'' GROUP BY lower(trim(inventatory_id)) HAVING COUNT(*) > 1 LIMIT 1",
      "SELECT 1 FROM inventatory_items WHERE trim(machine_code)<>'' GROUP BY lower(trim(machine_code)) HAVING COUNT(*) > 1 LIMIT 1",
      "SELECT 1 FROM inventatory_racks GROUP BY lower(trim(id)) HAVING COUNT(*) > 1 LIMIT 1",
      "SELECT 1 FROM inventatory_racks GROUP BY lower(trim(code)) HAVING COUNT(*) > 1 LIMIT 1",
      "SELECT 1 FROM inventatory_bom_projects GROUP BY lower(trim(id)) HAVING COUNT(*) > 1 LIMIT 1",
  };
  for (const auto& check : duplicateChecks) {
    bool hasRows = false;
    if (!queryHasRows(connection, check, hasRows)) {
      setSqlError(connection, error, "Unable to validate Inventatory SQLite identifiers");
      return false;
    }
    if (hasRows) {
      setSqlError(connection, error, "Duplicate identifier in the Inventatory SQLite database");
      return false;
    }
  }
  return true;
}

bool readIntegrityOk(SqliteConnection& connection, string* error) {
  string result;
  if (!queryString(connection, "PRAGMA integrity_check", result) || result != "ok") {
    setSqlError(connection, error, "SQLite integrity check failed");
    return false;
  }
  return true;
}

bool validateCurrentSchema(SqliteConnection& connection, string* error) {
  if (!validateExistingColumns(connection, error)) return false;
  if (!requireColumns(connection, "inventatory_racks", {"rows_count", "columns_count", "created_at"}, error) ||
      !requireColumns(connection, "inventatory_stock_movements", {"reference"}, error) ||
      !requireColumns(connection, "inventatory_device_events",
                      {"result_id", "result_status", "result_existing", "result_item_name", "result_requested_delta",
                       "result_applied_delta", "result_quantity", "result_location", "result_code", "result_message",
                       "result_acknowledged", "received_at", "completed_at"},
                      error) ||
      !requireColumns(connection, "inventatory_inventory_commits",
                      {"reference", "checkpoint", "corrective", "reverted_commit_id", "changed_item_count",
                       "changed_rack_count", "snapshot_version"},
                      error) ||
      !requireColumns(connection, "inventatory_bom_projects",
                      {"boards", "created_at", "last_opened", "last_built", "overrides", "enrichment"}, error)) {
    return false;
  }
  const char* const itemColumns[] = {
      "label_override", "vendor_provider", "vendor_product_number", "vendor_manufacturer_part_number",
      "vendor_category_id", "vendor_category_path", "vendor_title", "vendor_detailed_description", "vendor_parameters",
      "vendor_product_url", "vendor_locale", "inventatory_id", "created_at", "machine_code", "rack_id", "rack_slot",
      "rack_assignment",
  };
  for (const char* column : itemColumns) {
    if (!tableColumnExists(connection, "inventatory_items", column)) {
      setSqlError(connection, error, "Incomplete Inventatory SQLite item schema");
      return false;
    }
  }
  return validateDataValues(connection, error);
}
}  // namespace

bool ensureInventoryDatabaseSchema(SqliteConnection& connection, string* error) {
  if (connection.db == nullptr) {
    if (error != nullptr) *error = "SQLite connection is not open";
    return false;
  }
  sqlite3_int64 version = 0;
  if (!queryInt64(connection, "PRAGMA user_version", version) || version > kInventoryDatabaseSchemaVersion) {
    setSqlError(connection, error, "Unsupported Inventatory SQLite schema version");
    return false;
  }
  bool hasUserTables = false;
  if (!queryHasRows(connection,
                    "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1",
                    hasUserTables)) {
    setSqlError(connection, error, "Unable to inspect the Inventatory SQLite schema");
    return false;
  }
  if (hasUserTables) {
    if (version != kInventoryDatabaseSchemaVersion || !validateCurrentSchema(connection, error)) {
      if (error != nullptr && error->empty()) *error = "Unsupported or invalid Inventatory SQLite schema";
      return false;
    }
    return true;
  }
  if (version != 0) {
    setSqlError(connection, error, "Unsupported Inventatory SQLite schema version");
    return false;
  }
  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    setSqlError(connection, error, "Unable to begin SQLite schema initialization");
    return false;
  }
  const auto fail = [&]() {
    execSql(connection, "ROLLBACK");
    return false;
  };
  if (!createSchemaObjects(connection, error) || !createSchemaIndexes(connection, error) ||
      !execSql(connection, "PRAGMA user_version = 1") || !validateInventoryDatabase(connection, error)) return fail();
  if (!execSql(connection, "COMMIT")) {
    setSqlError(connection, error, "Unable to commit SQLite schema initialization");
    return fail();
  }
  return true;
}

bool validateInventoryDatabase(SqliteConnection& connection, string* error) {
  if (connection.db == nullptr) {
    if (error != nullptr) *error = "SQLite connection is not open";
    return false;
  }
  sqlite3_int64 version = 0;
  if (!queryInt64(connection, "PRAGMA user_version", version) || version != kInventoryDatabaseSchemaVersion) {
    setSqlError(connection, error, "Unsupported Inventatory SQLite schema version");
    return false;
  }
  if (!validateCurrentSchema(connection, error) || !readIntegrityOk(connection, error)) return false;
  return validateInventoryCommitHistory(connection, error);
}

#endif

}  // namespace inventatory
