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

CsvFormat detectCsvFormat(const string& text) {
  const auto cleaned = stripByteOrderMark(text);
  string error;
  const auto rows = parseCsv(cleaned, sniffDelimiter(cleaned), error, 1);
  if (!error.empty() || rows.empty()) {
    return CsvFormat::Unknown;
  }

  // KiCad is checked first: its Quantity/Value columns overlap with DigiKey's,
  // but only a KiCad export carries Designator plus Footprint.
  if (hasKicadHeaders(rows.front())) {
    return CsvFormat::KicadBom;
  }
  if (hasEasyEdaHeaders(rows.front())) {
    return CsvFormat::EasyEdaBom;
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
