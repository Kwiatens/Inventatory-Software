// Inventatory - Hardware Inventory Management System
// Durable storage for pinned BOM projects.

#include "core/BomProjectStore.h"

#include "core/InventorySqlite.h"

#include <algorithm>

namespace inventatory {

using namespace std;

namespace {

bool parseLegacyBomMap(const string& value, map<string, string>& values) {
  size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(';', start);
    const auto entry = value.substr(start, end == string::npos ? string::npos : end - start);
    const auto equals = entry.find('=');
    if (equals == string::npos ||
        (entry.find(':') != string::npos && entry.find(':') < equals)) return false;
    const auto key = trim(entry.substr(0, equals));
    if (key.empty() || !values.emplace(key, trim(entry.substr(equals + 1))).second) return false;
    if (end == string::npos) break;
    start = end + 1;
  }
  return true;
}

bool deserializeBomMapChecked(const string& value, map<string, string>& values) {
  if (value.empty()) {
    values.clear();
    return true;
  }
  const bool structured = value.rfind("v1:", 0) == 0 || value.rfind("v2:", 0) == 0;
  if (structured) {
    vector<Parameter> parameters;
    if (!deserializeParametersFromStorageStrict(value, parameters)) return false;
    map<string, string> parsed;
    for (const auto& parameter : parameters) {
      if (parameter.name.empty() || !parsed.emplace(parameter.name, parameter.value).second) return false;
    }
    values = move(parsed);
    return true;
  }
  map<string, string> parsed;
  if (!parseLegacyBomMap(value, parsed)) return false;
  values = move(parsed);
  return true;
}

}  // namespace

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
  if (!deserializeBomMapChecked(value, values)) values.clear();
  return values;
}

#ifdef _WIN32
namespace {

bool ensureBomProjectSchema(SqliteConnection& connection) {
  return ensureInventoryDatabaseSchema(connection);
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
  vector<BomProject> loadedProjects;
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

  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    BomProject project;
    project.id = sqliteText(statement.stmt, 0);
    project.name = sqliteText(statement.stmt, 1);
    project.sourcePath = sqliteText(statement.stmt, 2);
    if (!sqliteInt32(statement.stmt, 3, project.boards) || project.boards <= 0 ||
        !sqliteTime(statement.stmt, 4, project.createdAt) || !sqliteTime(statement.stmt, 5, project.lastOpened) ||
        !sqliteTime(statement.stmt, 6, project.lastBuilt)) return false;
    project.bomText = sqliteText(statement.stmt, 7);
    if (!deserializeBomMapChecked(sqliteText(statement.stmt, 8), project.overrides) ||
        !deserializeBomMapChecked(sqliteText(statement.stmt, 9), project.enrichment)) return false;
    loadedProjects.push_back(move(project));
  }
  if (stepResult != SQLITE_DONE) return false;
  projects = move(loadedProjects);
  return true;
#else
  (void)databasePath;
  return false;
#endif
}

#ifdef _WIN32
bool validateBomProjects(SqliteConnection& connection, string* error) {
  if (connection.db == nullptr) {
    if (error != nullptr) *error = "SQLite connection is not open";
    return false;
  }
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, name, source_path, boards, created_at, last_opened, last_built, bom_text, overrides, enrichment
    FROM inventatory_bom_projects
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    if (error != nullptr) *error = "Unable to read BOM projects for validation";
    return false;
  }

  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    int boards = 0;
    time_t createdAt = 0;
    time_t lastOpened = 0;
    time_t lastBuilt = 0;
    map<string, string> ignored;
    if (sqliteText(statement.stmt, 0).empty() || sqliteText(statement.stmt, 1).empty() ||
        !sqliteInt32(statement.stmt, 3, boards) || boards <= 0 ||
        !sqliteTime(statement.stmt, 4, createdAt) || !sqliteTime(statement.stmt, 5, lastOpened) ||
        !sqliteTime(statement.stmt, 6, lastBuilt) ||
        !deserializeBomMapChecked(sqliteText(statement.stmt, 8), ignored) ||
        !deserializeBomMapChecked(sqliteText(statement.stmt, 9), ignored)) {
      if (error != nullptr) *error = "BOM project data is malformed";
      return false;
    }
  }
  if (stepResult != SQLITE_DONE) {
    if (error != nullptr) *error = "Unable to finish reading BOM projects for validation";
    return false;
  }
  return true;
}
#endif

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
