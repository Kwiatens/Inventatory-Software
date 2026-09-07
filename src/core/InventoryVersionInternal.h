// Inventatory - Hardware Inventory Management System
// Internal SQLite helpers for persistent inventory commits.

#pragma once

#include "core/Inventory.h"
#include "core/InventorySqlite.h"

namespace inventatory {

#ifdef _WIN32

bool ensureInventoryCommitSchema(SqliteConnection& connection);
bool writeInventoryCommit(SqliteConnection& connection, const vector<InventoryItem>& items,
                          const vector<InventatoryRack>& racks, const InventoryCommitDraft& draft,
                          InventoryCommit& committed);
bool validateInventoryCommitHistory(SqliteConnection& connection, string* error = nullptr);

#endif

}  // namespace inventatory
