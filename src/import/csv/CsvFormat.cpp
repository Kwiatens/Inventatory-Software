// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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

// EasyEDA Pro: KiCad-style value columns plus Supplier or Manufacturer Part. Checked before KiCad.
bool hasEasyEdaProHeaders(const vector<string>& headers) {
  return findColumn(headers, {"Designator"}) >= 0 && findColumn(headers, {"Footprint"}) >= 0 &&
         findColumn(headers, {"Name", "Value", "Comment"}) >= 0 &&
         findColumn(headers, {"Supplier Part", "Manufacturer Part"}) >= 0;
}

// BOM-shaped header with no usable value column; reported as a BOM problem.
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

// Altium BOM headers overlap KiCad's, so a file is Altium only with a header KiCad does not
// write (LibRef, or Description beside a supplier or manufacturer part). Anything in doubt is KiCad.
// Keep the aliases in step with AltiumBom.cpp; "Qty" and "Supplier Part" are left out on purpose.
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

  // KiCad is checked after EasyEDA: its Value and Quantity columns overlap DigiKey's, but only
  // a BOM has Designator plus Footprint.
  if (hasEasyEdaProHeaders(rows.front())) {
    return CsvFormat::EasyEdaBom;
  }
  if (hasKicadHeaders(rows.front())) {
    return CsvFormat::KicadBom;
  }
  if (hasEasyEdaHeaders(rows.front())) {
    return CsvFormat::EasyEdaBom;
  }
  // Altium 365 exports have no Footprint column, so the checks above miss them.
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
