// Inventatory - Hardware Inventory Management System
// Read-only deterministic lookup for Inventatory Electronics Components Database snapshots.

#include "core/IecdDatabase.h"

#include "core/InventorySqlite.h"

#include <algorithm>
#include <cctype>

namespace inventatory {

using namespace std;

namespace {

string normalizedIdentity(string value) {
  value = trim(value);
  transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(toupper(ch));
  });
  return value;
}

#ifdef _WIN32
IecdRecord recordFromStatement(sqlite3_stmt* statement, const string& version) {
  IecdRecord record;
  record.componentId = sqliteText(statement, 0);
  record.databaseVersion = version;
  record.manufacturer = sqliteText(statement, 1);
  record.manufacturerPartNumber = sqliteText(statement, 2);
  record.canonicalName = sqliteText(statement, 3);
  record.purposeLabel = sqliteText(statement, 4);
  record.printLabel = sqliteText(statement, 5);
  record.category = sqliteText(statement, 6);
  record.datasheetUrl = sqliteText(statement, 7);
  return record;
}

IecdMatch queryMatch(const filesystem::path& path, const string& version, const string& sql,
                     const vector<string>& parameters, IecdMatchStatus successStatus) {
  SqliteConnection connection;
  if (!openReadOnlyDatabase(path, connection)) return {IecdMatchStatus::DatabaseUnavailable, {}};
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db, sql.c_str(), -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return {IecdMatchStatus::DatabaseUnavailable, {}};
  }
  for (size_t index = 0; index < parameters.size(); ++index) {
    sqliteApi().bind_text(statement.stmt, static_cast<int>(index + 1), parameters[index].c_str(), -1,
                          SQLITE_TRANSIENT);
  }
  if (sqliteApi().step(statement.stmt) != SQLITE_ROW) return {IecdMatchStatus::NotFound, {}};
  const auto record = recordFromStatement(statement.stmt, version);
  if (sqliteApi().step(statement.stmt) == SQLITE_ROW) return {IecdMatchStatus::Ambiguous, {}};
  return {successStatus, record};
}
#endif

}  // namespace

bool IecdMatch::matched() const {
  return status == IecdMatchStatus::ExactMatch || status == IecdMatchStatus::UniqueMpnMatch;
}

bool IecdDatabase::open(const filesystem::path& path) {
  path_ = path;
  version_.clear();
  available_ = false;
#ifdef _WIN32
  SqliteConnection connection;
  if (!openReadOnlyDatabase(path, connection)) return false;
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
                             "SELECT key, value FROM metadata WHERE key IN ('schema_version','database_version')", -1,
                             &statement.stmt, nullptr) != SQLITE_OK) return false;
  string schemaVersion;
  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    const auto key = sqliteText(statement.stmt, 0);
    if (key == "schema_version") schemaVersion = sqliteText(statement.stmt, 1);
    else if (key == "database_version") version_ = sqliteText(statement.stmt, 1);
  }
  if (schemaVersion != "1" || version_.empty()) return false;

  SqliteStatement integrity;
  if (sqliteApi().prepare_v2(connection.db, "PRAGMA quick_check", -1, &integrity.stmt, nullptr) != SQLITE_OK ||
      sqliteApi().step(integrity.stmt) != SQLITE_ROW || sqliteText(integrity.stmt, 0) != "ok") return false;

  SqliteStatement schemaProbe;
  constexpr char kSchemaProbe[] = R"SQL(
    SELECT c.id
    FROM components c
    JOIN manufacturers m ON m.id=c.manufacturer_id
    JOIN purpose_categories pc ON pc.id=c.purpose_category_id
    LEFT JOIN manufacturer_aliases ma ON ma.manufacturer_id=m.id
    LEFT JOIN mpn_aliases pa ON pa.component_id=c.id
    LIMIT 0
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, kSchemaProbe, -1, &schemaProbe.stmt, nullptr) != SQLITE_OK) return false;
  available_ = true;
#endif
  return available_;
}

bool IecdDatabase::available() const { return available_; }
const string& IecdDatabase::version() const { return version_; }

IecdMatch IecdDatabase::lookup(const string& manufacturer, const string& manufacturerPartNumber) const {
  if (!available_) return {IecdMatchStatus::DatabaseUnavailable, {}};
  const auto mpn = normalizedIdentity(manufacturerPartNumber);
  if (mpn.empty()) return {IecdMatchStatus::NotFound, {}};
#ifdef _WIN32
  const auto maker = normalizedIdentity(manufacturer);
  if (!maker.empty()) {
    const string exactSql = R"SQL(
      SELECT c.id, m.canonical_name, c.mpn, c.canonical_name, c.purpose_label,
             c.print_label, pc.path, c.datasheet_url
      FROM components c
      JOIN manufacturers m ON m.id=c.manufacturer_id
      JOIN purpose_categories pc ON pc.id=c.purpose_category_id
      LEFT JOIN manufacturer_aliases ma ON ma.manufacturer_id=m.id
      LEFT JOIN mpn_aliases pa ON pa.component_id=c.id
      WHERE (m.normalized_name=? OR ma.normalized_alias=?)
        AND (c.normalized_mpn=? OR pa.normalized_alias=?)
      GROUP BY c.id LIMIT 2
    )SQL";
    auto match = queryMatch(path_, version_, exactSql, {maker, maker, mpn, mpn}, IecdMatchStatus::ExactMatch);
    if (match.status != IecdMatchStatus::NotFound) return match;
  }
  const string uniqueSql = R"SQL(
    SELECT c.id, m.canonical_name, c.mpn, c.canonical_name, c.purpose_label,
           c.print_label, pc.path, c.datasheet_url
    FROM components c
    JOIN manufacturers m ON m.id=c.manufacturer_id
    JOIN purpose_categories pc ON pc.id=c.purpose_category_id
    LEFT JOIN mpn_aliases pa ON pa.component_id=c.id
    WHERE c.normalized_mpn=? OR pa.normalized_alias=?
    GROUP BY c.id LIMIT 2
  )SQL";
  return queryMatch(path_, version_, uniqueSql, {mpn, mpn}, IecdMatchStatus::UniqueMpnMatch);
#else
  return {IecdMatchStatus::DatabaseUnavailable, {}};
#endif
}

bool applyIecdEnrichment(InventoryItem& item, const IecdMatch& match) {
  if (!match.matched()) {
    const auto next = match.status == IecdMatchStatus::DatabaseUnavailable
                          ? "database_unavailable"
                          : (!item.iecdComponentId.empty() ? "stale" : "not_in_iecd");
    if (item.enrichmentStatus == next) return false;
    item.enrichmentStatus = next;
    return true;
  }
  const auto before = item.iecdComponentId + item.iecdVersion + item.iecdCanonicalName + item.iecdPurposeLabel +
                      item.iecdPrintLabel + item.iecdCategory + item.iecdDatasheetUrl + item.enrichmentStatus;
  item.iecdComponentId = match.record.componentId;
  item.iecdVersion = match.record.databaseVersion;
  item.iecdCanonicalName = match.record.canonicalName;
  item.iecdPurposeLabel = match.record.purposeLabel;
  item.iecdPrintLabel = match.record.printLabel;
  item.iecdCategory = match.record.category;
  item.iecdDatasheetUrl = match.record.datasheetUrl;
  item.enrichmentStatus = "matched";
  const auto after = item.iecdComponentId + item.iecdVersion + item.iecdCanonicalName + item.iecdPurposeLabel +
                     item.iecdPrintLabel + item.iecdCategory + item.iecdDatasheetUrl + item.enrichmentStatus;
  return before != after;
}

string effectiveDatasheetUrl(const InventoryItem& item) {
  return !trim(item.datasheetUrl).empty() ? item.datasheetUrl : item.iecdDatasheetUrl;
}

}  // namespace inventatory
