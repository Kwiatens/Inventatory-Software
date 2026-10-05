#pragma once

// Inventatory - three-way merge of staged inventory edits onto the live inventory.
//
// A form edit or CSV import review can stay open for minutes while the Scan R1 service keeps
// committing stock changes. Writing the staged copy back over the live store would silently
// revert those committed changes, so staged work is replayed as a delta instead: only what the
// user changed relative to the snapshot taken when the work started is applied, matched by item
// id, on top of whatever the live inventory holds now. The helpers are pure (no UI, no I/O) so
// they can be tested without the application shell.

#include "core/inventory/Inventory.h"

#include <string>
#include <vector>

namespace inventatory {

enum class QuantityMerge {
  // The staged quantity is an absolute value the user typed; it replaces a concurrent change
  // and the replaced scanner value is reported in the notices.
  PreferEdited,
  // The staged quantity is stock added on top of the snapshot (CSV import); the delta is applied
  // to the live quantity so a concurrent scanner change is kept.
  ApplyDelta,
};

// Merges one item. `base` is the item when the work started, `edited` the staged item and
// `current` the live item. Fields the user did not change keep the live value, fields the user
// changed win, and a field changed to a different value on both sides is reported in `notices`
// (user value kept). `lastUpdated` is the newer of the two. The identity (`id`) is taken from
// `edited`; callers match by id before calling.
InventoryItem mergeEditedItem(const InventoryItem& base, const InventoryItem& edited,
                              const InventoryItem& current, QuantityMerge quantity,
                              std::vector<std::string>* notices = nullptr);

// Replays the changes between `base` and `staged` onto `current` and returns the result:
// items are matched by id, so scanner quantity changes and scanner-created items in `current`
// survive. Staged additions are appended, staged edits are merged field by field, staged
// removals apply only to an item that was not changed in `current`, and an item that vanished
// from `current` is not resurrected. Racks created in `staged` are added unless the live
// inventory already has a rack with the same code; items pointing at a rack that could not be
// carried over fall back to automatic placement. Rack edits and removals are not part of any
// staged workflow and are not replayed. Anything that could not be merged cleanly is described
// in `notices`.
InventoryStore mergeInventoryChanges(const InventoryStore& base, const InventoryStore& staged,
                                     const InventoryStore& current, QuantityMerge quantity,
                                     std::vector<std::string>* notices = nullptr);

}  // namespace inventatory
