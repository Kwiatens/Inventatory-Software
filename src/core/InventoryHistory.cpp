// HIMS - Hardware Inventory Management System
// Inventory history persistence helpers.

#include "core/InventorySqlite.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace hims {

using namespace std;

namespace {

filesystem::path historyFilePath(const filesystem::path& path) {
  auto filename = path.filename();
  filename.replace_extension(".history.tsv");
  return path.parent_path() / filename;
}

string serializeHistoryPoint(const InventoryHistoryPoint& point) {
  ostringstream out;
  out << point.timestamp << '\t' << point.itemCount << '\t' << point.totalUnits << '\t' << point.lowStockCount << '\t'
      << point.outOfStockCount << '\t' << point.dataErrorCount;
  return out.str();
}

bool deserializeHistoryPoint(const string& line, InventoryHistoryPoint& point) {
  istringstream input(line);
  if (!(input >> point.timestamp >> point.itemCount >> point.totalUnits >> point.lowStockCount >>
        point.outOfStockCount >> point.dataErrorCount)) {
    return false;
  }
  return true;
}

#ifdef _WIN32

bool createHistoryTable(SqliteConnection& connection) {
  return execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS hims_inventory_history (
      timestamp INTEGER NOT NULL,
      item_count INTEGER NOT NULL,
      total_units INTEGER NOT NULL,
      low_stock_count INTEGER NOT NULL,
      out_of_stock_count INTEGER NOT NULL,
      data_error_count INTEGER NOT NULL
    )
  )SQL");
}

bool loadHistoryFromHimsTable(SqliteConnection& connection, vector<InventoryHistoryPoint>& history) {
  if (!tableExists(connection, "hims_inventory_history")) {
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT timestamp, item_count, total_units, low_stock_count, out_of_stock_count, data_error_count
    FROM hims_inventory_history
    ORDER BY timestamp ASC
  )SQL";

  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    InventoryHistoryPoint point;
    point.timestamp = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 0));
    point.itemCount = static_cast<size_t>(sqliteApi().column_int64(statement.stmt, 1));
    point.totalUnits = static_cast<size_t>(sqliteApi().column_int64(statement.stmt, 2));
    point.lowStockCount = static_cast<size_t>(sqliteApi().column_int64(statement.stmt, 3));
    point.outOfStockCount = static_cast<size_t>(sqliteApi().column_int64(statement.stmt, 4));
    point.dataErrorCount = static_cast<size_t>(sqliteApi().column_int64(statement.stmt, 5));
    history.push_back(move(point));
  }

  return true;
}

bool writeHistoryToHimsTable(SqliteConnection& connection, const vector<InventoryHistoryPoint>& history) {
  if (!createHistoryTable(connection)) {
    return false;
  }

  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    return false;
  }
  if (!execSql(connection, "DELETE FROM hims_inventory_history")) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT INTO hims_inventory_history (
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

#endif

}  // namespace

bool loadInventoryHistory(const filesystem::path& path, vector<InventoryHistoryPoint>& history) {
  history.clear();
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  return loadHistoryFromHimsTable(connection, history);
#else
  ifstream file(historyFilePath(path));
  if (!file) {
    return false;
  }

  string line;
  while (getline(file, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    InventoryHistoryPoint point;
    if (deserializeHistoryPoint(line, point)) {
      history.push_back(move(point));
    }
  }

  return !history.empty();
#endif
}

bool saveInventoryHistory(const filesystem::path& path, const vector<InventoryHistoryPoint>& history) {
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  return writeHistoryToHimsTable(connection, history);
#else
  filesystem::create_directories(path.parent_path());

  ofstream file(historyFilePath(path), ios::trunc);
  if (!file) {
    return false;
  }

  file << "# HIMS inventory history\n";
  for (const auto& point : history) {
    file << serializeHistoryPoint(point) << '\n';
  }
  return true;
#endif
}

}  // namespace hims
