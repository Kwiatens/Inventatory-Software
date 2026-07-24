// Inventatory - Hardware Inventory Management System
// Shared short part description classifier.

#pragma once

#include "core/Inventory.h"

#include <string>

namespace inventatory {

struct PartDescriptor {
  string shortDescription;
};

PartDescriptor describePart(const InventoryItem& item);
string partShortDescription(const InventoryItem& item);

}  // namespace inventatory
