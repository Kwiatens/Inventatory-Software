// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Picks the BOM dialect for a file's text and parses it into the shared BOM shape.

#include "import/bom/BomImport.h"

#include "import/csv/CsvFormat.h"
#include "import/easyeda/EasyEdaBom.h"

namespace inventatory {

KicadBomFile parseBomText(const string& text, const string& projectName) {
  if (detectCsvFormat(text) == CsvFormat::EasyEdaBom) {
    return parseEasyEdaBomText(text, projectName);
  }
  return parseKicadBomText(text, projectName);
}

}  // namespace inventatory
