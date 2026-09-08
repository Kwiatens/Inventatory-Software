// Inventatory - internal SQLite storage operations used by InventoryStore.

#pragma once

#include "core/Inventory.h"
#include "core/InventorySqlite.h"

namespace inventatory {

#ifdef _WIN32

bool ensureInventatoryTableSchema(SqliteConnection& connection);
bool loadRacks(SqliteConnection& connection, std::vector<InventatoryRack>& racks);
bool loadItemsFromInventatoryTable(SqliteConnection& connection, std::vector<InventoryItem>& items);
bool writeInventoryMovements(SqliteConnection& connection, const std::vector<InventoryMovement>& movements);
bool writeItemsToInventatoryTable(
    SqliteConnection& connection, const std::vector<InventoryItem>& items,
    const std::vector<InventatoryRack>& racks, const DeviceEventCommit* deviceEvent = nullptr,
    const std::vector<InventoryMovement>* movements = nullptr,
    const InventoryCommitDraft* commitDraft = nullptr, InventoryCommit* committed = nullptr,
    bool ensureInitialHistory = false, const std::vector<InventoryItem>* initialItems = nullptr,
    const std::vector<InventatoryRack>* initialRacks = nullptr);

#endif

}  // namespace inventatory
