// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// EasyEDA BOM export parsing (Standard and Pro CSV exports).

#pragma once

#include "import/kicad/KicadBom.h"

#include <string>

namespace inventatory {

using std::string;

// Parses an EasyEDA BOM export (Standard or Pro CSV) with Name, Designator and Footprint columns.
// Ambiguous input (repeated designator, missing name, non-whole quantity) gives `ok = false`.
KicadBomFile parseEasyEdaBomText(const string& text, const string& projectName);

}  // namespace inventatory
