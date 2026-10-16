// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Picks the BOM dialect for a file's text and parses it into the shared BOM shape.

#pragma once

#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

// EasyEDA exports go to the EasyEDA parser, and Altium Designer exports to the Altium parser. An Altium export is
// recognised only by headers that KiCad does not write, so everything else keeps the KiCad parser, which is what
// stored projects were always read with, so their analysis does not change.
KicadBomFile parseBomText(const string& text, const string& projectName);

}  // namespace inventatory
