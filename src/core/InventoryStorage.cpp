// HIMS - Hardware Inventory Management System
// Inventory store persistence and database-backed item loading.

#include "core/InventoryInternals.h"
#include "core/InventorySqlite.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <utility>

namespace hims {

using namespace std;

#ifdef _WIN32

namespace {

bool ensureHimsTableSchema(SqliteConnection& connection) {
  if (!execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS hims_items (
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
      digikey_part_number TEXT NOT NULL,
      datasheet_url TEXT NOT NULL,
      product_url TEXT NOT NULL,
      sync_status TEXT NOT NULL,
      sku TEXT NOT NULL,
      last_updated INTEGER NOT NULL,
      hims_id TEXT NOT NULL DEFAULT '',
      created_at INTEGER NOT NULL DEFAULT 0,
      machine_code TEXT NOT NULL DEFAULT '',
      rack_id TEXT NOT NULL DEFAULT '',
      rack_slot TEXT NOT NULL DEFAULT '',
      rack_assignment TEXT NOT NULL DEFAULT 'automatic'
    )
  )SQL")) {
    return false;
  }

  if (!tableColumnExists(connection, "hims_items", "hims_id")) {
    if (!execSql(connection, "ALTER TABLE hims_items ADD COLUMN hims_id TEXT NOT NULL DEFAULT ''")) {
      return false;
    }
  }
  if (!tableColumnExists(connection, "hims_items", "created_at")) {
    if (!execSql(connection, "ALTER TABLE hims_items ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0")) {
      return false;
    }
  }
  if (!tableColumnExists(connection, "hims_items", "machine_code")) {
    if (!execSql(connection, "ALTER TABLE hims_items ADD COLUMN machine_code TEXT NOT NULL DEFAULT ''")) {
      return false;
    }
  }
  if (!tableColumnExists(connection, "hims_items", "rack_id") &&
      !execSql(connection, "ALTER TABLE hims_items ADD COLUMN rack_id TEXT NOT NULL DEFAULT ''")) return false;
  if (!tableColumnExists(connection, "hims_items", "rack_slot") &&
      !execSql(connection, "ALTER TABLE hims_items ADD COLUMN rack_slot TEXT NOT NULL DEFAULT ''")) return false;
  if (!tableColumnExists(connection, "hims_items", "rack_assignment") &&
      !execSql(connection, "ALTER TABLE hims_items ADD COLUMN rack_assignment TEXT NOT NULL DEFAULT 'automatic'")) return false;
  if (!execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS hims_racks (
      id TEXT PRIMARY KEY,
      code TEXT NOT NULL UNIQUE,
      component_type TEXT NOT NULL,
      rows_count INTEGER NOT NULL DEFAULT 5,
      columns_count INTEGER NOT NULL DEFAULT 5,
      created_at INTEGER NOT NULL DEFAULT 0
    )
  )SQL")) return false;
  if (!execSql(connection, R"SQL(
    CREATE UNIQUE INDEX IF NOT EXISTS idx_hims_items_rack_slot
    ON hims_items(rack_id, rack_slot)
    WHERE rack_id <> '' AND rack_slot <> ''
  )SQL")) return false;
  return true;
}

bool loadRacks(SqliteConnection& connection, vector<HimsRack>& racks) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT id, code, component_type, rows_count, columns_count, created_at FROM hims_racks ORDER BY created_at, code",
      -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    HimsRack rack;
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

InventoryItem legacyRowToItem(sqlite3_stmt* stmt) {
  InventoryItem item;
  const auto legacyId = sqliteApi().column_int64(stmt, 0);
  const auto partNumber = sqliteText(stmt, 1);
  const auto partNumberNormalized = sqliteText(stmt, 2);
  const auto quantity = sqliteApi().column_int(stmt, 3);
  const auto location = sqliteText(stmt, 4);
  const auto manufacturer = sqliteText(stmt, 5);
  const auto packageName = sqliteText(stmt, 6);
  const auto description = sqliteText(stmt, 7);
  const auto notes = sqliteText(stmt, 8);
  const auto updatedAt = sqliteText(stmt, 10);
  const auto digikeyPartNumber = sqliteText(stmt, 11);
  const auto category = sqliteText(stmt, 12);
  const auto subcategory = sqliteText(stmt, 13);
  const auto productUrl = sqliteText(stmt, 14);
  const auto datasheetUrl = sqliteText(stmt, 15);
  const auto searchText = sqliteText(stmt, 18);
  const auto enrichmentStatus = sqliteText(stmt, 19);
  const auto inventoryArea = sqliteText(stmt, 22);
  const auto reorderOverride = sqliteApi().column_int(stmt, 23);
  const auto hardwareType = sqliteText(stmt, 24);
  const auto hardwareSize = sqliteText(stmt, 25);
  const auto hardwareLength = sqliteText(stmt, 26);
  const auto filamentMaterial = sqliteText(stmt, 28);
  const auto filamentColor = sqliteText(stmt, 29);
  const auto filamentDiameterMm = sqliteText(stmt, 30);

  item.id = to_string(legacyId);
  item.partName = partNumber.empty() ? description : partNumber;
  item.manufacturer = manufacturer;
  item.category = category.empty() ? subcategory : (subcategory.empty() ? category : category + " / " + subcategory);
  item.quantity = quantity;
  item.reorderThreshold = reorderOverride >= 0 ? reorderOverride : 0;
  item.location = location;
  if (!inventoryArea.empty()) {
    item.tags.push_back(inventoryArea);
  }
  if (!packageName.empty()) {
    item.tags.push_back(packageName);
  }
  if (!subcategory.empty()) {
    item.tags.push_back(subcategory);
  }
  if (!packageName.empty()) {
    item.parameters.push_back({"Package", packageName});
  }
  if (!inventoryArea.empty()) {
    item.parameters.push_back({"Inventory Area", inventoryArea});
  }
  if (!hardwareType.empty()) {
    item.parameters.push_back({"Hardware Type", hardwareType});
  }
  if (!hardwareSize.empty()) {
    item.parameters.push_back({"Hardware Size", hardwareSize});
  }
  if (!hardwareLength.empty()) {
    item.parameters.push_back({"Hardware Length", hardwareLength});
  }
  if (!filamentMaterial.empty()) {
    item.parameters.push_back({"Filament Material", filamentMaterial});
  }
  if (!filamentColor.empty()) {
    item.parameters.push_back({"Filament Color", filamentColor});
  }
  if (!filamentDiameterMm.empty()) {
    item.parameters.push_back({"Filament Diameter", filamentDiameterMm});
  }
  item.notes = description.empty() ? notes : (notes.empty() ? description : description + " | " + notes);
  item.digikeyPartNumber = digikeyPartNumber;
  item.datasheetUrl = datasheetUrl;
  item.productUrl = productUrl;
  item.syncStatus = enrichmentStatus.empty() ? "needs_metadata" : toLower(enrichmentStatus);
  item.sku = partNumberNormalized.empty() ? partNumber : partNumberNormalized;
  item.lastUpdated = nowEpoch();
  if (!updatedAt.empty()) {
    item.lastUpdated = nowEpoch();
  }
  if (!searchText.empty() && item.notes.empty()) {
    item.notes = searchText;
  }
  return item;
}

bool loadItemsFromHimsTable(SqliteConnection& connection, vector<InventoryItem>& items) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, part_name, manufacturer, category, quantity, reorder_threshold, location,
           tags, parameters, notes, digikey_part_number, datasheet_url, product_url,
           sync_status, sku, last_updated, hims_id, created_at, machine_code,
           rack_id, rack_slot, rack_assignment
    FROM hims_items
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
    item.digikeyPartNumber = sqliteText(statement.stmt, 10);
    item.datasheetUrl = sqliteText(statement.stmt, 11);
    item.productUrl = sqliteText(statement.stmt, 12);
    item.syncStatus = sqliteText(statement.stmt, 13);
    item.sku = sqliteText(statement.stmt, 14);
    item.lastUpdated = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 15));
    item.himsId = sqliteText(statement.stmt, 16);
    item.createdAt = static_cast<time_t>(sqliteApi().column_int64(statement.stmt, 17));
    item.machineCode = sqliteText(statement.stmt, 18);
    item.rackId = sqliteText(statement.stmt, 19);
    item.rackSlot = sqliteText(statement.stmt, 20);
    item.rackAssignment = parseRackAssignmentMode(sqliteText(statement.stmt, 21));
    items.push_back(move(item));
  }

  return true;
}

bool importLegacyItems(SqliteConnection& connection, vector<InventoryItem>& items) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, part_number, part_number_normalized, quantity, location, manufacturer, package, description,
           notes, created_at, updated_at, digikey_part_number, category, subcategory, product_url, datasheet_url,
           image_url, specs_json, search_text, enrichment_status, enrichment_error, last_enriched_at,
           inventory_area, reorder_point_override, hardware_type, hardware_size, hardware_length,
           hardware_material_finish, filament_material, filament_color, filament_diameter_mm,
           filament_spool_weight_g, filament_remaining_weight_g
    FROM items
    ORDER BY part_number COLLATE NOCASE ASC
  )SQL";

  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    items.push_back(legacyRowToItem(statement.stmt));
  }

  return true;
}

bool ensureDeviceEventCommitSchema(SqliteConnection& connection) {
  return execSql(connection, R"SQL(
    CREATE TABLE IF NOT EXISTS hims_device_events (
      event_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, event_type TEXT NOT NULL,
      event_code TEXT NOT NULL, event_value INTEGER NOT NULL, state TEXT NOT NULL DEFAULT 'received',
      result_id TEXT NOT NULL DEFAULT '', result_status TEXT NOT NULL DEFAULT '',
      result_existing INTEGER NOT NULL DEFAULT 0, result_item_name TEXT NOT NULL DEFAULT '',
      result_requested_delta INTEGER NOT NULL DEFAULT 0, result_applied_delta INTEGER NOT NULL DEFAULT 0,
      result_quantity INTEGER NOT NULL DEFAULT 0, result_location TEXT NOT NULL DEFAULT '',
      result_code TEXT NOT NULL DEFAULT '', result_message TEXT NOT NULL DEFAULT '',
      result_acknowledged INTEGER NOT NULL DEFAULT 0, received_at INTEGER NOT NULL DEFAULT 0,
      completed_at INTEGER NOT NULL DEFAULT 0
    )
  )SQL");
}

bool writeItemsToHimsTable(SqliteConnection& connection, const vector<InventoryItem>& items,
                           const vector<HimsRack>& racks, const DeviceEventCommit* deviceEvent = nullptr) {
  if (!ensureHimsTableSchema(connection)) {
    return false;
  }
  if (deviceEvent != nullptr && !ensureDeviceEventCommitSchema(connection)) return false;

  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    return false;
  }
  if (!execSql(connection, "DELETE FROM hims_racks")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  {
    SqliteStatement rackStatement;
    const char* rackSql = "INSERT INTO hims_racks (id, code, component_type, rows_count, columns_count, created_at) VALUES (?, ?, ?, ?, ?, ?)";
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
  if (!execSql(connection, "DELETE FROM hims_items")) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT OR REPLACE INTO hims_items (
      id, part_name, manufacturer, category, quantity, reorder_threshold, location,
      tags, parameters, notes, digikey_part_number, datasheet_url, product_url,
      sync_status, sku, last_updated, hims_id, created_at, machine_code,
      rack_id, rack_slot, rack_assignment
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
    sqliteApi().bind_text(statement.stmt, 11, item.digikeyPartNumber.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 12, item.datasheetUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 13, item.productUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 14, item.syncStatus.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 15, item.sku.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int64(statement.stmt, 16, static_cast<sqlite3_int64>(item.lastUpdated));
    sqliteApi().bind_text(statement.stmt, 17, item.himsId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int64(statement.stmt, 18, static_cast<sqlite3_int64>(item.createdAt));
    sqliteApi().bind_text(statement.stmt, 19, item.machineCode.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 20, item.rackId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 21, item.rackSlot.c_str(), -1, SQLITE_TRANSIENT);
    const auto assignment = rackAssignmentModeName(item.rackAssignment);
    sqliteApi().bind_text(statement.stmt, 22, assignment.c_str(), -1, SQLITE_TRANSIENT);

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
      UPDATE hims_device_events SET
        state='completed', result_id=?, result_status=?, result_existing=?, result_item_name=?,
        result_requested_delta=?, result_applied_delta=?, result_quantity=?, result_location=?,
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
    sqliteApi().bind_int(eventStatement.stmt, 5, deviceEvent->requestedDelta);
    sqliteApi().bind_int(eventStatement.stmt, 6, deviceEvent->appliedDelta);
    sqliteApi().bind_int(eventStatement.stmt, 7, deviceEvent->quantity);
    sqliteApi().bind_text(eventStatement.stmt, 8, deviceEvent->location.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(eventStatement.stmt, 9, deviceEvent->code.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(eventStatement.stmt, 10, deviceEvent->message.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int64(eventStatement.stmt, 11, static_cast<sqlite3_int64>(deviceEvent->completedAt));
    sqliteApi().bind_text(eventStatement.stmt, 12, deviceEvent->eventId.c_str(), -1, SQLITE_TRANSIENT);
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

vector<HimsRack>& InventoryStore::racks() { return racks_; }

const vector<HimsRack>& InventoryStore::racks() const { return racks_; }

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

  if (!ensureHimsTableSchema(connection)) {
    return false;
  }
  loadRacks(connection, racks_);

  const bool hasHimsTable = tableExists(connection, "hims_items");
  const bool hasLegacyTable = tableExists(connection, "items");

  vector<InventoryItem> himsItems;
  bool loaded = false;
  if (hasHimsTable) {
    loaded = loadItemsFromHimsTable(connection, himsItems);
  }

  vector<InventoryItem> legacyItems;
  if (hasLegacyTable) {
    importLegacyItems(connection, legacyItems);
  }

  if (!legacyItems.empty() && (himsItems.empty() || legacyItems.size() > himsItems.size())) {
    items_ = move(legacyItems);
    loaded = true;
    ensureInventoryIdentifiers(items_);
    writeItemsToHimsTable(connection, items_, racks_);
  } else if (loaded && !himsItems.empty()) {
    items_ = move(himsItems);
  } else if (!loaded || items_.empty()) {
    if (!himsItems.empty()) {
      items_ = move(himsItems);
      loaded = true;
    } else if (!legacyItems.empty()) {
      items_ = move(legacyItems);
      loaded = true;
      ensureInventoryIdentifiers(items_);
      writeItemsToHimsTable(connection, items_, racks_);
    }
  }

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

  return writeItemsToHimsTable(connection, items, racks_);
#else
  auto items = items_;
  ensureInventoryIdentifiers(items);

  filesystem::create_directories(path.parent_path());

  ofstream file(path, ios::trunc);
  if (!file) {
    return false;
  }

  file << "# HIMS inventory data\n";
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
  return writeItemsToHimsTable(connection, items, racks_, &event);
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
    return toLower(item.id) == needle || toLower(item.himsId) == needle || toLower(item.sku) == needle ||
           toLower(item.machineCode) == needle || toLower(item.digikeyPartNumber) == needle ||
           containsInsensitive(item.productUrl, needle) || containsInsensitive(item.datasheetUrl, needle);
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findByCode(const string& code) const {
  const auto needle = toLower(trim(code));
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return toLower(item.id) == needle || toLower(item.himsId) == needle || toLower(item.sku) == needle ||
           toLower(item.machineCode) == needle || toLower(item.digikeyPartNumber) == needle ||
           containsInsensitive(item.productUrl, needle) || containsInsensitive(item.datasheetUrl, needle);
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

}  // namespace hims
