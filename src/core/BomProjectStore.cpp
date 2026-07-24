// Inventatory - Hardware Inventory Management System
// Durable storage for pinned BOM projects.

#include "core/BomProjectStore.h"

#include "core/InventorySqlite.h"

#include <algorithm>

namespace inventatory {

using namespace std;

string serializeBomMap(const map<string, string>& values) {
  // Reuses the parameter encoding so escaping rules stay in one place.
  vector<Parameter> parameters;
  parameters.reserve(values.size());
  for (const auto& entry : values) {
    parameters.push_back({entry.first, entry.second});
  }
  return serializeParametersForStorage(parameters);
}

map<string, string> deserializeBomMap(const string& value) {
  map<string, string> values;
  for (const auto& parameter : deserializeParametersFromStorage(value)) {
    values[parameter.name] = parameter.value;
  }
  return values;
}

#ifdef _WIN32
namespace {

bool ensureBomProjectSchema(SqliteConnection& connection) {
  return execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS inventatory_bom_projects (
      id TEXT PRIMARY KEY,
      name TEXT NOT NULL,
      source_path TEXT NOT NULL,
      boards INTEGER NOT NULL DEFAULT 1,
      created_at INTEGER NOT NULL DEFAULT 0,
      last_opened INTEGER NOT NULL DEFAULT 0,
      last_built INTEGER NOT NULL DEFAULT 0,
      bom_text TEXT NOT NULL,
      overrides TEXT NOT NULL DEFAULT '',
      enrichment TEXT NOT NULL DEFAULT ''
    )
  )SQL");
}

bool insertProject(SqliteConnection& connection, const BomProject& project) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT INTO inventatory_bom_projects
      (id, name, source_path, boards, created_at, last_opened, last_built, bom_text, overrides, enrichment)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  const auto overrides = serializeBomMap(project.overrides);
  const auto enrichment = serializeBomMap(project.enrichment);
  sqliteApi().bind_text(statement.stmt, 1, project.id.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, project.name.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 3, project.sourcePath.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int(statement.stmt, 4, max(1, project.boards));
  sqliteApi().bind_int64(statement.stmt, 5, static_cast<sqlite3_int64>(project.createdAt));
  sqliteApi().bind_int64(statement.stmt, 6, static_cast<sqlite3_int64>(project.lastOpened));
  sqliteApi().bind_int64(statement.stmt, 7, static_cast<sqlite3_int64>(project.lastBuilt));
  sqliteApi().bind_text(statement.stmt, 8, project.bomText.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 9, overrides.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 10, enrichment.c_str(), -1, SQLITE_TRANSIENT);
  return sqliteApi().step(statement.stmt) == SQLITE_DONE;
}

}  // namespace
#endif

bool loadBomProjects(const filesystem::path& databasePath, vector<BomProject>& projects) {
  projects.clear();
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureBomProjectSchema(connection)) {
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, name, source_path, boards, created_at, last_opened, last_built, bom_text, overrides, enrichment
    FROM inventatory_bom_projects ORDER BY last_opened DESC, created_at DESC
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    BomProject project;
    project.id = sqliteText(statement.stmt, 0);
    project.name = sqliteText(statement.stmt, 1);
    project.sourcePath = sqliteText(statement.stmt, 2);
    project.boards = max(1, sqliteApi().column_int(statement.stmt, 3));
    project.createdAt = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 4));
    project.lastOpened = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 5));
    project.lastBuilt = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 6));
    project.bomText = sqliteText(statement.stmt, 7);
    project.overrides = deserializeBomMap(sqliteText(statement.stmt, 8));
    project.enrichment = deserializeBomMap(sqliteText(statement.stmt, 9));
    projects.push_back(move(project));
  }
  return true;
#else
  (void)databasePath;
  return false;
#endif
}

bool saveBomProjects(const filesystem::path& databasePath, const vector<BomProject>& projects) {
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureBomProjectSchema(connection) ||
      !execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    return false;
  }

  // The table is small and rewritten wholesale, matching how items and racks
  // are persisted elsewhere.
  bool ok = execSql(connection, "DELETE FROM inventatory_bom_projects");
  for (const auto& project : projects) {
    if (!ok) {
      break;
    }
    ok = insertProject(connection, project);
  }

  if (!ok || !execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  return true;
#else
  (void)databasePath;
  (void)projects;
  return false;
#endif
}

}  // namespace inventatory
