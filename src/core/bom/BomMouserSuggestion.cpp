// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// How Mouser is asked about a BOM line, and how its record becomes the line's suggestion.

#include "core/bom/BomMouserSuggestion.h"

#include "core/bom/BomMatch.h"
#include "core/bom/BomMatchPrivate.h"
#include "core/inventory/VendorProvider.h"
#include "platform/mouser/MouserApi.h"

namespace inventatory {

using namespace std;
using bom_match_detail::compactKey;

string bomMouserQuery(const BomLine& line) {
  // The supplier part number is a Mouser number only on a line that names Mouser; an LCSC or DigiKey number means
  // nothing to Mouser.
  const auto supplierPart = trim(line.supplierPartNumber);
  if (!supplierPart.empty() && compactKey(line.supplier) == compactKey(kVendorProviderMouser)) return supplierPart;
  if (const auto manufacturerPart = trim(line.manufacturerPartNumber); !manufacturerPart.empty()) {
    return manufacturerPart;
  }
  // A KiCad line names its part in the designation ("LM358"), never a value such as "100nF".
  if (looksLikePartNumber(line.designation)) return trim(line.designation);
  return {};
}

BomMouserSuggestion bomMouserSuggestion(const BomLine& line, const MouserProductDetails& product) {
  BomMouserSuggestion answer;
  const auto partNumber = trim(product.vendorMetadata.providerProductNumber);
  // A part Mouser cannot sell is never suggested for a line, however well it fits. Without a Mouser part number there
  // is nothing to order.
  if (!product.orderable || partNumber.empty()) {
    answer.outcome = BomLookupOutcome::NoMatch;
    return answer;
  }
  // The record is judged the same way a stock item is: by its part number, or by its value and package. The package
  // is among the parameters, as the "Package / Case" attribute. The Mouser record is kept on the probe, so a line that
  // names a Mouser part number is verified by that number.
  InventoryItem probe;
  probe.partName = product.productDescription;
  probe.sku = product.vendorMetadata.manufacturerPartNumber;
  probe.parameters = product.parameters;
  probe.vendorMetadata = product.vendorMetadata;
  answer.outcome = BomLookupOutcome::Found;
  answer.suggestion = partNumber;
  answer.verified = bomSuggestionFitsLine(line, probe);
  return answer;
}

}  // namespace inventatory
