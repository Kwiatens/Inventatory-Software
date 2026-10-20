// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Inventatory - three-way merge of staged inventory edits onto the live inventory.
//
// A form or CSV review can stay open for minutes while scanner commits continue. Staged work is
// replayed as a delta, matched by item id, so committed changes are not reverted.
// Pure helpers: no UI, no I/O.

#include "core/inventory/Inventory.h"

#include <string>
#include <vector>

namespace inventatory {

enum class QuantityMerge {
  // A quantity the user typed; replaces a concurrent change and reports it in the notices.
  PreferEdited,
  // Stock added on top of the snapshot (CSV import); the delta is applied to the live quantity.
  ApplyDelta,
};

// Merges one item: `base` is the item when the work started, `edited` the staged copy and
// `current` the live one. Unchanged fields take the live value and changed fields win.
// A field changed on both sides is reported in `notices`, with the user's value kept.
// `id` comes from `edited`.
InventoryItem mergeEditedItem(const InventoryItem& base, const InventoryItem& edited,
                              const InventoryItem& current, QuantityMerge quantity,
                              std::vector<std::string>* notices = nullptr);

// Call after merging an item into `store`. Clears a placement that names a missing rack or a slot
// another part took meanwhile, since keeping it would fail every save. Item becomes automatic
// and a notice is added. Returns true when cleared.
bool releaseConflictingRackPlacement(const InventoryStore& store, InventoryItem& item,
                                     std::vector<std::string>* notices = nullptr);

// Replays changes between `base` and `staged` onto `current`, matching items by id. Staged
// additions are appended, edits merge field by field, and removals apply only to items unchanged
// in `current`. Rack edits and removals are not replayed. Problems go to `notices`.
InventoryStore mergeInventoryChanges(const InventoryStore& base, const InventoryStore& staged,
                                     const InventoryStore& current, QuantityMerge quantity,
                                     std::vector<std::string>* notices = nullptr);

}  // namespace inventatory
