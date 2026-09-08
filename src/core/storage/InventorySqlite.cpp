// Inventatory - Hardware Inventory Management System
// Internal SQLite helpers shared by inventory persistence files.

#include "core/storage/InventorySqlite.h"
#include "core/history/InventoryVersionInternal.h"

#include <initializer_list>
#include <limits>
#include <sstream>
#include <thread>
#include <chrono>
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

bool createSqliteSnapshot(const filesystem::path& sourcePath, const filesystem::path& destinationPath,
                          string* error) {
  if (error != nullptr) error->clear();
  error_code filesystemError;
  if (!filesystem::is_regular_file(sourcePath, filesystemError) || filesystemError) {
    if (error != nullptr) *error = "SQLite snapshot source does not exist";
    return false;
  }
  if (filesystem::exists(destinationPath, filesystemError) || filesystemError) {
    if (error != nullptr) *error = "SQLite snapshot destination already exists";
    return false;
  }

  SqliteConnection source;
  SqliteConnection destination;
  if (!openDatabaseReadOnly(sourcePath, source)) {
    if (error != nullptr) *error = "Unable to open SQLite snapshot source read-only";
    return false;
  }
  if (!openDatabase(destinationPath, destination)) {
    if (error != nullptr) *error = "Unable to create SQLite snapshot destination";
    return false;
  }

  auto& api = sqliteApi();
  if (api.backup_init == nullptr || api.backup_step == nullptr || api.backup_finish == nullptr) {
    if (error != nullptr) *error = "SQLite online backup is unavailable";
    return false;
  }
  sqlite3_backup* backup = api.backup_init(destination.db, "main", source.db, "main");
  if (backup == nullptr) {
    if (error != nullptr) *error = "Unable to initialize SQLite online backup";
    return false;
  }

  int stepResult = SQLITE_OK;
  int busyRetries = 0;
  do {
    stepResult = api.backup_step(backup, 256);
    if (stepResult == SQLITE_BUSY || stepResult == SQLITE_LOCKED) {
      if (++busyRetries > 300) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } else {
      busyRetries = 0;
    }
  } while (stepResult == SQLITE_OK || stepResult == SQLITE_BUSY || stepResult == SQLITE_LOCKED);

  const int finishResult = api.backup_finish(backup);
  if (stepResult != SQLITE_DONE || finishResult != SQLITE_OK) {
    if (error != nullptr) {
      const char* message = sqliteApi().errmsg(source.db);
      const string detail = message == nullptr || *message == '\0' || string(message) == "not an error"
                                ? string()
                                : string(": ") + message;
      *error = "SQLite online backup failed" + detail;
    }
    return false;
  }

  // Closing the destination after backup_finish commits its rollback journal
  // and makes the resulting file safe to hash and reopen read-only.
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


}  // namespace


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
  if (static_cast<uint64_t>(raw) > static_cast<uint64_t>((numeric_limits<time_t>::max)())) return false;
  value = static_cast<time_t>(raw);
  return true;
}

#endif

}  // namespace inventatory
