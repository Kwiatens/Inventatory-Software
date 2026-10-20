// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
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
  int value = -1;
  int name = -1;
  int footprint = -1;
  int quantity = -1;
  int supplier = -1;
  int supplierPart = -1;
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

// Columns are found by name, not position, as the user chooses them. The aliases are unverified
// against a real Altium export. Only the first supplier and manufacturer choice is read.
AltiumColumns mapAltiumColumns(const vector<string>& headers) {
  AltiumColumns columns;
  // Only "Designator" is read: BomImport.cpp detects Altium reports by that header.
  columns.designator = findColumnByPriority(headers, {"Designator"});
  // Comment is Altium's value column; Description is the fallback.
  columns.value = findColumnByPriority(headers, {"Comment", "Description"});
  // Name is the last choice when a row has neither.
  columns.name = findColumnByPriority(headers, {"Name"});
  columns.footprint = findColumnByPriority(headers, {"Footprint", "Pattern"});
  // "Qty" is not an alias: KiCad writes that header, and BomImport.cpp never routes it here.
  columns.quantity = findColumnByPriority(headers, {"Quantity"});
  columns.supplier = findColumnByPriority(headers, {"Supplier 1", "Supplier Name 1", "Supplier"});
  // "Supplier Part" is not an alias: BomImport.cpp sends that EasyEDA Pro header elsewhere.
  columns.supplierPart = findColumnByPriority(headers, {"Supplier Part Number 1", "Supplier Part Number"});
  columns.manufacturerPart = findColumnByPriority(headers, {"Manufacturer Part Number 1", "Manufacturer Part Number"});
  return columns;
}

}  // namespace

KicadBomFile parseAltiumBomText(const string& text, const string& projectName) {
  KicadBomFile bom;
  bom.projectName = projectName;
  if (text.size() > kMaximumCsvInputBytes) {
    bom.error = "BOM import exceeds the 25 MiB safety limit";
    return bom;
  }

  // Some Altium 365 exports write inch marks as \" in quotes; a backslash must not end the cell.
  const auto cleaned = stripByteOrderMark(text);
  string parseError;
  const auto rows = parseCsvBackslashQuotes(cleaned, sniffDelimiter(cleaned), parseError);
  if (!parseError.empty()) {
    bom.error = parseError;
    return bom;
  }
  if (rows.size() < 2) {
    bom.error = "BOM does not contain a header and component rows";
    return bom;
  }

  const auto columns = mapAltiumColumns(rows.front());
  if (columns.designator < 0 || columns.value < 0) {
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
    const auto sourceRow = rowIndex + 1;
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
    const auto partColumns = {columns.designator, columns.value, columns.footprint, columns.quantity,
                              columns.supplier, columns.supplierPart, columns.manufacturerPart};
    if (all_of(partColumns.begin(), partColumns.end(), [&](int column) { return csvCell(row, column).empty(); })) {
      continue;
    }

    BomLine line;
    line.sourceRow = sourceRow;
    if (!splitBomDesignators(csvCell(row, columns.designator), line.designators)) {
      bom.error = rowLabel + "too many designators on one row. Fix the BOM export and import it again.";
      return bom;
    }
    if (line.designators.empty()) {
      bom.error =
          rowLabel + "has no designator, so its parts cannot be placed. Fix the BOM export and import it again.";
      return bom;
    }
    line.designation = csvCell(row, columns.value);
    if (line.designation.empty()) {
      line.designation = csvCell(row, columns.name);
    }
    if (line.designation.empty()) {
      bom.error = rowLabel + "has no Comment, Description or Name for " + join(line.designators, ',') +
                  ". Fix the BOM export and import it again.";
      return bom;
    }
    line.footprint = csvCell(row, columns.footprint);
    line.supplier = csvCell(row, columns.supplier);
    line.supplierPartNumber = csvCell(row, columns.supplierPart);
    line.manufacturerPartNumber = csvCell(row, columns.manufacturerPart);

    for (const auto& designator : line.designators) {
      const auto [seen, inserted] = designatorRows.emplace(designator, sourceRow);
      if (!inserted) {
        bom.error = "Designator " + designator + " appears on rows " + to_string(seen->second) + " and " +
                    to_string(sourceRow) + ". Fix the BOM export and import it again.";
        return bom;
      }
    }

    // Test points, mounting holes and fiducials are never bought. Unlike EasyEDA, a part number
    // does not keep them; the designator and footprint decide, as for KiCad.
    const bool excluded = all_of(line.designators.begin(), line.designators.end(), [&](const string& ref) {
      return isNonOrderableDesignator(ref, line.footprint);
    });
    if (excluded) {
      bom.warnings.push_back(rowLabel + line.designation + " (" + join(line.designators, ',') + ") is not orderable");
      continue;
    }

    // Demand is at least the designator count. A Quantity that disagrees needs confirmation.
    const int designatorCount = static_cast<int>(line.designators.size());
    int requested = 0;
    const auto quantityText = csvCell(row, columns.quantity);
    if (!trim(quantityText).empty()) {
      const auto parsed = parseBomCount(quantityText);
      if (!parsed) {
        bom.error = rowLabel + "Quantity \"" + quantityText +
                    "\" is not a whole number. Fix the BOM export and import it again.";
        return bom;
      }
      requested = *parsed;
      if (requested != designatorCount) {
        line.quantityConflict = true;
        bom.warnings.push_back(rowLabel + line.designation + " lists " + to_string(designatorCount) +
                               " designators but Quantity " + to_string(requested) + "; confirm the match");
      }
    }
    line.quantityPerBoard = max(requested, designatorCount);
    bom.lines.push_back(move(line));
  }

  if (bom.lines.empty()) {
    bom.error = "BOM headers were recognized, but no orderable component rows were found";
    return bom;
  }

  bom.ok = true;
  return bom;
}

}  // namespace inventatory
