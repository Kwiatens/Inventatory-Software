// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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

// Reads only the header row, so it is cheap and ignores the product lines.
CsvFormat detectCsvFormat(const string& text);

// True when the header row is an Altium BOM. Shared by the detector and the BOM dispatch.
bool hasAltiumBomHeaders(const vector<string>& headers);

}  // namespace inventatory
