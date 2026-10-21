// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/bom/BomMouserSuggestion.h"

#include "core/bom/BomMatch.h"
#include "core/bom/BomMatchPrivate.h"
#include "core/inventory/VendorProvider.h"
#include "platform/mouser/MouserApi.h"

namespace inventatory {

using namespace std;
using bom_match_detail::compactKey;

string bomMouserQuery(const BomLine& line) {
  // A supplier part number is a Mouser number only on a line that names Mouser.
  const auto supplierPart = trim(line.supplierPartNumber);
  if (!supplierPart.empty() && compactKey(line.supplier) == compactKey(kVendorProviderMouser)) return supplierPart;
  if (const auto manufacturerPart = trim(line.manufacturerPartNumber); !manufacturerPart.empty()) {
    return manufacturerPart;
  }
  // KiCad puts the part name in the designation, never a value such as "100nF".
  if (looksLikePartNumber(line.designation)) return trim(line.designation);
  return {};
}

BomMouserSuggestion bomMouserSuggestion(const BomLine& line, const MouserProductDetails& product) {
  BomMouserSuggestion answer;
  const auto partNumber = trim(product.vendorMetadata.providerProductNumber);
  // Never suggest a part Mouser cannot sell, however well it fits.
  if (!product.orderable || partNumber.empty()) {
    answer.outcome = BomLookupOutcome::NoMatch;
    return answer;
  }
  // Judged like a stock item: by part number, or by value and package.
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
