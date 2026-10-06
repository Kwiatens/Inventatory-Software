// Inventatory - Hardware Inventory Management System
// Inventory history persistence helpers.

#include "core/storage/InventorySqlite.h"

#include <utility>

namespace inventatory {

using namespace std;

namespace {

bool loadHistoryFromInventatoryTable(SqliteConnection& connection, vector<InventoryHistoryPoint>& history) {
  if (!tableExists(connection, "inventatory_inventory_history")) {
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT timestamp, item_count, total_units, low_stock_count, out_of_stock_count, data_error_count
    FROM inventatory_inventory_history
    ORDER BY timestamp ASC
  )SQL";

  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    InventoryHistoryPoint point;
    if (!sqliteTime(statement.stmt, 0, point.timestamp) || !sqliteSize(statement.stmt, 1, point.itemCount) ||
        !sqliteSize(statement.stmt, 2, point.totalUnits) || !sqliteSize(statement.stmt, 3, point.lowStockCount) ||
        !sqliteSize(statement.stmt, 4, point.outOfStockCount) || !sqliteSize(statement.stmt, 5, point.dataErrorCount)) {
      return false;
    }
    history.push_back(move(point));
  }

  return stepResult == SQLITE_DONE;
}

bool writeHistoryToInventatoryTable(SqliteConnection& connection, const vector<InventoryHistoryPoint>& history) {
  if (!ensureInventoryDatabaseSchema(connection)) {
    return false;
  }

  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    return false;
  }
  if (!execSql(connection, "DELETE FROM inventatory_inventory_history")) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT INTO inventatory_inventory_history (
      timestamp, item_count, total_units, low_stock_count, out_of_stock_count, data_error_count
    ) VALUES (?, ?, ?, ?, ?, ?)
  )SQL";

  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  for (const auto& point : history) {
    sqliteApi().bind_int64(statement.stmt, 1, static_cast<sqlite3_int64>(point.timestamp));
    sqliteApi().bind_int64(statement.stmt, 2, static_cast<sqlite3_int64>(point.itemCount));
    sqliteApi().bind_int64(statement.stmt, 3, static_cast<sqlite3_int64>(point.totalUnits));
    sqliteApi().bind_int64(statement.stmt, 4, static_cast<sqlite3_int64>(point.lowStockCount));
    sqliteApi().bind_int64(statement.stmt, 5, static_cast<sqlite3_int64>(point.outOfStockCount));
    sqliteApi().bind_int64(statement.stmt, 6, static_cast<sqlite3_int64>(point.dataErrorCount));

    if (sqliteApi().step(statement.stmt) != SQLITE_DONE) {
      execSql(connection, "ROLLBACK");
      return false;
    }

    sqliteApi().reset(statement.stmt);
    sqliteApi().clear_bindings(statement.stmt);
  }

  if (!execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  return true;
}

}  // namespace

bool loadInventoryHistory(const filesystem::path& path, vector<InventoryHistoryPoint>& history) {
  history.clear();
  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  return loadHistoryFromInventatoryTable(connection, history);
}

bool saveInventoryHistory(const filesystem::path& path, const vector<InventoryHistoryPoint>& history) {
  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  return writeHistoryToInventatoryTable(connection, history);
}

}  // namespace inventatory
