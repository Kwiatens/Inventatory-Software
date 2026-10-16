// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// How Mouser is asked about a BOM line, and how its record becomes the line's suggestion.

#pragma once

#include "core/bom/BomProjectStore.h"
#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

struct MouserProductDetails;

// The part number Mouser is asked about for a BOM line: the line's supplier part number when the line names Mouser as
// its supplier, else its manufacturer part number, else its designation when that reads as a part number. Empty when
// the line names no part number. Mouser keeps only exact part number matches, so a value and package could never
// match and would only spend the request quota.
string bomMouserQuery(const BomLine& line);

// A Mouser record offered for a BOM line.
struct BomMouserSuggestion {
  BomLookupOutcome outcome = BomLookupOutcome::Failed;
  string suggestion;      // the Mouser part number
  bool verified = false;  // the record fits the line by the rules stock must pass
};

// What a BOM line is given from the Mouser record found for it. A record Mouser marks as not orderable, or one without
// a Mouser part number, is never suggested: NoMatch. Otherwise its Mouser part number is suggested (Found), verified
// when the record fits the line by its part number, or by its value and package.
BomMouserSuggestion bomMouserSuggestion(const BomLine& line, const MouserProductDetails& product);

}  // namespace inventatory
