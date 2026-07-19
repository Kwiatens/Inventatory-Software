// Inventatory - Hardware Inventory Management System
// Shared access to reviewed IECD short part descriptions.

#include "core/PartDescriptor.h"

namespace inventatory {

PartDescriptor describePart(const InventoryItem& item) {
  if (!trim(item.iecdPurposeLabel).empty()) return {trim(item.iecdPurposeLabel)};
  return {"Not in IECD"};
}

string partShortDescription(const InventoryItem& item) {
  return describePart(item).shortDescription;
}

}  // namespace inventatory
