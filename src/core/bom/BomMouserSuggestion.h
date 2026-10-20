// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System

#pragma once

#include "core/bom/BomProjectStore.h"
#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

struct MouserProductDetails;

// Mouser part number to query for a line, or empty. Mouser matches exact part numbers only, so a
// value or package would only spend request quota.
string bomMouserQuery(const BomLine& line);

// A Mouser record offered for a BOM line.
struct BomMouserSuggestion {
  BomLookupOutcome outcome = BomLookupOutcome::Failed;
  string suggestion;      // the Mouser part number
  bool verified = false;  // the record fits the line by the rules stock must pass
};

// Suggests the record's Mouser part number. Not orderable or no part number: NoMatch.
// Otherwise Found, and verified when the record fits the line.
BomMouserSuggestion bomMouserSuggestion(const BomLine& line, const MouserProductDetails& product);

}  // namespace inventatory
