// Inventatory - Hardware Inventory Management System
// Inventory store persistence and database-backed item loading.

#include "core/InventoryInternals.h"
#include "core/InventorySqlite.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <utility>

namespace inventatory {

using namespace std;

#ifdef _WIN32

namespace {

bool migrateLegacySchema(SqliteConnection& connection) {
  if (!tableExists(connection, "inventatory_items") && tableExists(connection, "hims_items") &&
      !execSql(connection, "ALTER TABLE hims_items RENAME TO inventatory_items")) return false;
  if (tableExists(connection, "inventatory_items") && tableColumnExists(connection, "inventatory_items", "hims_id") &&
      !execSql(connection, "ALTER TABLE inventatory_items RENAME COLUMN hims_id TO inventatory_id")) return false;
  if (!tableExists(connection, "inventatory_racks") && tableExists(connection, "hims_racks") &&
      !execSql(connection, "ALTER TABLE hims_racks RENAME TO inventatory_racks")) return false;
  if (!tableExists(connection, "inventatory_device_events") && tableExists(connection, "hims_device_events") &&
      !execSql(connection, "ALTER TABLE hims_device_events RENAME TO inventatory_device_events")) return false;
  return true;
}

bool ensureInventatoryTableSchema(SqliteConnection& connection) {
  if (!migrateLegacySchema(connection)) return false;
  const auto createCurrentTable = [&]() {
    return execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS inventatory_items (
      id TEXT PRIMARY KEY,
      part_name TEXT NOT NULL,
      manufacturer TEXT NOT NULL,
      category TEXT NOT NULL,
      quantity INTEGER NOT NULL,
      reorder_threshold INTEGER NOT NULL,
      location TEXT NOT NULL,
      tags TEXT NOT NULL,
      parameters TEXT NOT NULL,
      notes TEXT NOT NULL,
      manufacturer_part_number TEXT NOT NULL,
      datasheet_url TEXT NOT NULL,
      enrichment_status TEXT NOT NULL,
      catalogue_component_id TEXT NOT NULL DEFAULT '',
      catalogue_version TEXT NOT NULL DEFAULT '',
      catalogue_canonical_name TEXT NOT NULL DEFAULT '',
      catalogue_purpose_label TEXT NOT NULL DEFAULT '',
      catalogue_print_label TEXT NOT NULL DEFAULT '',
      catalogue_category TEXT NOT NULL DEFAULT '',
      catalogue_datasheet_url TEXT NOT NULL DEFAULT '',
      last_updated INTEGER NOT NULL,
      inventatory_id TEXT NOT NULL DEFAULT '',
      created_at INTEGER NOT NULL DEFAULT 0,
      machine_code TEXT NOT NULL DEFAULT '',
      rack_id TEXT NOT NULL DEFAULT '',
      rack_slot TEXT NOT NULL DEFAULT '',
      rack_assignment TEXT NOT NULL DEFAULT 'automatic'
    )
  )SQL");
  };

  // The retired enrichment overlay lived in the inventory table. Rebuild the
  // table once, copying only user-owned inventory fields. Catalogue data now
  // lives in catalogues.db and can be removed without touching inventory.
  const string retiredPrefix = "ie" "cd_";
  if (tableExists(connection, "inventatory_items") &&
      tableColumnExists(connection, "inventatory_items", retiredPrefix + "component_id")) {
    if (!execSql(connection, "BEGIN IMMEDIATE") ||
        !execSql(connection, "ALTER TABLE inventatory_items RENAME TO inventatory_items_retired_enrichment") ||
        !createCurrentTable() ||
        !execSql(connection, R"SQL(
          INSERT INTO inventatory_items (
            id, part_name, manufacturer, category, quantity, reorder_threshold, location,
            tags, parameters, notes, manufacturer_part_number, datasheet_url, enrichment_status,
            last_updated, inventatory_id, created_at, machine_code, rack_id, rack_slot, rack_assignment
          )
          SELECT id, part_name, manufacturer, category, quantity, reorder_threshold, location,
                 tags, parameters, notes, manufacturer_part_number, datasheet_url, 'not_in_catalogue',
                 last_updated, inventatory_id, created_at, machine_code, rack_id, rack_slot, rack_assignment
          FROM inventatory_items_retired_enrichment
        )SQL") ||
        !execSql(connection, "DROP TABLE inventatory_items_retired_enrichment") ||
        !execSql(connection, "COMMIT")) {
      execSql(connection, "ROLLBACK");
      return false;
    }
  }

  if (tableExists(connection, "inventatory_items") &&
      !tableColumnExists(connection, "inventatory_items", "manufacturer_part_number")) {
    if (!execSql(connection, "BEGIN IMMEDIATE") ||
        !execSql(connection, "ALTER TABLE inventatory_items RENAME TO inventatory_items_pre_catalogue") ||
        !createCurrentTable() ||
        !execSql(connection, R"SQL(
          INSERT INTO inventatory_items (
            id, part_name, manufacturer, category, quantity, reorder_threshold, location,
            tags, parameters, notes, manufacturer_part_number, datasheet_url, enrichment_status,
            last_updated, inventatory_id, created_at, machine_code, rack_id, rack_slot, rack_assignment
          )
          SELECT id, part_name, manufacturer, category, quantity, reorder_threshold, location,
                 tags, parameters, notes, sku,
                 CASE WHEN lower(datasheet_url) LIKE '%.pdf%' THEN datasheet_url ELSE '' END,
                 'not_in_catalogue', last_updated, inventatory_id, created_at, machine_code,
                 rack_id, rack_slot, rack_assignment
          FROM inventatory_items_pre_catalogue
        )SQL") ||
        !execSql(connection, "DROP TABLE inventatory_items_pre_catalogue") ||
        !execSql(connection, "COMMIT")) {
      execSql(connection, "ROLLBACK");
      return false;
    }
  }
  if (!createCurrentTable()) return false;

  if (!tableColumnExists(connection, "inventatory_items", "inventatory_id")) {
    if (!execSql(connection, "ALTER TABLE inventatory_items ADD COLUMN inventatory_id TEXT NOT NULL DEFAULT ''")) {
      return false;
    }
  }
  if (!tableColumnExists(connection, "inventatory_items", "created_at")) {
    if (!execSql(connection, "ALTER TABLE inventatory_items ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0")) {
      return false;
    }
  }
  if (!tableColumnExists(connection, "inventatory_items", "machine_code")) {
    if (!execSql(connection, "ALTER TABLE inventatory_items ADD COLUMN machine_code TEXT NOT NULL DEFAULT ''")) {
      return false;
    }
  }
  if (!tableColumnExists(connection, "inventatory_items", "rack_id") &&
      !execSql(connection, "ALTER TABLE inventatory_items ADD COLUMN rack_id TEXT NOT NULL DEFAULT ''")) return false;
  if (!tableColumnExists(connection, "inventatory_items", "rack_slot") &&
      !execSql(connection, "ALTER TABLE inventatory_items ADD COLUMN rack_slot TEXT NOT NULL DEFAULT ''")) return false;
  if (!tableColumnExists(connection, "inventatory_items", "rack_assignment") &&
      !execSql(connection, "ALTER TABLE inventatory_items ADD COLUMN rack_assignment TEXT NOT NULL DEFAULT 'automatic'")) return false;
  if (!execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS inventatory_racks (
      id TEXT PRIMARY KEY,
      code TEXT NOT NULL UNIQUE,
      component_type TEXT NOT NULL,
      rows_count INTEGER NOT NULL DEFAULT 5,
      columns_count INTEGER NOT NULL DEFAULT 5,
      created_at INTEGER NOT NULL DEFAULT 0
    )
  )SQL")) return false;
  if (!execSql(connection, R"SQL(
    CREATE UNIQUE INDEX IF NOT EXISTS idx_inventatory_items_rack_slot
    ON inventatory_items(rack_id, rack_slot)
    WHERE rack_id <> '' AND rack_slot <> ''
  )SQL")) return false;
  return true;
}

bool loadRacks(SqliteConnection& connection, vector<InventatoryRack>& racks) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT id, code, component_type, rows_count, columns_count, created_at FROM inventatory_racks ORDER BY created_at, code",
      -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    InventatoryRack rack;
    rack.id = sqliteText(statement.stmt, 0);
    rack.code = sqliteText(statement.stmt, 1);
    rack.componentType = sqliteText(statement.stmt, 2);
    rack.rows = sqliteApi().column_int(statement.stmt, 3);
    rack.columns = sqliteApi().column_int(statement.stmt, 4);
    rack.createdAt = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 5));
    racks.push_back(move(rack));
  }
  return true;
}

bool loadItemsFromInventatoryTable(SqliteConnection& connection, vector<InventoryItem>& items) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, part_name, manufacturer, category, quantity, reorder_threshold, location,
           tags, parameters, notes, manufacturer_part_number, datasheet_url, enrichment_status,
           catalogue_component_id, catalogue_version, catalogue_canonical_name, catalogue_purpose_label,
           catalogue_print_label, catalogue_category, catalogue_datasheet_url,
           last_updated, inventatory_id, created_at, machine_code,
           rack_id, rack_slot, rack_assignment
    FROM inventatory_items
    ORDER BY part_name COLLATE NOCASE ASC
  )SQL";

  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    InventoryItem item;
    item.id = sqliteText(statement.stmt, 0);
    item.partName = sqliteText(statement.stmt, 1);
    item.manufacturer = sqliteText(statement.stmt, 2);
    item.category = sqliteText(statement.stmt, 3);
    item.quantity = sqliteApi().column_int(statement.stmt, 4);
    item.reorderThreshold = sqliteApi().column_int(statement.stmt, 5);
    item.location = sqliteText(statement.stmt, 6);
    item.tags = deserializeTagsFromStorage(sqliteText(statement.stmt, 7));
    item.parameters = deserializeParametersFromStorage(sqliteText(statement.stmt, 8));
    item.notes = sqliteText(statement.stmt, 9);
    item.manufacturerPartNumber = sqliteText(statement.stmt, 10);
    item.datasheetUrl = sqliteText(statement.stmt, 11);
    item.catalogueStatus = sqliteText(statement.stmt, 12);
    item.cataloguePartId = sqliteText(statement.stmt, 13);
    item.catalogueSnapshot = sqliteText(statement.stmt, 14);
    item.catalogueName = sqliteText(statement.stmt, 15);
    item.cataloguePurposeLabel = sqliteText(statement.stmt, 16);
    item.cataloguePrintLabel = sqliteText(statement.stmt, 17);
    item.catalogueCategory = sqliteText(statement.stmt, 18);
    item.catalogueDatasheetUrl = sqliteText(statement.stmt, 19);
    item.lastUpdated = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 20));
    item.inventatoryId = sqliteText(statement.stmt, 21);
    item.createdAt = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 22));
    item.machineCode = sqliteText(statement.stmt, 23);
    item.rackId = sqliteText(statement.stmt, 24);
    item.rackSlot = sqliteText(statement.stmt, 25);
    item.rackAssignment = parseRackAssignmentMode(sqliteText(statement.stmt, 26));
    items.push_back(move(item));
  }

  return true;
}

bool ensureDeviceEventCommitSchema(SqliteConnection& connection) {
  if (!execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS inventatory_device_events (
      event_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, event_type TEXT NOT NULL,
      event_code TEXT NOT NULL, event_value INTEGER NOT NULL, state TEXT NOT NULL DEFAULT 'received',
      result_id TEXT NOT NULL DEFAULT '', result_status TEXT NOT NULL DEFAULT '',
      result_existing INTEGER NOT NULL DEFAULT 0, result_item_name TEXT NOT NULL DEFAULT '',
      result_purpose_label TEXT NOT NULL DEFAULT '',
      result_requested_delta INTEGER NOT NULL DEFAULT 0, result_applied_delta INTEGER NOT NULL DEFAULT 0,
      result_quantity INTEGER NOT NULL DEFAULT 0, result_location TEXT NOT NULL DEFAULT '',
      result_code TEXT NOT NULL DEFAULT '', result_message TEXT NOT NULL DEFAULT '',
      result_acknowledged INTEGER NOT NULL DEFAULT 0, received_at INTEGER NOT NULL DEFAULT 0,
      completed_at INTEGER NOT NULL DEFAULT 0
    )
  )SQL")) return false;
  if (!tableColumnExists(connection, "inventatory_device_events", "result_purpose_label")) {
    return execSql(connection,
                   "ALTER TABLE inventatory_device_events ADD COLUMN result_purpose_label TEXT NOT NULL DEFAULT ''");
  }
  return true;
}

bool writeItemsToInventatoryTable(SqliteConnection& connection, const vector<InventoryItem>& items,
                           const vector<InventatoryRack>& racks, const DeviceEventCommit* deviceEvent = nullptr) {
  if (!ensureInventatoryTableSchema(connection)) {
    return false;
  }
  if (deviceEvent != nullptr && !ensureDeviceEventCommitSchema(connection)) return false;

  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    return false;
  }
  if (!execSql(connection, "DELETE FROM inventatory_racks")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  {
    SqliteStatement rackStatement;
    const char* rackSql = "INSERT INTO inventatory_racks (id, code, component_type, rows_count, columns_count, created_at) VALUES (?, ?, ?, ?, ?, ?)";
    if (sqliteApi().prepare_v2(connection.db, rackSql, -1, &rackStatement.stmt, nullptr) != SQLITE_OK) {
      execSql(connection, "ROLLBACK");
      return false;
    }
    for (const auto& rack : racks) {
      sqliteApi().bind_text(rackStatement.stmt, 1, rack.id.c_str(), -1, SQLITE_TRANSIENT);
      sqliteApi().bind_text(rackStatement.stmt, 2, rack.code.c_str(), -1, SQLITE_TRANSIENT);
      sqliteApi().bind_text(rackStatement.stmt, 3, rack.componentType.c_str(), -1, SQLITE_TRANSIENT);
      sqliteApi().bind_int(rackStatement.stmt, 4, rack.rows);
      sqliteApi().bind_int(rackStatement.stmt, 5, rack.columns);
      sqliteApi().bind_int64(rackStatement.stmt, 6, static_cast<sqlite3_int64>(rack.createdAt));
      if (sqliteApi().step(rackStatement.stmt) != SQLITE_DONE) {
        execSql(connection, "ROLLBACK");
        return false;
      }
      sqliteApi().reset(rackStatement.stmt);
      sqliteApi().clear_bindings(rackStatement.stmt);
    }
  }
  if (!execSql(connection, "DELETE FROM inventatory_items")) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT OR REPLACE INTO inventatory_items (
      id, part_name, manufacturer, category, quantity, reorder_threshold, location,
      tags, parameters, notes, manufacturer_part_number, datasheet_url, enrichment_status,
      catalogue_component_id, catalogue_version, catalogue_canonical_name, catalogue_purpose_label,
      catalogue_print_label, catalogue_category, catalogue_datasheet_url,
      last_updated, inventatory_id, created_at, machine_code,
      rack_id, rack_slot, rack_assignment
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  )SQL";

  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  for (const auto& item : items) {
    sqliteApi().bind_text(statement.stmt, 1, item.id.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 2, item.partName.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 3, item.manufacturer.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 4, item.category.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int(statement.stmt, 5, item.quantity);
    sqliteApi().bind_int(statement.stmt, 6, item.reorderThreshold);
    sqliteApi().bind_text(statement.stmt, 7, item.location.c_str(), -1, SQLITE_TRANSIENT);
    const auto tags = serializeTagsForStorage(item.tags);
    sqliteApi().bind_text(statement.stmt, 8, tags.c_str(), -1, SQLITE_TRANSIENT);
    const auto parameters = serializeParametersForStorage(item.parameters);
    sqliteApi().bind_text(statement.stmt, 9, parameters.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 10, item.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 11, item.manufacturerPartNumber.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 12, item.datasheetUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 13, item.catalogueStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 14, item.cataloguePartId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 15, item.catalogueSnapshot.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 16, item.catalogueName.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 17, item.cataloguePurposeLabel.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 18, item.cataloguePrintLabel.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 19, item.catalogueCategory.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 20, item.catalogueDatasheetUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int64(statement.stmt, 21, static_cast<sqlite3_int64>(item.lastUpdated));
    sqliteApi().bind_text(statement.stmt, 22, item.inventatoryId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int64(statement.stmt, 23, static_cast<sqlite3_int64>(item.createdAt));
    sqliteApi().bind_text(statement.stmt, 24, item.machineCode.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 25, item.rackId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 26, item.rackSlot.c_str(), -1, SQLITE_TRANSIENT);
    const auto assignment = rackAssignmentModeName(item.rackAssignment);
    sqliteApi().bind_text(statement.stmt, 27, assignment.c_str(), -1, SQLITE_TRANSIENT);

    if (sqliteApi().step(statement.stmt) != SQLITE_DONE) {
      execSql(connection, "ROLLBACK");
      return false;
    }

    sqliteApi().reset(statement.stmt);
    sqliteApi().clear_bindings(statement.stmt);
  }

  if (deviceEvent != nullptr) {
    SqliteStatement eventStatement;
    const char* eventSql = R"SQL(
      UPDATE inventatory_device_events SET
        state='completed', result_id=?, result_status=?, result_existing=?, result_item_name=?,
        result_purpose_label=?, result_requested_delta=?, result_applied_delta=?, result_quantity=?, result_location=?,
        result_code=?, result_message=?, completed_at=?
      WHERE event_id=? AND state='received'
    )SQL";
    if (sqliteApi().prepare_v2(connection.db, eventSql, -1, &eventStatement.stmt, nullptr) != SQLITE_OK) {
      execSql(connection, "ROLLBACK");
      return false;
    }
    sqliteApi().bind_text(eventStatement.stmt, 1, deviceEvent->resultId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(eventStatement.stmt, 2, deviceEvent->status.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int(eventStatement.stmt, 3, deviceEvent->existing ? 1 : 0);
    sqliteApi().bind_text(eventStatement.stmt, 4, deviceEvent->itemName.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(eventStatement.stmt, 5, deviceEvent->purposeLabel.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int(eventStatement.stmt, 6, deviceEvent->requestedDelta);
    sqliteApi().bind_int(eventStatement.stmt, 7, deviceEvent->appliedDelta);
    sqliteApi().bind_int(eventStatement.stmt, 8, deviceEvent->quantity);
    sqliteApi().bind_text(eventStatement.stmt, 9, deviceEvent->location.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(eventStatement.stmt, 10, deviceEvent->code.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(eventStatement.stmt, 11, deviceEvent->message.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int64(eventStatement.stmt, 12, static_cast<sqlite3_int64>(deviceEvent->completedAt));
    sqliteApi().bind_text(eventStatement.stmt, 13, deviceEvent->eventId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqliteApi().step(eventStatement.stmt) != SQLITE_DONE) {
      execSql(connection, "ROLLBACK");
      return false;
    }
  }

  if (!execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  return true;
}

#endif

}  // namespace

vector<InventoryItem>& InventoryStore::items() {
  return items_;
}

vector<InventatoryRack>& InventoryStore::racks() { return racks_; }

const vector<InventatoryRack>& InventoryStore::racks() const { return racks_; }

const vector<InventoryItem>& InventoryStore::items() const {
  return items_;
}

bool InventoryStore::load(const filesystem::path& path) {
  items_.clear();
  racks_.clear();
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  if (!ensureInventatoryTableSchema(connection)) {
    return false;
  }
  loadRacks(connection, racks_);

  vector<InventoryItem> inventatoryItems;
  const bool loaded = loadItemsFromInventatoryTable(connection, inventatoryItems);
  if (loaded) items_ = move(inventatoryItems);

  ensureInventoryIdentifiers(items_);
  reconcileRackAssignments(*this);
  return loaded;
#else
  ifstream file(path);
  if (!file) {
    return false;
  }

  string line;
  while (getline(file, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    InventoryItem item;
    if (deserializeItem(line, item)) {
      items_.push_back(move(item));
    }
  }

  ensureInventoryIdentifiers(items_);
  return !items_.empty();
#endif
}

bool InventoryStore::save(const filesystem::path& path) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);

  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  return writeItemsToInventatoryTable(connection, items, racks_);
#else
  auto items = items_;
  ensureInventoryIdentifiers(items);

  filesystem::create_directories(path.parent_path());

  ofstream file(path, ios::trunc);
  if (!file) {
    return false;
  }

  file << "# Inventatory inventory data\n";
  for (const auto& item : items) {
    file << serializeItem(item) << '\n';
  }
  return true;
#endif
}

bool InventoryStore::saveWithDeviceEvent(const filesystem::path& path, const DeviceEventCommit& event) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  SqliteConnection connection;
  if (!openDatabase(path, connection)) return false;
  return writeItemsToInventatoryTable(connection, items, racks_, &event);
#else
  (void)event;
  return save(path);
#endif
}

InventoryItem* InventoryStore::findById(const string& id) {
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return item.id == id;
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findById(const string& id) const {
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return item.id == id;
  });
  return it == items_.end() ? nullptr : &(*it);
}

InventoryItem* InventoryStore::findByCode(const string& code) {
  const auto needle = toLower(trim(code));
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return toLower(item.id) == needle || toLower(item.inventatoryId) == needle || toLower(item.manufacturerPartNumber) == needle ||
           toLower(item.machineCode) == needle || containsInsensitive(item.datasheetUrl, needle) ||
           containsInsensitive(item.catalogueDatasheetUrl, needle);
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findByCode(const string& code) const {
  const auto needle = toLower(trim(code));
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return toLower(item.id) == needle || toLower(item.inventatoryId) == needle || toLower(item.manufacturerPartNumber) == needle ||
           toLower(item.machineCode) == needle || containsInsensitive(item.datasheetUrl, needle) ||
           containsInsensitive(item.catalogueDatasheetUrl, needle);
  });
  return it == items_.end() ? nullptr : &(*it);
}

InventoryItem* InventoryStore::findByMachineCode(const string& machineCode) {
  const auto needle = normalizeMachineCode(machineCode);
  if (needle.empty()) {
    return nullptr;
  }

  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return normalizeMachineCode(item.machineCode) == needle;
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findByMachineCode(const string& machineCode) const {
  const auto needle = normalizeMachineCode(machineCode);
  if (needle.empty()) {
    return nullptr;
  }

  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return normalizeMachineCode(item.machineCode) == needle;
  });
  return it == items_.end() ? nullptr : &(*it);
}

}  // namespace inventatory
