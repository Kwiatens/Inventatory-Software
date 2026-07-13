// HIMS - Hardware Inventory Management System
// Internal SQLite helpers shared by inventory persistence files.

#pragma once

#include "core/Inventory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace hims {

#ifdef _WIN32
using sqlite3 = struct sqlite3;
using sqlite3_stmt = struct sqlite3_stmt;
using sqlite3_int64 = long long;
using sqlite3_destructor_type = void (*)(void*);

extern const sqlite3_destructor_type SQLITE_TRANSIENT;
constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;

using sqlite3_initialize_fn = int (*)();
using sqlite3_open_v2_fn = int (*)(const char*, sqlite3**, int, const char*);
using sqlite3_close_fn = int (*)(sqlite3*);
using sqlite3_exec_fn = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
using sqlite3_errmsg_fn = const char* (*)(sqlite3*);
using sqlite3_prepare_v2_fn = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using sqlite3_step_fn = int (*)(sqlite3_stmt*);
using sqlite3_finalize_fn = int (*)(sqlite3_stmt*);
using sqlite3_reset_fn = int (*)(sqlite3_stmt*);
using sqlite3_clear_bindings_fn = int (*)(sqlite3_stmt*);
using sqlite3_bind_text_fn = int (*)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type);
using sqlite3_bind_int_fn = int (*)(sqlite3_stmt*, int, int);
using sqlite3_bind_int64_fn = int (*)(sqlite3_stmt*, int, sqlite3_int64);
using sqlite3_bind_null_fn = int (*)(sqlite3_stmt*, int);
using sqlite3_column_int_fn = int (*)(sqlite3_stmt*, int);
using sqlite3_column_int64_fn = sqlite3_int64 (*)(sqlite3_stmt*, int);
using sqlite3_column_text_fn = const unsigned char* (*)(sqlite3_stmt*, int);
using sqlite3_busy_timeout_fn = int (*)(sqlite3*, int);
using sqlite3_free_fn = void (*)(void*);

struct SqliteApi {
  HMODULE module = nullptr;
  sqlite3_initialize_fn initialize = nullptr;
  sqlite3_open_v2_fn open_v2 = nullptr;
  sqlite3_close_fn close = nullptr;
  sqlite3_exec_fn exec = nullptr;
  sqlite3_errmsg_fn errmsg = nullptr;
  sqlite3_prepare_v2_fn prepare_v2 = nullptr;
  sqlite3_step_fn step = nullptr;
  sqlite3_finalize_fn finalize = nullptr;
  sqlite3_reset_fn reset = nullptr;
  sqlite3_clear_bindings_fn clear_bindings = nullptr;
  sqlite3_bind_text_fn bind_text = nullptr;
  sqlite3_bind_int_fn bind_int = nullptr;
  sqlite3_bind_int64_fn bind_int64 = nullptr;
  sqlite3_bind_null_fn bind_null = nullptr;
  sqlite3_column_int_fn column_int = nullptr;
  sqlite3_column_int64_fn column_int64 = nullptr;
  sqlite3_column_text_fn column_text = nullptr;
  sqlite3_busy_timeout_fn busy_timeout = nullptr;
  sqlite3_free_fn free = nullptr;

  template <typename T>
  static T loadSymbol(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
  }

  bool load();

  ~SqliteApi();
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

}  // namespace hims
