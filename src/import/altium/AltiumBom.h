// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Altium Designer BOM export parsing (a Bill of Materials report saved as CSV).

#pragma once

#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

// Parses a Bill of Materials report exported from Altium Designer as CSV. The user chooses the report's columns,
// so every column is found by its header name and never by its position. A row has Designator and a value
// (Comment, or Description when there is no Comment column), and optionally Footprint, Quantity and the supplier
// and manufacturer part columns. The result has the same shape as a KiCad BOM so the rest of the BOM workflow is
// shared. A file that cannot be read without guessing (a repeated designator, a row with no designator or no
// value, a quantity that is not a whole number) is refused with `ok = false` rather than imported partly.
//
// The header names are matched against a list of plausible Altium names that has not been checked against a real
// Altium export. parseBomText chooses this parser; see import/bom/BomImport.h.
KicadBomFile parseAltiumBomText(const string& text, const string& projectName);

}  // namespace inventatory
