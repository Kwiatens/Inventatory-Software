// Inventatory - SQLite row loading and transactional inventory writes.

#include "core/storage/InventoryStorageInternal.h"
#include "core/inventory/InventoryInternals.h"
#include "core/history/InventoryVersionInternal.h"

#include <utility>

namespace inventatory {

using namespace std;

#ifdef _WIN32

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
                           const vector<InventatoryRack>& racks, const DeviceEventCommit* deviceEvent,
                           const vector<InventoryMovement>* movements,
                           const InventoryCommitDraft* commitDraft,
                           InventoryCommit* committed, bool ensureInitialHistory,
                           const vector<InventoryItem>* initialItems,
                           const vector<InventatoryRack>* initialRacks) {
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
      const auto& baselineItems = initialItems == nullptr ? items : *initialItems;
      const auto& baselineRacks = initialRacks == nullptr ? racks : *initialRacks;
      if (!writeInventoryCommit(connection, baselineItems, baselineRacks, initialDraft, initialCommit)) {
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

}  // namespace inventatory
