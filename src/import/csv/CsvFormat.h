// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Header-only sniffing that tells the importer which CSV dialect arrived.

#pragma once

#include <string>
#include <vector>

namespace inventatory {

using std::string;
using std::vector;

enum class CsvFormat {
  Unknown,
  DigiKeyOrder,
  KicadBom,
  EasyEdaBom,
  // Designator and footprint columns, but no value column the BOM importer can read.
  BomMissingValue,
};

// Reads only the header row, so detection stays cheap and never depends on
// whether the file happens to contain valid product lines.
CsvFormat detectCsvFormat(const string& text);

// True when the header row is an Altium Designer Bill of Materials. Shared by the format detector and the BOM
// dispatch, so a file is routed to the Altium parser exactly when the detector called it a BOM.
bool hasAltiumBomHeaders(const vector<string>& headers);

}  // namespace inventatory
