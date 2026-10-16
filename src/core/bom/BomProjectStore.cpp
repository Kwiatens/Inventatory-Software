// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Durable storage for pinned BOM projects.

#include "core/bom/BomProjectStore.h"

#include "core/inventory/VendorProvider.h"
#include "core/storage/InventorySqlite.h"
#include "core/transfer/CsvExport.h"

#include <algorithm>

namespace inventatory {

using namespace std;

optional<string> bomEnrichmentEntry(BomLookupOutcome outcome, const string& suggestion, bool verified, bool fromTme,
                                    bool fromMouser) {
  switch (outcome) {
    case BomLookupOutcome::Found: {
      const auto trimmed = trim(suggestion);
      // A part without a usable number gives the user nothing to act on.
      if (trimmed.empty() || trimmed == "-") return string(kBomEnrichmentNoMatch);
      // A DigiKey part is stored bare, as it was before the other suppliers were asked.
      const string stored = fromTme      ? string(kBomEnrichmentTme) + trimmed
                            : fromMouser ? string(kBomEnrichmentMouser) + trimmed
                                         : trimmed;
      return verified ? string(kBomEnrichmentVerified) + stored : stored;
    }
    case BomLookupOutcome::NoMatch:
      return string(kBomEnrichmentNoMatch);
    case BomLookupOutcome::Failed:
      break;
  }
  return nullopt;
}

bool bomEnrichmentCached(const map<string, string>& enrichment, const string& key) {
  const auto found = enrichment.find(key);
  if (found == enrichment.end()) return false;
  const auto value = trim(found->second);
  return !value.empty() && value != "-";
}

namespace {

// The stored suggestion without its verification prefix.
string withoutVerification(const string& stored) {
  const string prefix = kBomEnrichmentVerified;
  return stored.rfind(prefix, 0) == 0 ? stored.substr(prefix.size()) : stored;
}

// A supplier prefix of a stored suggestion and the vendor it names. A suggestion without one came from DigiKey.
struct SupplierPrefix {
  const char* prefix;
  const char* provider;
};
constexpr SupplierPrefix kSupplierPrefixes[] = {
    {kBomEnrichmentTme, kVendorProviderTme},
    {kBomEnrichmentMouser, kVendorProviderMouser},
};

// The supplier prefix a suggestion starts with, once its verification prefix is removed; nullptr for none.
const SupplierPrefix* supplierPrefixOf(const string& value) {
  for (const auto& entry : kSupplierPrefixes) {
    if (value.rfind(entry.prefix, 0) == 0) return &entry;
  }
  return nullptr;
}

}  // namespace

string bomEnrichmentPart(const string& stored) {
  auto value = withoutVerification(stored);
  if (const auto* supplier = supplierPrefixOf(value)) value = value.substr(string(supplier->prefix).size());
  const auto trimmed = trim(value);
  return trimmed == "-" || trimmed == kBomEnrichmentNoMatch ? string() : trimmed;
}

string bomEnrichmentSupplier(const string& stored) {
  if (bomEnrichmentPart(stored).empty()) return {};
  const auto* supplier = supplierPrefixOf(withoutVerification(stored));
  return vendorDisplayName(supplier == nullptr ? kVendorProviderDigiKey : supplier->provider);
}

bool bomEnrichmentVerified(const string& stored) {
  return stored.rfind(string(kBomEnrichmentVerified), 0) == 0;
}

string bomEnrichmentExportText(const string& stored) {
  return bomEnrichmentPart(stored);
}

BomShortageExport buildBomShortageCsv(const BomAnalysis& analysis, const map<string, string>& enrichment) {
  BomShortageExport result;
  result.text = "Designation,Footprint,Package,Designators,Needed,On hand,Suggested part,Supplier,"
                "Suggestion check\r\n";
  for (const auto& match : analysis.matches) {
    if (match.sufficient || match.lineIndex >= analysis.lines.size()) {
      continue;
    }
    const auto& line = analysis.lines[match.lineIndex];
    const auto suggestion = enrichment.find(bomLineKey(line));
    const auto part = suggestion == enrichment.end() ? string() : bomEnrichmentPart(suggestion->second);
    const auto supplier = suggestion == enrichment.end() ? string() : bomEnrichmentSupplier(suggestion->second);
    // A suggestion that did not fit the line is never presented as a verified part.
    const string check = part.empty() ? string()
                         : bomEnrichmentVerified(suggestion->second) ? string("verified")
                                                                     : string("check before ordering");
    // A line waiting for confirmation is on the list, and counts nothing on hand: the stock it names is not yet
    // the part the user has accepted for this line.
    const int onHand = match.confirmed ? match.available : 0;
    result.text += csvTextCell(line.designation) + ',' + csvTextCell(line.footprint) + ',' +
                   csvTextCell(packageFromFootprint(line.footprint)) + ',' + csvTextCell(join(line.designators, ' ')) +
                   ',' + to_string(match.needed) + ',' + to_string(onHand) + ',' + csvTextCell(part) + ',' +
                   csvTextCell(supplier) + ',' + csvTextCell(check) + "\r\n";
    ++result.rows;
  }
  return result;
}

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

string serializeBomMap(const map<string, string>& values) {
  // Reuses the parameter encoding so escaping rules stay in one place.
  vector<Parameter> parameters;
  parameters.reserve(values.size());
  for (const auto& entry : values) {
    parameters.push_back({entry.first, entry.second});
  }
  return serializeParametersForStorage(parameters);
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

bool loadBomProjects(const filesystem::path& databasePath, vector<BomProject>& projects) {
  vector<BomProject> loadedProjects;
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection)) {
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
}

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

bool saveBomProjects(const filesystem::path& databasePath, const vector<BomProject>& projects) {
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection) ||
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
}

}  // namespace inventatory
