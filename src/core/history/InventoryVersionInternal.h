// Inventatory - Hardware Inventory Management System
// Internal SQLite helpers for persistent inventory commits.

#pragma once

#include "core/inventory/Inventory.h"
#include "core/storage/InventorySqlite.h"

namespace inventatory {

// Shared by the SQLite commit writer and the commit-diff/reversal unit.
std::string serializeRackSnapshot(const InventatoryRack& rack);

#ifdef _WIN32

bool ensureInventoryCommitSchema(SqliteConnection& connection);
bool writeInventoryCommit(SqliteConnection& connection, const vector<InventoryItem>& items,
                          const vector<InventatoryRack>& racks, const InventoryCommitDraft& draft,
                          InventoryCommit& committed);
bool validateInventoryCommitHistory(SqliteConnection& connection, string* error = nullptr);

#endif

}  // namespace inventatory
