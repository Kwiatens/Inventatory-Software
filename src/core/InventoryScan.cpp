// Inventatory - Hardware Inventory Management System
// Scan code resolution for local and standards-based component flows.

#include "core/InventoryInternals.h"

#include <utility>

namespace inventatory {

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

  if (toLower(code).rfind("inventatory:", 0) == 0) {
    return {false, false, {}, "Unknown Inventatory ID"};
  }

  if (auto* existing = store.findByCode(code)) {
    return {true, false, existing->id, "Matched existing item"};
  }

  InventoryItem item;
  item.id = sanitizeIdPart(code) + "-" + makeId().substr(0, 8);
  item.partName = code;
  item.manufacturer = "Unknown";
  item.category = "Unsorted";
  item.quantity = 0;
  item.reorderThreshold = 0;
  item.location = "Scan Inbox";
  item.tags = {"scanned"};
  item.notes = "Created from a standards-based component scan.";
  item.catalogueStatus = "not_in_catalogue";
  item.manufacturerPartNumber = code;
  item.lastUpdated = nowEpoch();
  item.createdAt = item.lastUpdated;
  store.items().push_back(move(item));
  return {true, true, store.items().back().id, "Created a placeholder item from the scanned code"};
}

ScanResolution resolveDecodedComponent(InventoryStore& store, const string& manufacturer,
                                       const string& manufacturerPartNumber, const string& encodedPartName) {
  const auto mpn = trim(manufacturerPartNumber);
  const auto name = trim(encodedPartName);
  if (!mpn.empty()) {
    auto resolution = resolveScanCode(store, mpn);
    if (resolution.matched) {
      if (auto* item = store.findById(resolution.itemId)) {
        if (!trim(manufacturer).empty()) item->manufacturer = trim(manufacturer);
        item->manufacturerPartNumber = mpn;
        if (!name.empty() && (resolution.created || trim(item->partName).empty())) item->partName = name;
      }
    }
    return resolution;
  }
  if (name.empty()) return {false, false, {}, "Scan contains no usable component identity"};

  InventoryItem item;
  item.id = "unidentified-" + makeId().substr(0, 12);
  item.partName = name;
  item.manufacturer = trim(manufacturer);
  if (item.manufacturer.empty()) item.manufacturer = "Unknown";
  item.category = "Unsorted";
  item.location = "Scan Inbox";
  item.tags = {"scanned", "unidentifiable"};
  item.notes = "No usable manufacturer part number was encoded in the scan.";
  item.catalogueStatus = "not_in_catalogue";
  item.lastUpdated = nowEpoch();
  item.createdAt = item.lastUpdated;
  store.items().push_back(move(item));
  return {true, true, store.items().back().id, "Created an unidentifiable item from the encoded part name"};
}

}  // namespace inventatory
