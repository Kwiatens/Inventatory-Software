// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Label text and electrical-value formatting helpers.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "core/parts/PartDescriptor.h"
#include "core/parts/PhysicalValue.h"
#include "ui/shared/AppUiShared.h"

namespace inventatory {

using namespace std;

namespace label_printer_detail {

optional<string> parameterValueMatching(const InventoryItem& item, initializer_list<const char*> names,
                                        bool (*predicate)(const string&)) {
  for (const auto* name : names) {
    for (const auto& parameter : item.parameters) {
      if (!parameterLabelMatches(parameter.name, name)) {
        continue;
      }
      const auto value = trim(parameter.value);
      if (!value.empty() && !looksLikePackagingValue(value) && predicate(value)) {
        return value;
      }
    }
  }
  return nullopt;
}

optional<string> firstParameter(const InventoryItem& item, initializer_list<const char*> names) {
  return parameterValue(item, names);
}

optional<string> firstInductanceParameter(const InventoryItem& item) {
  if (const auto value =
          parameterValueMatching(item, {"Inductance", "Value"}, value_text::looksLikeInductanceValue)) {
    return value;
  }
  return value_text::extractInductance(item.notes + " " + item.partName + " " + item.sku);
}

bool itemTextContains(const InventoryItem& item, initializer_list<const char*> needles) {
  const auto textMatches = [&](const string& text) {
    for (const auto* needle : needles) {
      if (containsInsensitive(text, needle)) {
        return true;
      }
    }
    return false;
  };

  // The vendor's description says what the part is. A parameter's value does not: a 555 timer's "Type" reads
  // "Timer/Oscillator", which must not make it a crystal.
  if (textMatches(item.category) || textMatches(displayCategory(item.category)) || textMatches(item.partName) ||
      textMatches(item.manufacturer) || textMatches(item.location) || textMatches(item.notes) ||
      textMatches(item.digikeyPartNumber) || textMatches(item.sku) || textMatches(item.vendorMetadata.title)) {
    return true;
  }

  for (const auto& tag : item.tags) {
    if (textMatches(tag)) {
      return true;
    }
  }

  for (const auto& parameter : item.parameters) {
    if (textMatches(parameter.name)) {
      return true;
    }
  }

  return false;
}

bool hasParameter(const InventoryItem& item, initializer_list<const char*> names) {
  return findParameter(item.parameters, names) != nullptr;
}

}  // namespace label_printer_detail
}  // namespace inventatory
