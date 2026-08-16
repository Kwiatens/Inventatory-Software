// Inventatory - Hardware Inventory Management System
// Internal SQLite helpers shared by inventory persistence files.

#include "core/InventorySqlite.h"

namespace inventatory {

using namespace std;

#ifdef _WIN32

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

  if (api.open_v2(path.string().c_str(), &connection.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
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
  const string sql = "PRAGMA table_info(" + tableName + ")";
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

#endif

}  // namespace inventatory
