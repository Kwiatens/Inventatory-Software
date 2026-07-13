// HIMS - Hardware Inventory Management System
// Internal SQLite helpers shared by inventory persistence files.

#include "core/InventorySqlite.h"

#include <cstdlib>
#include <system_error>

namespace hims {

using namespace std;

#ifdef _WIN32

namespace {

filesystem::path executableDirectory() {
  wchar_t buffer[MAX_PATH];
  const auto length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }

  return filesystem::path(buffer).parent_path();
}

filesystem::path sqliteSearchRoot(const char* envName) {
  if (const char* value = getenv(envName); value != nullptr && *value != '\0') {
    return filesystem::path(value);
  }
  return {};
}

filesystem::path findSqliteDllUnder(const filesystem::path& root, int maxDepth) {
  error_code error;
  if (root.empty()) {
    return {};
  }

  filesystem::recursive_directory_iterator end;
  for (filesystem::recursive_directory_iterator it(root, filesystem::directory_options::skip_permission_denied, error);
       it != end && !error; ++it) {
    if (it.depth() > maxDepth) {
      it.disable_recursion_pending();
      continue;
    }

    if (!it->is_regular_file(error)) {
      continue;
    }
    if (toLower(it->path().filename().string()) == "sqlite3.dll") {
      return it->path();
    }
  }

  return {};
}

filesystem::path findSqliteDllInCommonLocations() {
  const filesystem::path roots[] = {
      sqliteSearchRoot("ProgramFiles") / "Blender Foundation",
      sqliteSearchRoot("LOCALAPPDATA") / "Programs" / "Python",
      sqliteSearchRoot("ProgramFiles") / "Python",
      sqliteSearchRoot("ProgramFiles(x86)") / "Python",
  };

  for (const auto& root : roots) {
    const auto found = findSqliteDllUnder(root, 6);
    if (!found.empty()) {
      return found;
    }
  }

  return {};
}

}  // namespace

bool SqliteApi::load() {
  if (module != nullptr) {
    return true;
  }

  const filesystem::path candidates[] = {
      filesystem::path("sqlite3.dll"),
      filesystem::current_path() / "sqlite3.dll",
      executableDirectory() / "sqlite3.dll",
      findSqliteDllInCommonLocations()};

  for (const auto& candidate : candidates) {
    module = LoadLibraryW(candidate.wstring().c_str());
    if (module != nullptr) {
      break;
    }
  }

  if (module == nullptr) {
    module = LoadLibraryW(L"sqlite3.dll");
  }

  if (module == nullptr) {
    return false;
  }

  initialize = loadSymbol<sqlite3_initialize_fn>(module, "sqlite3_initialize");
  open_v2 = loadSymbol<sqlite3_open_v2_fn>(module, "sqlite3_open_v2");
  close = loadSymbol<sqlite3_close_fn>(module, "sqlite3_close");
  exec = loadSymbol<sqlite3_exec_fn>(module, "sqlite3_exec");
  errmsg = loadSymbol<sqlite3_errmsg_fn>(module, "sqlite3_errmsg");
  prepare_v2 = loadSymbol<sqlite3_prepare_v2_fn>(module, "sqlite3_prepare_v2");
  step = loadSymbol<sqlite3_step_fn>(module, "sqlite3_step");
  finalize = loadSymbol<sqlite3_finalize_fn>(module, "sqlite3_finalize");
  reset = loadSymbol<sqlite3_reset_fn>(module, "sqlite3_reset");
  clear_bindings = loadSymbol<sqlite3_clear_bindings_fn>(module, "sqlite3_clear_bindings");
  bind_text = loadSymbol<sqlite3_bind_text_fn>(module, "sqlite3_bind_text");
  bind_int = loadSymbol<sqlite3_bind_int_fn>(module, "sqlite3_bind_int");
  bind_int64 = loadSymbol<sqlite3_bind_int64_fn>(module, "sqlite3_bind_int64");
  bind_null = loadSymbol<sqlite3_bind_null_fn>(module, "sqlite3_bind_null");
  column_int = loadSymbol<sqlite3_column_int_fn>(module, "sqlite3_column_int");
  column_int64 = loadSymbol<sqlite3_column_int64_fn>(module, "sqlite3_column_int64");
  column_text = loadSymbol<sqlite3_column_text_fn>(module, "sqlite3_column_text");
  busy_timeout = loadSymbol<sqlite3_busy_timeout_fn>(module, "sqlite3_busy_timeout");
  free = loadSymbol<sqlite3_free_fn>(module, "sqlite3_free");

  if (!(initialize && open_v2 && close && exec && errmsg && prepare_v2 && step && finalize && reset &&
        clear_bindings && bind_text && bind_int && bind_int64 && bind_null && column_int && column_int64 &&
        column_text && busy_timeout && free)) {
    return false;
  }

  return initialize() == SQLITE_OK;
}

SqliteApi::~SqliteApi() {
  if (module != nullptr) {
    FreeLibrary(module);
  }
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

const sqlite3_destructor_type SQLITE_TRANSIENT = reinterpret_cast<sqlite3_destructor_type>(-1);

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

}  // namespace hims
