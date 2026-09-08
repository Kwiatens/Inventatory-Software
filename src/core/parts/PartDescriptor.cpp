// Inventatory - Hardware Inventory Management System
// Deterministic, provider-backed part label resolution.

#include "core/parts/PartDescriptor.h"

#include "PartDescriptorPrivate.h"

namespace inventatory {

using namespace part_descriptor_detail;

PartDescriptor describePart(const InventoryItem& item) {
  if (!trim(item.labelOverride).empty()) {
    return resolved(item.labelOverride, item.labelOverride, PartLabelSource::ManualOverride);
  }

  const auto context = makeContext(item);
  if (auto descriptor = discreteSemiconductorRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = integratedCircuitRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = specificCategoryMapping(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = categoryMapping(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = parentCategoryRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = categoryAndParameterRule(context); !descriptor.printLabel.empty()) return descriptor;
  if (auto descriptor = parentFamily(context); !descriptor.printLabel.empty()) return descriptor;
  const auto fallback = fallbackCategory(context);
  return resolved(fallback, fallback, PartLabelSource::Fallback);
}

string partShortDescription(const InventoryItem& item) {
  return describePart(item).printLabel;
}

string partPurposeLabel(const InventoryItem& item) {
  return describePart(item).purposeLabel;
}

}  // namespace inventatory
