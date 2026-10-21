// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Internal SQLite helpers for persistent inventory commits.

#pragma once

#include "core/inventory/Inventory.h"
#include "core/storage/InventorySqlite.h"

namespace inventatory {

std::string serializeRackSnapshot(const InventatoryRack& rack);
// True when history validation accepts the snapshot: valid identifiers and timestamps, and rack
// slots that name an existing rack and are used once. `error`, when given, receives the reason.
bool validateSnapshotSemantics(const InventoryStore& snapshot, std::string* error);

bool ensureInventoryCommitSchema(SqliteConnection& connection);
// Appends a commit inside the caller's transaction, with change counts recomputed from the stored
// parent. Writes nothing and leaves `committed` empty when the snapshot equals its parent and the
// draft is not a checkpoint.
bool writeInventoryCommit(SqliteConnection& connection, const vector<InventoryItem>& items,
                          const vector<InventatoryRack>& racks, const InventoryCommitDraft& draft,
                          InventoryCommit& committed);
bool validateInventoryCommitHistory(SqliteConnection& connection, string* error = nullptr);
// Reads and validates one stored commit snapshot.
bool readInventoryCommitSnapshot(SqliteConnection& connection, const string& id, InventoryStore& snapshot);

}  // namespace inventatory
