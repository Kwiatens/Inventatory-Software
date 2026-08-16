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
  decltype(&::sqlite3_column_text) column_text = &::sqlite3_column_text;
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
bool execSql(SqliteConnection& connection, const string& sql);
bool tableExists(SqliteConnection& connection, const string& tableName);
bool tableColumnExists(SqliteConnection& connection, const string& tableName, const string& columnName);

#endif

}  // namespace inventatory
