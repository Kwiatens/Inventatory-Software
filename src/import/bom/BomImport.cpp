// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Picks the BOM dialect for a file's text and parses it into the shared BOM shape.

#include "import/bom/BomImport.h"

#include "import/altium/AltiumBom.h"
#include "import/csv/CsvFormat.h"
#include "import/csv/CsvReader.h"
#include "import/easyeda/EasyEdaBom.h"

namespace inventatory {

using namespace std;

namespace {

bool looksLikeAltiumBom(const string& text) {
  const auto cleaned = stripByteOrderMark(text);
  string error;
  const auto rows = parseCsv(cleaned, sniffDelimiter(cleaned), error, 1);
  if (!error.empty() || rows.empty()) {
    return false;
  }
  return hasAltiumBomHeaders(rows.front());
}

}  // namespace

KicadBomFile parseBomText(const string& text, const string& projectName) {
  if (detectCsvFormat(text) == CsvFormat::EasyEdaBom) {
    return parseEasyEdaBomText(text, projectName);
  }
  if (looksLikeAltiumBom(text)) {
    return parseAltiumBomText(text, projectName);
  }
  return parseKicadBomText(text, projectName);
}

}  // namespace inventatory
