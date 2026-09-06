// Inventatory - Hardware Inventory Management System
// Internal SQLite helpers shared by inventory persistence files.

#pragma once

#include "core/Inventory.h"
#include <sqlite3.h>

namespace inventatory {

#ifdef _WIN32
struct SqliteApi {
  decltype(&::sqlite3_initialize) initialize = &::sqlite3_initialize;
  decltype(&::sqlite3_open_v2) open_v2 = &::sqlite3_open_v2;
  decltype(&::sqlite3_close) close = &::sqlite3_close;
  decltype(&::sqlite3_exec) exec = &::sqlite3_exec;
  decltype(&::sqlite3_errmsg) errmsg = &::sqlite3_errmsg;
  decltype(&::sqlite3_prepare_v2) prepare_v2 = &::sqlite3_prepare_v2;
  decltype(&::sqlite3_step) step = &::sqlite3_step;
  decltype(&::sqlite3_finalize) finalize = &::sqlite3_finalize;
  decltype(&::sqlite3_reset) reset = &::sqlite3_reset;
  decltype(&::sqlite3_clear_bindings) clear_bindings = &::sqlite3_clear_bindings;
  decltype(&::sqlite3_bind_text) bind_text = &::sqlite3_bind_text;
  decltype(&::sqlite3_bind_int) bind_int = &::sqlite3_bind_int;
  decltype(&::sqlite3_bind_int64) bind_int64 = &::sqlite3_bind_int64;
  decltype(&::sqlite3_bind_null) bind_null = &::sqlite3_bind_null;
  decltype(&::sqlite3_column_int) column_int = &::sqlite3_column_int;
  decltype(&::sqlite3_column_int64) column_int64 = &::sqlite3_column_int64;
  decltype(&::sqlite3_column_type) column_type = &::sqlite3_column_type;
  decltype(&::sqlite3_column_text) column_text = &::sqlite3_column_text;
  decltype(&::sqlite3_changes) changes = &::sqlite3_changes;
  decltype(&::sqlite3_busy_timeout) busy_timeout = &::sqlite3_busy_timeout;
  decltype(&::sqlite3_free) free = &::sqlite3_free;

  bool load();
};

SqliteApi& sqliteApi();

struct SqliteConnection {
  sqlite3* db = nullptr;
  ~SqliteConnection();
};

struct SqliteStatement {
  sqlite3_stmt* stmt = nullptr;
  ~SqliteStatement();
};

string sqliteText(sqlite3_stmt* stmt, int column);
bool openDatabase(const filesystem::path& path, SqliteConnection& connection);
bool openDatabaseReadOnly(const filesystem::path& path, SqliteConnection& connection);
bool execSql(SqliteConnection& connection, const string& sql);
bool tableExists(SqliteConnection& connection, const string& tableName);
bool tableColumnExists(SqliteConnection& connection, const string& tableName, const string& columnName);

// All SQLite persistence modules share this versioned schema.  The migration
// is transactional and must be run only on a read-write connection.  Backup
// validation uses validateInventoryDatabase() instead; it never creates or
// alters objects.
constexpr int kInventoryDatabaseSchemaVersion = 1;
bool ensureInventoryDatabaseSchema(SqliteConnection& connection, string* error = nullptr);
bool validateInventoryDatabase(SqliteConnection& connection, string* error = nullptr);

// sqlite3_column_int()/column_int64() silently coerce and truncate values.
// Persistence loaders use these checked conversions so malformed databases
// cannot wrap into application-sized integers.
bool sqliteInt32(sqlite3_stmt* statement, int column, int& value);
bool sqliteUInt64(sqlite3_stmt* statement, int column, std::uint64_t& value);
bool sqliteSize(sqlite3_stmt* statement, int column, size_t& value);
bool sqliteTime(sqlite3_stmt* statement, int column, time_t& value);

#endif

}  // namespace inventatory
