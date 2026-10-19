// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Picks the BOM dialect for a file's text and parses it into the shared BOM shape.

#pragma once

#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

// Altium exports are recognised only by headers KiCad does not write. Everything else stays on the
// KiCad parser, so stored projects keep the same analysis.
KicadBomFile parseBomText(const string& text, const string& projectName);

}  // namespace inventatory
