// Inventatory - Hardware Inventory Management System
// Shared access to reviewed catalogue short part descriptions.

#include "core/PartDescriptor.h"

namespace inventatory {

PartDescriptor describePart(const InventoryItem& item) {
  if (!trim(item.cataloguePurposeLabel).empty()) return {trim(item.cataloguePurposeLabel)};
  return {"Not in catalogue"};
}

string partShortDescription(const InventoryItem& item) {
  return describePart(item).shortDescription;
}

}  // namespace inventatory
