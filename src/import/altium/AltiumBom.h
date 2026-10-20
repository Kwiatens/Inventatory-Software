// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Altium Designer BOM export parsing (a Bill of Materials report saved as CSV).

#pragma once

#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

// Parses an Altium Designer BOM report saved as CSV, finding columns by header name. Rows need
// a Designator and a value (Comment, or Description). Ambiguous input is refused with `ok = false`.
//
// The header aliases are unverified against a real Altium export. BomImport.h selects this parser.
KicadBomFile parseAltiumBomText(const string& text, const string& projectName);

}  // namespace inventatory
