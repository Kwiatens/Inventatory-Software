// Inventatory - Hardware Inventory Management System
// Quantity movement diffing for the append-only stock ledger.

#include "core/Inventory.h"

#include <ctime>
#include <unordered_map>

namespace inventatory {

using namespace std;

vector<InventoryMovement> inventoryMovementDiff(const InventoryStore& before, const InventoryStore& after,
                                               const string& source, const string& reference, time_t occurredAt) {
  const auto timestamp = occurredAt == 0 ? time(nullptr) : occurredAt;
  const auto movementSource = trim(source).empty() ? string("manual") : trim(source);
  unordered_map<string, const InventoryItem*> beforeById;
  beforeById.reserve(before.items().size());
  for (const auto& item : before.items()) {
    if (!item.id.empty()) beforeById[item.id] = &item;
  }

  unordered_map<string, bool> seen;
  seen.reserve(before.items().size() + after.items().size());
  vector<InventoryMovement> movements;
  movements.reserve(before.items().size() + after.items().size());

  auto appendMovement = [&](const InventoryItem& item, int quantityBefore, int quantityAfter) {
    const int delta = quantityAfter - quantityBefore;
    if (delta == 0) return;
    InventoryMovement movement;
    movement.id = makeId();
    movement.itemId = item.id;
    movement.itemName = item.partName;
    movement.source = movementSource;
    movement.reference = reference;
    movement.quantityBefore = quantityBefore;
    movement.delta = delta;
    movement.quantityAfter = quantityAfter;
    movement.occurredAt = timestamp;
    movements.push_back(move(movement));
  };

  for (const auto& item : after.items()) {
    if (item.id.empty()) continue;
    seen[item.id] = true;
    const auto beforeIt = beforeById.find(item.id);
    if (beforeIt == beforeById.end()) {
      appendMovement(item, 0, item.quantity);
    } else {
      appendMovement(item, beforeIt->second->quantity, item.quantity);
    }
  }

  for (const auto& item : before.items()) {
    if (item.id.empty() || seen.find(item.id) != seen.end()) continue;
    appendMovement(item, item.quantity, 0);
  }
  return movements;
}

}  // namespace inventatory
