// Inventatory - Hardware Inventory Management System
// Internal SQLite helpers shared by inventory persistence files.

#include "core/InventorySqlite.h"

#include <initializer_list>
#include <limits>
#include <utility>

namespace inventatory {

using namespace std;

#ifdef _WIN32

namespace {

string sqlitePath(const filesystem::path& path) {
  // sqlite3_open_v2 takes UTF-8. path.string() uses the active Windows code
  // page and cannot address otherwise valid Unicode data directories.
  return path.u8string();
}

string quoteIdentifier(const string& identifier) {
  string quoted = "\"";
  for (const char ch : identifier) {
    if (ch == '\"') quoted += "\"\"";
    else quoted += ch;
  }
  quoted += '"';
  return quoted;
}

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

bool addColumnIfMissing(SqliteConnection& connection, const string& table, const string& column,
                        const string& declaration, string* error) {
  if (tableColumnExists(connection, table, column)) return true;
  if (!execSql(connection, "ALTER TABLE " + quoteIdentifier(table) + " ADD COLUMN " + quoteIdentifier(column) + " " +
                            declaration)) {
    setSqlError(connection, error, "Unable to migrate SQLite column: " + table + "." + column);
    return false;
  }
  return true;
}

bool migrateLegacyNames(SqliteConnection& connection, string* error) {
  const auto renameIfNeeded = [&](const string& oldName, const string& newName) {
    if (tableExists(connection, newName) || !tableExists(connection, oldName)) return true;
    if (!execSql(connection, "ALTER TABLE " + quoteIdentifier(oldName) + " RENAME TO " + quoteIdentifier(newName))) {
      setSqlError(connection, error, "Unable to migrate SQLite table " + oldName);
      return false;
    }
    return true;
  };
  if (!renameIfNeeded("hims_items", "inventatory_items") || !renameIfNeeded("hims_racks", "inventatory_racks") ||
      !renameIfNeeded("hims_device_events", "inventatory_device_events")) return false;
  if (tableExists(connection, "inventatory_items") && tableColumnExists(connection, "inventatory_items", "hims_id") &&
      !tableColumnExists(connection, "inventatory_items", "inventatory_id")) {
    if (!execSql(connection, "ALTER TABLE \"inventatory_items\" RENAME COLUMN \"hims_id\" TO \"inventatory_id\"")) {
      setSqlError(connection, error, "Unable to migrate inventatory item identifiers");
      return false;
    }
  }
  return true;
}

}  // namespace

bool SqliteApi::load() {
  return initialize() == SQLITE_OK;
}

SqliteApi& sqliteApi() {
  static SqliteApi api;
  static const bool loaded = api.load();
  (void)loaded;
  return api;
}

SqliteConnection::~SqliteConnection() {
  if (db != nullptr) {
    sqliteApi().close(db);
  }
}

SqliteStatement::~SqliteStatement() {
  if (stmt != nullptr) {
    sqliteApi().finalize(stmt);
  }
}

string sqliteText(sqlite3_stmt* stmt, int column) {
  const auto* text = sqliteApi().column_text(stmt, column);
  return text == nullptr ? string() : reinterpret_cast<const char*>(text);
}

bool openDatabase(const filesystem::path& path, SqliteConnection& connection) {
  const auto& api = sqliteApi();
  if (api.open_v2 == nullptr) {
    return false;
  }

  const auto utf8Path = sqlitePath(path);
  if (api.open_v2(utf8Path.c_str(), &connection.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    connection.db = nullptr;
    return false;
  }

  api.busy_timeout(connection.db, 3000);
  return true;
}

bool openDatabaseReadOnly(const filesystem::path& path, SqliteConnection& connection) {
  const auto& api = sqliteApi();
  if (api.open_v2 == nullptr) return false;
  const auto utf8Path = sqlitePath(path);
  if (api.open_v2(utf8Path.c_str(), &connection.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    connection.db = nullptr;
    return false;
  }
  api.busy_timeout(connection.db, 3000);
  return true;
}

bool execSql(SqliteConnection& connection, const string& sql) {
  char* error = nullptr;
  const auto rc = sqliteApi().exec(connection.db, sql.c_str(), nullptr, nullptr, &error);
  if (error != nullptr) {
    sqliteApi().free(error);
  }
  return rc == SQLITE_OK;
}

bool tableExists(SqliteConnection& connection, const string& tableName) {
  SqliteStatement statement;
  const string sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=? LIMIT 1";
  if (sqliteApi().prepare_v2(connection.db, sql.c_str(), -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqliteApi().bind_text(statement.stmt, 1, tableName.c_str(), -1, SQLITE_TRANSIENT);
  return sqliteApi().step(statement.stmt) == SQLITE_ROW;
}

bool tableColumnExists(SqliteConnection& connection, const string& tableName, const string& columnName) {
  SqliteStatement statement;
  const string sql = "PRAGMA table_info(" + quoteIdentifier(tableName) + ")";
  if (sqliteApi().prepare_v2(connection.db, sql.c_str(), -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    const auto name = sqliteText(statement.stmt, 1);
    if (toLower(name) == toLower(columnName)) {
      return true;
    }
  }
  return false;
}

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

bool migrateOptionalColumns(SqliteConnection& connection, string* error) {
  const pair<const char*, const char*> itemColumns[] = {
      {"label_override", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_provider", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_product_number", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_manufacturer_part_number", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_category_id", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_category_path", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_title", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_detailed_description", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_parameters", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_product_url", "TEXT NOT NULL DEFAULT ''"},
      {"vendor_locale", "TEXT NOT NULL DEFAULT ''"},
      {"inventatory_id", "TEXT NOT NULL DEFAULT ''"},
      {"created_at", "INTEGER NOT NULL DEFAULT 0"},
      {"machine_code", "TEXT NOT NULL DEFAULT ''"},
      {"rack_id", "TEXT NOT NULL DEFAULT ''"},
      {"rack_slot", "TEXT NOT NULL DEFAULT ''"},
      {"rack_assignment", "TEXT NOT NULL DEFAULT 'automatic'"},
  };
  for (const auto& column : itemColumns) {
    if (!addColumnIfMissing(connection, "inventatory_items", column.first, column.second, error)) return false;
  }
  const pair<const char*, const char*> rackColumns[] = {
      {"rows_count", "INTEGER NOT NULL DEFAULT 5"},
      {"columns_count", "INTEGER NOT NULL DEFAULT 5"},
      {"created_at", "INTEGER NOT NULL DEFAULT 0"},
  };
  for (const auto& column : rackColumns) {
    if (!addColumnIfMissing(connection, "inventatory_racks", column.first, column.second, error)) return false;
  }
  if (!addColumnIfMissing(connection, "inventatory_stock_movements", "reference", "TEXT NOT NULL DEFAULT ''", error)) {
    return false;
  }
  const pair<const char*, const char*> deviceColumns[] = {
      {"result_id", "TEXT NOT NULL DEFAULT ''"}, {"result_status", "TEXT NOT NULL DEFAULT ''"},
      {"result_existing", "INTEGER NOT NULL DEFAULT 0"}, {"result_item_name", "TEXT NOT NULL DEFAULT ''"},
      {"result_requested_delta", "INTEGER NOT NULL DEFAULT 0"}, {"result_applied_delta", "INTEGER NOT NULL DEFAULT 0"},
      {"result_quantity", "INTEGER NOT NULL DEFAULT 0"}, {"result_location", "TEXT NOT NULL DEFAULT ''"},
      {"result_code", "TEXT NOT NULL DEFAULT ''"}, {"result_message", "TEXT NOT NULL DEFAULT ''"},
      {"result_acknowledged", "INTEGER NOT NULL DEFAULT 0"}, {"received_at", "INTEGER NOT NULL DEFAULT 0"},
      {"completed_at", "INTEGER NOT NULL DEFAULT 0"},
  };
  for (const auto& column : deviceColumns) {
    if (!addColumnIfMissing(connection, "inventatory_device_events", column.first, column.second, error)) return false;
  }
  const pair<const char*, const char*> commitColumns[] = {
      {"reference", "TEXT NOT NULL DEFAULT ''"}, {"checkpoint", "INTEGER NOT NULL DEFAULT 0"},
      {"corrective", "INTEGER NOT NULL DEFAULT 0"}, {"reverted_commit_id", "TEXT NOT NULL DEFAULT ''"},
      {"changed_item_count", "INTEGER NOT NULL DEFAULT 0"}, {"changed_rack_count", "INTEGER NOT NULL DEFAULT 0"},
      {"snapshot_version", "INTEGER NOT NULL DEFAULT 1"},
  };
  for (const auto& column : commitColumns) {
    if (!addColumnIfMissing(connection, "inventatory_inventory_commits", column.first, column.second, error)) return false;
  }
  const pair<const char*, const char*> projectColumns[] = {
      {"boards", "INTEGER NOT NULL DEFAULT 1"}, {"created_at", "INTEGER NOT NULL DEFAULT 0"},
      {"last_opened", "INTEGER NOT NULL DEFAULT 0"}, {"last_built", "INTEGER NOT NULL DEFAULT 0"},
      {"overrides", "TEXT NOT NULL DEFAULT ''"}, {"enrichment", "TEXT NOT NULL DEFAULT ''"},
  };
  for (const auto& column : projectColumns) {
    if (!addColumnIfMissing(connection, "inventatory_bom_projects", column.first, column.second, error)) return false;
  }
  return true;
}

bool migrateLegacyColumns(SqliteConnection& connection, string* error) {
  // Releases before the vendor-neutral catalogue used
  // manufacturer_part_number/enrichment_status instead of the current
  // DigiKey fields. Preserve the old part number as the SKU while exposing
  // the current columns with safe defaults.
  if (!tableExists(connection, "inventatory_items") ||
      !tableColumnExists(connection, "inventatory_items", "manufacturer_part_number")) return true;
  if (!addColumnIfMissing(connection, "inventatory_items", "digikey_part_number", "TEXT NOT NULL DEFAULT ''", error) ||
      !addColumnIfMissing(connection, "inventatory_items", "product_url", "TEXT NOT NULL DEFAULT ''", error) ||
      !addColumnIfMissing(connection, "inventatory_items", "sync_status", "TEXT NOT NULL DEFAULT 'synced'", error) ||
      !addColumnIfMissing(connection, "inventatory_items", "sku", "TEXT NOT NULL DEFAULT ''", error)) return false;
  return execSql(connection,
                 "UPDATE inventatory_items SET sku=manufacturer_part_number WHERE trim(sku)='' AND "
                 "trim(manufacturer_part_number)<>''");
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
  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    setSqlError(connection, error, "Unable to begin SQLite schema migration");
    return false;
  }
  const auto fail = [&]() {
    execSql(connection, "ROLLBACK");
    return false;
  };
  if (!migrateLegacyNames(connection, error) || !createSchemaObjects(connection, error) ||
      !migrateLegacyColumns(connection, error) ||
      !validateExistingColumns(connection, error) || !migrateOptionalColumns(connection, error) ||
      !createSchemaIndexes(connection, error) || !validateDataValues(connection, error)) return fail();
  if (!execSql(connection, "PRAGMA user_version = 1") || !execSql(connection, "COMMIT")) {
    setSqlError(connection, error, "Unable to commit SQLite schema migration");
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
  if (!queryInt64(connection, "PRAGMA user_version", version) || version > kInventoryDatabaseSchemaVersion) {
    setSqlError(connection, error, "Unsupported Inventatory SQLite schema version");
    return false;
  }
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
  return readIntegrityOk(connection, error) && validateDataValues(connection, error);
}

bool sqliteInt32(sqlite3_stmt* statement, int column, int& value) {
  if (sqliteApi().column_type(statement, column) != SQLITE_INTEGER) return false;
  const sqlite3_int64 raw = sqliteApi().column_int64(statement, column);
  if (raw < numeric_limits<int>::min() || raw > numeric_limits<int>::max()) return false;
  value = static_cast<int>(raw);
  return true;
}

bool sqliteUInt64(sqlite3_stmt* statement, int column, uint64_t& value) {
  if (sqliteApi().column_type(statement, column) != SQLITE_INTEGER) return false;
  const sqlite3_int64 raw = sqliteApi().column_int64(statement, column);
  if (raw < 0) return false;
  value = static_cast<uint64_t>(raw);
  return true;
}

bool sqliteSize(sqlite3_stmt* statement, int column, size_t& value) {
  if (sqliteApi().column_type(statement, column) != SQLITE_INTEGER) return false;
  const sqlite3_int64 raw = sqliteApi().column_int64(statement, column);
  if (raw < 0 || static_cast<unsigned long long>(raw) > numeric_limits<size_t>::max()) return false;
  value = static_cast<size_t>(raw);
  return true;
}

bool sqliteTime(sqlite3_stmt* statement, int column, time_t& value) {
  if (sqliteApi().column_type(statement, column) != SQLITE_INTEGER) return false;
  const sqlite3_int64 raw = sqliteApi().column_int64(statement, column);
  // Inventatory timestamps are Unix times.  Negative values are outside the
  // persisted domain even on platforms whose time_t can represent them.
  if (raw < 0) return false;
  if (numeric_limits<time_t>::is_signed) {
    if (raw > static_cast<sqlite3_int64>(numeric_limits<time_t>::max())) return false;
  } else if (static_cast<unsigned long long>(raw) > numeric_limits<time_t>::max()) {
    return false;
  }
  value = static_cast<time_t>(raw);
  return true;
}

#endif

}  // namespace inventatory
