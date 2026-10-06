// Inventatory - Hardware Inventory Management System
// Internal SQLite helpers for persistent inventory commits.

#pragma once

#include "core/inventory/Inventory.h"
#include "core/storage/InventorySqlite.h"

namespace inventatory {

// Shared by the SQLite commit writer and the commit-diff/reversal unit.
std::string serializeRackSnapshot(const InventatoryRack& rack);
// True when the items and racks form a snapshot that history validation accepts: valid identifiers,
// timestamps, and rack assignments that name an existing rack, a valid slot, and a slot used once.
// On failure `error` (when given) receives the reason.
bool validateSnapshotSemantics(const InventoryStore& snapshot, std::string* error);

#ifdef INVENTATORY_SQLITE_STORAGE

bool ensureInventoryCommitSchema(SqliteConnection& connection);
// Appends a commit holding the given snapshot inside the caller's transaction. The change counts
// are recomputed here from the parent commit's stored snapshot, so they match what history
// validation checks whatever diff baseline the caller used. When the snapshot equals its parent
// and the draft is not a checkpoint nothing is written and `committed` stays empty.
bool writeInventoryCommit(SqliteConnection& connection, const vector<InventoryItem>& items,
                          const vector<InventatoryRack>& racks, const InventoryCommitDraft& draft,
                          InventoryCommit& committed);
bool validateInventoryCommitHistory(SqliteConnection& connection, string* error = nullptr);
// Reads one stored commit snapshot (items and racks) and validates it.
bool readInventoryCommitSnapshot(SqliteConnection& connection, const string& id, InventoryStore& snapshot);

#endif

}  // namespace inventatory
