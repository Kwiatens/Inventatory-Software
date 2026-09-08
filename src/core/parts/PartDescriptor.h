// Inventatory - Hardware Inventory Management System
// Shared short part description classifier.

#pragma once

#include "core/inventory/Inventory.h"

#include <string>

namespace inventatory {

enum class PartLabelSource {
  ManualOverride,
  VendorCategory,
  VendorRule,
  ParentFamily,
  Fallback,
};

struct PartDescriptor {
  string purposeLabel;
  string printLabel;
  PartLabelSource source = PartLabelSource::Fallback;
  bool usedFallback = true;
  string shortDescription;
};

PartDescriptor describePart(const InventoryItem& item);
string partShortDescription(const InventoryItem& item);
string partPurposeLabel(const InventoryItem& item);

}  // namespace inventatory
