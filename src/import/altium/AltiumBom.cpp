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
  int footprint = -1;
  int quantity = -1;
  int supplier = -1;
  int supplierPart = -1;
  int manufacturerPart = -1;
};

// The index of the column that the first alias in the list names. An earlier alias beats a later one whatever the
// order of the columns, which findColumn alone does not do: it returns the leftmost header that matches any alias.
int findColumnByPriority(const vector<string>& headers, initializer_list<const char*> aliases) {
  for (const auto* alias : aliases) {
    const auto index = findColumn(headers, {alias});
    if (index >= 0) return index;
  }
  return -1;
}

// An Altium Bill of Materials report has the columns the user chose, so the header row varies and every column is
// found by its name, never by its position. The default column names are NOT verified: every alias list below is
// a set of plausible names, to be checked against a real Altium export. Supplier and manufacturer columns come
// from the part choices in the Altium library, and Altium numbers them (Supplier 1, Supplier Part Number 1), so
// only the first choice is read.
AltiumColumns mapAltiumColumns(const vector<string>& headers) {
  AltiumColumns columns;
  // To be checked against a real Altium export. Only "Designator" is read: BomImport.cpp recognises an Altium report by
  // that header, so a file with another name would never reach this parser.
  columns.designator = findColumnByPriority(headers, {"Designator"});
  // Comment is Altium's value column. Description is the fallback when the report has no Comment.
  // To be checked against a real Altium export.
  columns.value = findColumnByPriority(headers, {"Comment", "Description"});
  // To be checked against a real Altium export.
  columns.footprint = findColumnByPriority(headers, {"Footprint", "Pattern"});
  // To be checked against a real Altium export. "Qty" is not an alias: KiCad writes that header, and BomImport.cpp
  // never sends a file with it to this parser, so the alias could not be reached.
  columns.quantity = findColumnByPriority(headers, {"Quantity"});
  // To be checked against a real Altium export.
  columns.supplier = findColumnByPriority(headers, {"Supplier 1", "Supplier Name 1", "Supplier"});
  // To be checked against a real Altium export. "Supplier Part" is not an alias: an exact "Supplier Part" header marks
  // an EasyEDA Pro export, which BomImport.cpp checks first, so such a file would never reach this parser.
  columns.supplierPart = findColumnByPriority(headers, {"Supplier Part Number 1", "Supplier Part Number"});
  // To be checked against a real Altium export.
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

  // Quoted cells are read by the shared CSV scanner, which keeps a delimiter inside quotes as part of the cell.
  const auto cleaned = stripByteOrderMark(text);
  string parseError;
  const auto rows = parseCsv(cleaned, sniffDelimiter(cleaned), parseError);
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

  // Each reference designator may appear once across the whole export. A repeated reference would count one
  // physical part twice, so the whole file is refused and the message names the reference.
  unordered_map<string, size_t> designatorRows;
  for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
    const auto& row = rows[rowIndex];
    // A row of delimiters with nothing in any cell (a spreadsheet leaves these at the end of a file) holds no part.
    if (all_of(row.begin(), row.end(), [](const string& cell) { return trim(cell).empty(); })) {
      continue;
    }
    const auto sourceRow = rowIndex + 1;
    const auto rowLabel = "Row " + to_string(sourceRow) + ": ";

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
      bom.error = rowLabel + "has no Comment or Description for " + join(line.designators, ',') +
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

    // Test points, mounting holes and fiducials are real rows that are never bought. Unlike the EasyEDA import,
    // a part number on the row does not keep it: the designators and the footprint decide, as they do for KiCad.
    const bool excluded = all_of(line.designators.begin(), line.designators.end(), [&](const string& ref) {
      return isNonOrderableDesignator(ref, line.footprint);
    });
    if (excluded) {
      bom.warnings.push_back(rowLabel + line.designation + " (" + join(line.designators, ',') + ") is not orderable");
      continue;
    }

    // Demand is never less than the designators placed. A Quantity that disagrees is kept visible and the
    // row cannot match on its own until the user confirms it.
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
