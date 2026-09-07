// Inventatory - Hardware Inventory Management System
// Inventory store persistence and database-backed item loading.

#include "core/InventoryInternals.h"
#include "core/InventorySqlite.h"
#include "core/InventoryVersionInternal.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

namespace inventatory {

using namespace std;

#ifdef _WIN32

namespace {

bool ensureInventatoryTableSchema(SqliteConnection& connection) {
  return ensureInventoryDatabaseSchema(connection);
}

bool loadRacks(SqliteConnection& connection, vector<InventatoryRack>& racks) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT id, code, component_type, rows_count, columns_count, created_at FROM inventatory_racks ORDER BY created_at, code",
      -1, &statement.stmt, nullptr) != SQLITE_OK) return false;
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    InventatoryRack rack;
    rack.id = sqliteText(statement.stmt, 0);
    rack.code = sqliteText(statement.stmt, 1);
    rack.componentType = sqliteText(statement.stmt, 2);
    if (!sqliteInt32(statement.stmt, 3, rack.rows) || !sqliteInt32(statement.stmt, 4, rack.columns) ||
        !sqliteTime(statement.stmt, 5, rack.createdAt) || rack.rows <= 0 || rack.columns <= 0 || rack.rows > 10000 ||
        rack.columns > 10000) {
      return false;
    }
    racks.push_back(move(rack));
  }
  return stepResult == SQLITE_DONE;
}

bool loadItemsFromInventatoryTable(SqliteConnection& connection, vector<InventoryItem>& items) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, part_name, manufacturer, category, quantity, reorder_threshold, location,
           tags, parameters, notes, digikey_part_number, datasheet_url, product_url,
           sync_status, sku, last_updated, inventatory_id, created_at, machine_code,
           rack_id, rack_slot, rack_assignment,
           label_override, vendor_provider, vendor_product_number, vendor_manufacturer_part_number,
           vendor_category_id, vendor_category_path, vendor_title, vendor_detailed_description,
           vendor_parameters, vendor_product_url, vendor_locale
    FROM inventatory_items
    ORDER BY part_name COLLATE NOCASE ASC
  )SQL";

  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    InventoryItem item;
    item.id = sqliteText(statement.stmt, 0);
    item.partName = sqliteText(statement.stmt, 1);
    item.manufacturer = sqliteText(statement.stmt, 2);
    item.category = sqliteText(statement.stmt, 3);
    if (!sqliteInt32(statement.stmt, 4, item.quantity) || !sqliteInt32(statement.stmt, 5, item.reorderThreshold)) {
      return false;
    }
    item.location = sqliteText(statement.stmt, 6);
    item.tags = deserializeTagsFromStorage(sqliteText(statement.stmt, 7));
    item.parameters = deserializeParametersFromStorage(sqliteText(statement.stmt, 8));
    item.notes = sqliteText(statement.stmt, 9);
    item.digikeyPartNumber = sqliteText(statement.stmt, 10);
    item.datasheetUrl = sqliteText(statement.stmt, 11);
    item.productUrl = sqliteText(statement.stmt, 12);
    item.syncStatus = sqliteText(statement.stmt, 13);
    item.sku = sqliteText(statement.stmt, 14);
    if (!sqliteTime(statement.stmt, 15, item.lastUpdated)) return false;
    item.inventatoryId = sqliteText(statement.stmt, 16);
    if (!sqliteTime(statement.stmt, 17, item.createdAt)) return false;
    item.machineCode = sqliteText(statement.stmt, 18);
    item.rackId = sqliteText(statement.stmt, 19);
    item.rackSlot = sqliteText(statement.stmt, 20);
    item.rackAssignment = parseRackAssignmentMode(sqliteText(statement.stmt, 21));
    item.labelOverride = sqliteText(statement.stmt, 22);
    item.vendorMetadata.provider = sqliteText(statement.stmt, 23);
    item.vendorMetadata.providerProductNumber = sqliteText(statement.stmt, 24);
    item.vendorMetadata.manufacturerPartNumber = sqliteText(statement.stmt, 25);
    item.vendorMetadata.categoryId = sqliteText(statement.stmt, 26);
    item.vendorMetadata.categoryPath = deserializeTagsFromStorage(sqliteText(statement.stmt, 27));
    item.vendorMetadata.title = sqliteText(statement.stmt, 28);
    item.vendorMetadata.detailedDescription = sqliteText(statement.stmt, 29);
    item.vendorMetadata.parameters = deserializeParametersFromStorage(sqliteText(statement.stmt, 30));
    item.vendorMetadata.productUrl = sqliteText(statement.stmt, 31);
    item.vendorMetadata.locale = sqliteText(statement.stmt, 32);
    items.push_back(move(item));
  }

  return stepResult == SQLITE_DONE;
}

bool writeInventoryMovements(SqliteConnection& connection, const vector<InventoryMovement>& movements) {
  if (movements.empty()) return true;

  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT INTO inventatory_stock_movements (
      movement_id, item_id, item_name, source, reference,
      quantity_before, delta, quantity_after, occurred_at
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  for (const auto& movement : movements) {
    if (movement.delta == 0) continue;
    const auto movementId = movement.id.empty() ? makeId() : movement.id;
    sqliteApi().bind_text(statement.stmt, 1, movementId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 2, movement.itemId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 3, movement.itemName.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 4, movement.source.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 5, movement.reference.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int(statement.stmt, 6, movement.quantityBefore);
    sqliteApi().bind_int(statement.stmt, 7, movement.delta);
    sqliteApi().bind_int(statement.stmt, 8, movement.quantityAfter);
    sqliteApi().bind_int64(statement.stmt, 9, static_cast<sqlite3_int64>(movement.occurredAt));
    if (sqliteApi().step(statement.stmt) != SQLITE_DONE) {
      return false;
    }
    sqliteApi().reset(statement.stmt);
    sqliteApi().clear_bindings(statement.stmt);
  }
  return true;
}

bool writeItemsToInventatoryTable(SqliteConnection& connection, const vector<InventoryItem>& items,
                           const vector<InventatoryRack>& racks, const DeviceEventCommit* deviceEvent = nullptr,
                           const vector<InventoryMovement>* movements = nullptr,
                           const InventoryCommitDraft* commitDraft = nullptr,
                           InventoryCommit* committed = nullptr, bool ensureInitialHistory = false) {
  if (!ensureInventatoryTableSchema(connection)) {
    return false;
  }
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
      tags, parameters, notes, digikey_part_number, datasheet_url, product_url, sync_status, sku,
      last_updated, inventatory_id, created_at, machine_code,
      rack_id, rack_slot, rack_assignment,
      label_override, vendor_provider, vendor_product_number, vendor_manufacturer_part_number,
      vendor_category_id, vendor_category_path, vendor_title, vendor_detailed_description,
      vendor_parameters, vendor_product_url, vendor_locale
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
    sqliteApi().bind_text(statement.stmt, 17, item.inventatoryId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_int64(statement.stmt, 18, static_cast<sqlite3_int64>(item.createdAt));
    sqliteApi().bind_text(statement.stmt, 19, item.machineCode.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 20, item.rackId.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 21, item.rackSlot.c_str(), -1, SQLITE_TRANSIENT);
    const auto assignment = rackAssignmentModeName(item.rackAssignment);
    sqliteApi().bind_text(statement.stmt, 22, assignment.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 23, item.labelOverride.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 24, item.vendorMetadata.provider.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 25, item.vendorMetadata.providerProductNumber.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 26, item.vendorMetadata.manufacturerPartNumber.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 27, item.vendorMetadata.categoryId.c_str(), -1, SQLITE_TRANSIENT);
    const auto vendorCategoryPath = serializeTagsForStorage(item.vendorMetadata.categoryPath);
    sqliteApi().bind_text(statement.stmt, 28, vendorCategoryPath.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 29, item.vendorMetadata.title.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 30, item.vendorMetadata.detailedDescription.c_str(), -1, SQLITE_TRANSIENT);
    const auto vendorParameters = serializeParametersForStorage(item.vendorMetadata.parameters);
    sqliteApi().bind_text(statement.stmt, 31, vendorParameters.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 32, item.vendorMetadata.productUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqliteApi().bind_text(statement.stmt, 33, item.vendorMetadata.locale.c_str(), -1, SQLITE_TRANSIENT);

    if (sqliteApi().step(statement.stmt) != SQLITE_DONE) {
      execSql(connection, "ROLLBACK");
      return false;
    }

    sqliteApi().reset(statement.stmt);
    sqliteApi().clear_bindings(statement.stmt);
  }

  if (movements != nullptr && !writeInventoryMovements(connection, *movements)) {
    execSql(connection, "ROLLBACK");
    return false;
  }

  if (deviceEvent != nullptr) {
    SqliteStatement eventStatement;
    const char* eventSql = R"SQL(
      UPDATE inventatory_device_events SET
        state='completed', result_id=?, result_status=?, result_existing=?, result_item_name=?,
        result_requested_delta=?, result_applied_delta=?, result_quantity=?, result_location=?,
        result_code=?, result_message=?, completed_at=?
      WHERE event_id=? AND device_id=? AND state='received'
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
    sqliteApi().bind_text(eventStatement.stmt, 13, deviceEvent->deviceId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqliteApi().step(eventStatement.stmt) != SQLITE_DONE || sqliteApi().changes(connection.db) != 1) {
      execSql(connection, "ROLLBACK");
      return false;
    }
  }

  if (ensureInitialHistory) {
    SqliteStatement countStatement;
    if (sqliteApi().prepare_v2(connection.db, "SELECT COUNT(*) FROM inventatory_inventory_commits", -1,
                               &countStatement.stmt, nullptr) != SQLITE_OK ||
        sqliteApi().step(countStatement.stmt) != SQLITE_ROW ||
        sqliteApi().column_type(countStatement.stmt, 0) != SQLITE_INTEGER) {
      execSql(connection, "ROLLBACK");
      return false;
    }
    if (sqliteApi().column_int64(countStatement.stmt, 0) == 0) {
      InventoryCommitDraft initialDraft;
      initialDraft.source = "system";
      initialDraft.message = "Initial inventory";
      InventoryCommit initialCommit;
      if (!writeInventoryCommit(connection, items, racks, initialDraft, initialCommit)) {
        execSql(connection, "ROLLBACK");
        return false;
      }
    }
  }

  if (commitDraft != nullptr) {
    InventoryCommit created;
    if (!writeInventoryCommit(connection, items, racks, *commitDraft, created)) {
      execSql(connection, "ROLLBACK");
      return false;
    }
    if (committed != nullptr) *committed = move(created);
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
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  if (!ensureInventatoryTableSchema(connection)) {
    return false;
  }
  if (!validateInventoryCommitHistory(connection, nullptr)) {
    return false;
  }
  vector<InventatoryRack> loadedRacks;
  if (!loadRacks(connection, loadedRacks)) {
    return false;
  }

  vector<InventoryItem> inventatoryItems;
  if (!loadItemsFromInventatoryTable(connection, inventatoryItems)) return false;

  InventoryStore loadedStore;
  loadedStore.items() = move(inventatoryItems);
  loadedStore.racks() = move(loadedRacks);

  ensureInventoryIdentifiers(loadedStore.items());
  if (!validateInventoryIdentifiers(loadedStore.items(), loadedStore.racks())) return false;
  reconcileRackAssignments(loadedStore);
  items_ = move(loadedStore.items());
  racks_ = move(loadedStore.racks());
  return true;
#else
  vector<InventoryItem> loadedItems;
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
      loadedItems.push_back(move(item));
    }
  }

  ensureInventoryIdentifiers(loadedItems);
  if (loadedItems.empty()) return false;
  items_ = move(loadedItems);
  racks_.clear();
  return true;
#endif
}

bool InventoryStore::save(const filesystem::path& path) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;

  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  return writeItemsToInventatoryTable(connection, items, racks_);
#else
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;

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

bool InventoryStore::saveWithMovements(const filesystem::path& path,
                                       const vector<InventoryMovement>& movements) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;
  SqliteConnection connection;
  if (!openDatabase(path, connection)) return false;
  return writeItemsToInventatoryTable(connection, items, racks_, nullptr, &movements);
#else
  (void)movements;
  return save(path);
#endif
}

bool InventoryStore::saveWithDeviceEvent(const filesystem::path& path, const DeviceEventCommit& event,
                                          const vector<InventoryMovement>& movements) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;
  SqliteConnection connection;
  if (!openDatabase(path, connection)) return false;
  return writeItemsToInventatoryTable(connection, items, racks_, &event, &movements);
#else
  (void)event;
  (void)movements;
  return save(path);
#endif
}

bool InventoryStore::saveWithCommit(const filesystem::path& path, const InventoryStore& previous,
                                    const InventoryCommitDraft& draft, const vector<InventoryMovement>& movements,
                                    const DeviceEventCommit* deviceEvent, InventoryCommit* committed) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  InventoryStore normalized;
  normalized.items() = items;
  normalized.racks() = racks_;
  if (!validateInventoryIdentifiers(normalized.items(), normalized.racks())) return false;
  InventoryStore normalizedPrevious = previous;
  ensureInventoryIdentifiers(normalizedPrevious.items());
  if (!validateInventoryIdentifiers(normalizedPrevious.items(), normalizedPrevious.racks())) return false;
  const auto changes = inventoryCommitDiff(normalizedPrevious, normalized);
  InventoryCommitDraft enriched = draft;
  enriched.changedItemCount = 0;
  enriched.changedRackCount = 0;
  unordered_set<string> changedItems;
  unordered_set<string> changedRacks;
  for (const auto& change : changes) {
    if (change.entityType == "item") changedItems.insert(change.entityId);
    if (change.entityType == "rack") changedRacks.insert(change.entityId);
  }
  enriched.changedItemCount = changedItems.size();
  enriched.changedRackCount = changedRacks.size();
  if (trim(enriched.message).empty()) {
    string operation = "Updated inventory";
    if (enriched.source == "import") operation = "Imported inventory";
    else if (enriched.source == "stocktake") operation = "Completed stocktake";
    else if (enriched.source == "bom_build") operation = "Built project inventory";
    else if (enriched.source == "scanner") operation = "Recorded scanner event";
    else if (enriched.source == "digikey") operation = "Applied DigiKey enrichment";
    else if (enriched.source == "undo") operation = "Undid inventory commit";
    else if (enriched.source == "revert") operation = "Corrected inventory history";
    else if (enriched.source == "checkpoint") operation = "Inventory checkpoint";
    enriched.message = operation;
    if (!enriched.reference.empty()) enriched.message += " [" + enriched.reference + "]";
    enriched.message += " · " + to_string(enriched.changedItemCount) +
                        (enriched.changedItemCount == 1 ? " part, " : " parts, ") +
                        to_string(enriched.changedRackCount) +
                        (enriched.changedRackCount == 1 ? " rack" : " racks");
  }

  const bool shouldCommit = !changes.empty() || draft.checkpoint;
  SqliteConnection connection;
  if (!openDatabase(path, connection)) return false;
  return writeItemsToInventatoryTable(connection, items, racks_, deviceEvent, &movements,
                                      shouldCommit ? &enriched : nullptr, committed, true);
#else
  (void)previous;
  (void)draft;
  (void)movements;
  (void)deviceEvent;
  (void)committed;
  return save(path);
#endif
}

vector<InventoryMovement> loadInventoryMovements(const filesystem::path& path, size_t limit) {
#ifdef _WIN32
  if (limit == 0) return {};
  error_code filesystemError;
  if (!filesystem::is_regular_file(path, filesystemError) || filesystemError) return {};
  SqliteConnection connection;
  if (!openDatabase(path, connection) || !tableExists(connection, "inventatory_stock_movements")) return {};

  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT movement_id, item_id, item_name, source, reference,
           quantity_before, delta, quantity_after, occurred_at
    FROM inventatory_stock_movements
    ORDER BY occurred_at DESC, movement_id DESC
    LIMIT ?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return {};
  const auto boundedLimit = min(limit, static_cast<size_t>(numeric_limits<int>::max()));
  sqliteApi().bind_int(statement.stmt, 1, static_cast<int>(boundedLimit));

  vector<InventoryMovement> movements;
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    InventoryMovement movement;
    movement.id = sqliteText(statement.stmt, 0);
    movement.itemId = sqliteText(statement.stmt, 1);
    movement.itemName = sqliteText(statement.stmt, 2);
    movement.source = sqliteText(statement.stmt, 3);
    movement.reference = sqliteText(statement.stmt, 4);
    if (!sqliteInt32(statement.stmt, 5, movement.quantityBefore) || !sqliteInt32(statement.stmt, 6, movement.delta) ||
        !sqliteInt32(statement.stmt, 7, movement.quantityAfter) || !sqliteTime(statement.stmt, 8, movement.occurredAt)) {
      return {};
    }
    movements.push_back(move(movement));
  }
  return stepResult == SQLITE_DONE ? movements : vector<InventoryMovement>();
#else
  (void)path;
  (void)limit;
  return {};
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
    return toLower(item.id) == needle || toLower(item.inventatoryId) == needle || toLower(item.sku) == needle ||
           toLower(item.machineCode) == needle || toLower(item.digikeyPartNumber) == needle ||
           containsInsensitive(item.productUrl, needle) || containsInsensitive(item.datasheetUrl, needle);
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findByCode(const string& code) const {
  const auto needle = toLower(trim(code));
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return toLower(item.id) == needle || toLower(item.inventatoryId) == needle || toLower(item.sku) == needle ||
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

}  // namespace inventatory
