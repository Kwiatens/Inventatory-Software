// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Altium Designer BOM export parsing (a Bill of Materials report saved as CSV).

#include "import/altium/AltiumBom.h"

#include "core/inventory/Inventory.h"
#include "import/bom/BomRows.h"
#include "import/csv/CsvReader.h"

#include <algorithm>
#include <unordered_map>

namespace inventatory {

using namespace std;

namespace {

struct AltiumColumns {
  int designator = -1;
  int comment = -1;
  int value = -1;
  int description = -1;
  int name = -1;
  int libRef = -1;
  int footprint = -1;
  int quantity = -1;
  int supplier = -1;
  int supplierPart = -1;
  int lcscPart = -1;
  int manufacturerPart = -1;
};

// The first alias listed wins regardless of column order (findColumn takes the leftmost).
int findColumnByPriority(const vector<string>& headers, initializer_list<const char*> aliases) {
  for (const auto* alias : aliases) {
    const auto index = findColumn(headers, {alias});
    if (index >= 0) return index;
  }
  return -1;
}

// Columns are found by name, not position, as the user chooses them. Comment, Description,
// Designator, Footprint, LibRef, Value and Quantity match public Altium Designer exports (see the
// altium-real fixtures); the supplier aliases match Altium 365 exports. Only the first supplier and manufacturer choice is read.
AltiumColumns mapAltiumColumns(const vector<string>& headers) {
  AltiumColumns columns;
  // Only "Designator" is read: BomImport.cpp detects Altium reports by that header.
  columns.designator = findColumnByPriority(headers, {"Designator"});
  // Comment is Altium's value column. A row with an empty Comment falls back, cell by cell, to a
  // Value parameter, then Description, Name and the library part name (see designationFor).
  columns.comment = findColumnByPriority(headers, {"Comment"});
  columns.value = findColumnByPriority(headers, {"Value"});
  columns.description = findColumnByPriority(headers, {"Description"});
  columns.name = findColumnByPriority(headers, {"Name"});
  columns.libRef = findColumnByPriority(headers, {"LibRef"});
  columns.footprint = findColumnByPriority(headers, {"Footprint", "Pattern"});
  // "Qty" is not an alias: KiCad writes that header, and BomImport.cpp never routes it here.
  columns.quantity = findColumnByPriority(headers, {"Quantity"});
  columns.supplier = findColumnByPriority(headers, {"Supplier 1", "Supplier Name 1", "Supplier"});
  // "Supplier Part" is not an alias: BomImport.cpp sends that EasyEDA Pro header elsewhere.
  columns.supplierPart = findColumnByPriority(headers, {"Supplier Part Number 1", "Supplier Part Number"});
  // Altium parameters carry whatever name the library author chose; these are the ones seen in public
  // projects. "Manufacturer Part" is not an alias: BomImport.cpp sends that EasyEDA Pro header elsewhere.
  columns.lcscPart = findColumnByPriority(headers, {"LCSC Part #", "LCSC Part Number", "LCSC Part", "LCSC"});
  columns.manufacturerPart = findColumnByPriority(
      headers, {"Manufacturer Part Number 1", "Manufacturer Part Number", "MPN", "Mfr Part Number", "Mfg Part Number"});
  return columns;
}

// The first non-empty cell of Comment, Value, Description, Name and LibRef; empty when all are.
string designationFor(const vector<string>& row, const AltiumColumns& columns) {
  for (const auto column : {columns.comment, columns.value, columns.description, columns.name, columns.libRef}) {
    auto cell = csvCell(row, column);
    if (!cell.empty()) return cell;
  }
  return {};
}

}  // namespace

KicadBomFile parseAltiumBomText(const string& text, const string& projectName) {
  KicadBomFile bom;
  bom.projectName = projectName;
  // Some Altium 365 exports write inch marks as \" in quotes; a backslash must not end the cell.
  vector<vector<string>> rows;
  vector<size_t> rowLines;
  if (!readBomRows(text, true, rows, rowLines, bom)) return bom;

  const auto columns = mapAltiumColumns(rows.front());
  if (columns.designator < 0 || (columns.comment < 0 && columns.description < 0)) {
    bom.error = "BOM is missing the Designator column, or both the Comment and Description columns";
    return bom;
  }

  // A designator may appear once per export; a repeat would count one part twice.
  unordered_map<string, size_t> designatorRows;
  const auto columnCount = rows.front().size();
  for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
    const auto& row = rows[rowIndex];
    // Blank rows (spreadsheets leave these at the end) hold no part.
    if (all_of(row.begin(), row.end(), [](const string& cell) { return trim(cell).empty(); })) {
      continue;
    }
    const auto sourceRow = rowLines[rowIndex];
    const auto rowLabel = "Row " + to_string(sourceRow) + ": ";

    // A non-empty cell past the last column means a quote was misread. Empty ones are harmless.
    for (size_t cell = columnCount; cell < row.size(); ++cell) {
      if (!row[cell].empty()) {
        bom.error = rowLabel +
                    "has a cell past the last column, so a quote is probably misread. Fix the BOM export and import it "
                    "again.";
        return bom;
      }
    }

    // A total fills no part column, so skip it. A part number without a designator is refused.
    // Description counts only when there is no Comment column, as a note row may fill it alone.
    const auto partColumns = {columns.designator, columns.comment >= 0 ? columns.comment : columns.description,
                              columns.value, columns.footprint, columns.quantity, columns.supplier,
                              columns.supplierPart, columns.lcscPart, columns.manufacturerPart};
    if (all_of(partColumns.begin(), partColumns.end(), [&](int column) { return csvCell(row, column).empty(); })) {
      continue;
    }

    // A section heading ("Through Hole Items") fills one cell and no designator. Say so, as a
    // skipped row may also be a part that lost its designator.
    const auto filledCells = count_if(row.begin(), row.end(), [](const string& cell) { return !trim(cell).empty(); });
    if (filledCells == 1 && csvCell(row, columns.designator).empty() && !designationFor(row, columns).empty()) {
      bom.warnings.push_back(rowLabel + "\"" + designationFor(row, columns) +
                             "\" has no designator and looks like a heading, so it was skipped");
      continue;
    }

    BomLine line;
    line.sourceRow = sourceRow;
    if (!readRequiredBomDesignators(csvCell(row, columns.designator), rowLabel, line, bom)) return bom;
    line.designation = designationFor(row, columns);
    if (line.designation.empty()) {
      bom.error = rowLabel + "has no Comment, Description or Name for " + join(line.designators, ',') +
                  ". Fix the BOM export and import it again.";
      return bom;
    }
    line.footprint = csvCell(row, columns.footprint);
    line.supplier = csvCell(row, columns.supplier);
    line.supplierPartNumber = csvCell(row, columns.supplierPart);
    // An LCSC number fills the supplier slot only when the export names no other supplier part.
    if (line.supplierPartNumber.empty() && !csvCell(row, columns.lcscPart).empty()) {
      line.supplier = "LCSC";
      line.supplierPartNumber = csvCell(row, columns.lcscPart);
    }
    line.manufacturerPartNumber = csvCell(row, columns.manufacturerPart);

    if (!claimBomDesignators(line, designatorRows, bom)) return bom;

    // Test points, mounting holes and fiducials are never bought. Unlike EasyEDA, a part number
    // does not keep them; the designator and footprint decide, as for KiCad.
    const bool excluded = all_of(line.designators.begin(), line.designators.end(), [&](const string& ref) {
      return isNonOrderableDesignator(ref, line.footprint);
    });
    if (excluded) {
      bom.warnings.push_back(rowLabel + line.designation + " (" + join(line.designators, ',') + ") is not orderable");
      continue;
    }

    if (!applyBomQuantity(csvCell(row, columns.quantity), rowLabel, line, bom)) return bom;
    bom.lines.push_back(move(line));
  }
  finishBom(bom);
  return bom;
}

}  // namespace inventatory
