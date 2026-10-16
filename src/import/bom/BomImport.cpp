// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Picks the BOM dialect for a file's text and parses it into the shared BOM shape.

#include "import/bom/BomImport.h"

#include "import/altium/AltiumBom.h"
#include "import/csv/CsvFormat.h"
#include "import/csv/CsvReader.h"
#include "import/easyeda/EasyEdaBom.h"

namespace inventatory {

using namespace std;

namespace {

// An Altium Designer Bill of Materials names its reference column "Designator" and its value column "Comment",
// and KiCad BOMs can carry those names too, so the two headers alone prove nothing. A file is Altium only when it
// also has a header that KiCad does not write (LibRef, or Description beside a supplier or manufacturer part
// column) and none of the headers only KiCad writes. Anything in doubt stays on the KiCad parser, which is what
// stored projects were always read with.
//
// The supplier and manufacturer names below are the aliases AltiumBom.cpp reads, and they are NOT verified against
// a real Altium export. Keep them in step with AltiumBom.cpp; "Qty" and "Supplier Part" are left out on purpose (see
// AltiumBom.cpp).
bool looksLikeAltiumBom(const string& text) {
  const auto cleaned = stripByteOrderMark(text);
  string error;
  const auto rows = parseCsv(cleaned, sniffDelimiter(cleaned), error, 1);
  if (!error.empty() || rows.empty()) {
    return false;
  }

  const auto& headers = rows.front();
  if (findColumn(headers, {"Designator"}) < 0 || findColumn(headers, {"Comment"}) < 0) {
    return false;
  }
  if (findColumn(headers, {"Ref", "Reference", "Value", "Qty", "Cmp name"}) >= 0) {
    return false;
  }
  if (findColumn(headers, {"LibRef"}) >= 0) {
    return true;
  }
  return findColumn(headers, {"Description"}) >= 0 &&
         findColumn(headers, {"Supplier 1", "Supplier Name 1", "Supplier", "Supplier Part Number 1",
                              "Supplier Part Number", "Manufacturer Part Number 1", "Manufacturer Part Number"}) >= 0;
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
