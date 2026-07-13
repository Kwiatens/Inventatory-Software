// HIMS - Hardware Inventory Management System
// Shared short part description classifier.

#pragma once

#include "core/Inventory.h"

#include <string>

namespace hims {

struct PartDescriptor {
  string shortDescription;
};

PartDescriptor describePart(const InventoryItem& item);
string partShortDescription(const InventoryItem& item);

}  // namespace hims
