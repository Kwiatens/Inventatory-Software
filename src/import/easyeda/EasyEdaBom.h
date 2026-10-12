// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// EasyEDA BOM export parsing (Standard and Pro CSV exports).

#pragma once

#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

// Parses an EasyEDA BOM export: tab or comma delimited, one row per part, with Name, Designator and
// Footprint columns, and optionally Quantity, Supplier, Supplier Part and Manufacturer Part. The
// result has the same shape as a KiCad BOM so the rest of the BOM workflow is shared. A file that
// cannot be read without guessing (a repeated designator, a row with no name or designator, a quantity
// that is not a whole number) is refused with `ok = false` rather than imported partly.
KicadBomFile parseEasyEdaBomText(const string& text, const string& projectName);

}  // namespace inventatory
