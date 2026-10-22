// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// EasyEDA BOM export parsing (Standard and Pro CSV exports).

#include "import/easyeda/EasyEdaBom.h"

#include "core/inventory/Inventory.h"
#include "import/bom/BomRows.h"
#include "import/csv/CsvReader.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace inventatory {

using namespace std;

namespace {

struct EasyEdaColumns {
  int designator = -1;
  int footprint = -1;
  int name = -1;
  int quantity = -1;
  int supplier = -1;
  int supplierPart = -1;
  int manufacturerPart = -1;
};

EasyEdaColumns mapEasyEdaColumns(const vector<string>& headers) {
  EasyEdaColumns columns;
  columns.designator = findColumn(headers, {"Designator"});
  columns.footprint = findColumn(headers, {"Footprint"});
  columns.name = findColumn(headers, {"Name"});
  if (columns.name < 0) {
    columns.name = findColumn(headers, {"Value"});
  }
  if (columns.name < 0) {
    columns.name = findColumn(headers, {"Comment"});
  }
  columns.quantity = findColumn(headers, {"Quantity"});
  columns.supplier = findColumn(headers, {"Supplier"});
  columns.supplierPart = findColumn(headers, {"Supplier Part"});
  columns.manufacturerPart = findColumn(headers, {"Manufacturer Part"});
  return columns;
}

// Footprints of parts that are never bought. Excluded only when the row has no part number.
bool isNonElectricalFootprint(const string& footprint) {
  string compact;
  for (const unsigned char ch : footprint) {
    if (isalnum(ch)) compact.push_back(static_cast<char>(tolower(ch)));
  }
  static const initializer_list<const char*> kNeedles = {"castellated", "fiducial", "mountinghole", "testpoint",
                                                         "logo", "netie", "solderjumper"};
  return any_of(kNeedles.begin(), kNeedles.end(),
                [&](const char* needle) { return compact.find(needle) != string::npos; });
}

}  // namespace

KicadBomFile parseEasyEdaBomText(const string& text, const string& projectName) {
  KicadBomFile bom;
  bom.projectName = projectName;
  vector<vector<string>> rows;
  if (!readBomRows(text, false, rows, bom)) return bom;

  const auto columns = mapEasyEdaColumns(rows.front());
  if (columns.name < 0 || columns.designator < 0 || columns.footprint < 0) {
    bom.error = "BOM is missing the Name, Value or Comment, Designator or Footprint column";
    return bom;
  }

  // A designator may appear once per export; a repeat would count one part twice.
  unordered_map<string, size_t> designatorRows;
  for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
    const auto& row = rows[rowIndex];
    const auto sourceRow = rowIndex + 1;
    const auto rowLabel = "Row " + to_string(sourceRow) + ": ";

    BomLine line;
    line.sourceRow = sourceRow;
    if (!readRequiredBomDesignators(csvCell(row, columns.designator), rowLabel, line, bom)) return bom;
    line.designation = csvCell(row, columns.name);
    if (line.designation.empty()) {
      bom.error = rowLabel + "has no Name for " + join(line.designators, ',') +
                  ". Fix the BOM export and import it again.";
      return bom;
    }
    line.footprint = csvCell(row, columns.footprint);
    line.supplier = csvCell(row, columns.supplier);
    line.supplierPartNumber = csvCell(row, columns.supplierPart);
    line.manufacturerPartNumber = csvCell(row, columns.manufacturerPart);

    if (!claimBomDesignators(line, designatorRows, bom)) return bom;

    const bool hasPartNumber = !line.supplierPartNumber.empty() || !line.manufacturerPartNumber.empty();
    const bool allNonOrderable = all_of(line.designators.begin(), line.designators.end(), [&](const string& ref) {
      return isNonOrderableDesignator(ref, line.footprint);
    });
    if (!hasPartNumber && (allNonOrderable || isNonElectricalFootprint(line.footprint))) {
      bom.warnings.push_back(rowLabel + line.designation + " is not orderable");
      continue;
    }

    if (!applyBomQuantity(csvCell(row, columns.quantity), rowLabel, line, bom)) return bom;
    bom.lines.push_back(move(line));
  }
  finishBom(bom);
  return bom;
}

}  // namespace inventatory
