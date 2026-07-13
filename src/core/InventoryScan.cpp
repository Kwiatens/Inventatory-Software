// HIMS - Hardware Inventory Management System
// Scan code resolution for local and DigiKey flows.

#include "core/InventoryInternals.h"

#include <utility>

namespace hims {

using namespace std;

ScanResolution resolveScanCode(InventoryStore& store, const string& rawCode) {
  const auto code = trim(rawCode);
  if (code.empty()) {
    return {false, false, {}, "Empty scan code"};
  }

  if (isMachineCode(code)) {
    if (auto* existing = store.findByMachineCode(code)) {
      return {true, false, existing->id, "Matched existing item"};
    }
    return {false, false, {}, "Unknown machine code"};
  }

  if (toLower(code).rfind("hims:", 0) == 0) {
    return {false, false, {}, "Unknown HIMS ID"};
  }

  if (auto* existing = store.findByCode(code)) {
    return {true, false, existing->id, "Matched existing item"};
  }

  InventoryItem item;
  item.id = sanitizeIdPart(code) + "-" + makeId().substr(0, 8);
  item.partName = "Scanned DigiKey Item";
  item.manufacturer = "Unknown";
  item.category = "Unsorted";
  item.quantity = 0;
  item.reorderThreshold = 0;
  item.location = "Scan Inbox";
  item.tags = {"scanned"};
  item.notes = "Created from a DigiKey code scan.";
  item.digikeyPartNumber = code;
  item.syncStatus = "needs_metadata";
  item.sku = code;
  item.lastUpdated = nowEpoch();
  item.createdAt = item.lastUpdated;
  store.items().push_back(move(item));
  return {true, true, store.items().back().id, "Created a placeholder item from the scanned code"};
}

}  // namespace hims
