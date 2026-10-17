// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Header-only sniffing that tells the importer which CSV dialect arrived.

#include "import/csv/CsvFormat.h"

#include "import/csv/CsvReader.h"

namespace inventatory {

using namespace std;

namespace {

bool hasKicadHeaders(const vector<string>& headers) {
  const bool designator = findColumn(headers, {"Designator", "Designators", "Reference", "References", "Ref",
                                               "Refs"}) >= 0;
  const bool value = findColumn(headers, {"Designation", "Value", "Comment"}) >= 0;
  const bool footprint = findColumn(headers, {"Footprint", "Package", "Footprint Name"}) >= 0;
  return designator && value && footprint;
}

// EasyEDA exports name the value column "Name", and always carry Designator and Footprint.
bool hasEasyEdaHeaders(const vector<string>& headers) {
  return findColumn(headers, {"Designator"}) >= 0 && findColumn(headers, {"Footprint"}) >= 0 &&
         findColumn(headers, {"Name"}) >= 0;
}

// EasyEDA Pro exports use Value or Comment for the value, like KiCad, but carry Supplier Part or Manufacturer Part.
// Those columns are what tell the two apart, so this is checked before KiCad.
bool hasEasyEdaProHeaders(const vector<string>& headers) {
  return findColumn(headers, {"Designator"}) >= 0 && findColumn(headers, {"Footprint"}) >= 0 &&
         findColumn(headers, {"Name", "Value", "Comment"}) >= 0 &&
         findColumn(headers, {"Supplier Part", "Manufacturer Part"}) >= 0;
}

// A BOM-shaped header with no usable value column. Reported as a BOM problem rather than a DigiKey one.
bool hasBomHeadersWithoutValue(const vector<string>& headers) {
  const bool designator = findColumn(headers, {"Designator", "Designators", "Reference", "References", "Ref",
                                               "Refs"}) >= 0;
  const bool footprint = findColumn(headers, {"Footprint", "Package", "Footprint Name"}) >= 0;
  return designator && footprint && !hasEasyEdaHeaders(headers);
}

bool hasDigiKeyHeaders(const vector<string>& headers) {
  const bool digikeyPart = findColumn(headers, {"Digi-Key Part Number", "DigiKey Part Number", "DigiKey Part",
                                                "Nr kat. DigiKey", "Digi-Key Part #"}) >= 0;
  const bool manufacturerPart = findColumn(headers, {"Manufacturer Part Number", "Mfr Part Number",
                                                     "Manufacturer Part #", "Nr producenta"}) >= 0;
  const bool manufacturer = findColumn(headers, {"Manufacturer", "Producent", "Mfr"}) >= 0;
  const bool description = findColumn(headers, {"Description", "Opis", "Product Description"}) >= 0;
  const bool quantity = findColumn(headers, {"Quantity", "Qty", "Ilość", "Ilosc"}) >= 0;
  return digikeyPart && manufacturerPart && manufacturer && description && quantity;
}

}  // namespace

// An Altium Designer Bill of Materials names its reference column "Designator" and its value column "Comment"
// (or "Description" in an Altium 365 export). KiCad BOMs can carry those names too, so the two headers alone prove
// nothing. A file is Altium only when it also has a header that KiCad does not write (LibRef, or Description beside
// a supplier or manufacturer part column) and none of the headers only KiCad writes. Anything in doubt stays on the
// KiCad parser, which is what stored projects were always read with.
//
// The supplier and manufacturer names below are the aliases AltiumBom.cpp reads, and they are NOT verified against
// a real Altium export. Keep them in step with AltiumBom.cpp; "Qty" and "Supplier Part" are left out on purpose (see
// AltiumBom.cpp).
bool hasAltiumBomHeaders(const vector<string>& headers) {
  if (findColumn(headers, {"Designator"}) < 0 || findColumn(headers, {"Comment", "Description"}) < 0) {
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

CsvFormat detectCsvFormat(const string& text) {
  const auto cleaned = stripByteOrderMark(text);
  string error;
  const auto rows = parseCsv(cleaned, sniffDelimiter(cleaned), error, 1);
  if (!error.empty() || rows.empty()) {
    return CsvFormat::Unknown;
  }

  // An export with supplier or manufacturer part columns is EasyEDA. Otherwise KiCad is checked next: its
  // Quantity/Value columns overlap with DigiKey's, but only a BOM carries Designator plus Footprint.
  if (hasEasyEdaProHeaders(rows.front())) {
    return CsvFormat::EasyEdaBom;
  }
  if (hasKicadHeaders(rows.front())) {
    return CsvFormat::KicadBom;
  }
  if (hasEasyEdaHeaders(rows.front())) {
    return CsvFormat::EasyEdaBom;
  }
  // An Altium 365 export has no Footprint column, so none of the checks above matches it.
  if (hasAltiumBomHeaders(rows.front())) {
    return CsvFormat::KicadBom;
  }
  if (hasBomHeadersWithoutValue(rows.front())) {
    return CsvFormat::BomMissingValue;
  }
  if (hasDigiKeyHeaders(rows.front())) {
    return CsvFormat::DigiKeyOrder;
  }
  return CsvFormat::Unknown;
}

}  // namespace inventatory
