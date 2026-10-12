// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
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

// Footprints of parts that are never bought: copper pads on a panel edge, fiducials, mounting holes, test
// points and similar. They are excluded only when the row has no supplier or manufacturer part number.
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
  if (text.size() > kMaximumCsvInputBytes) {
    bom.error = "BOM import exceeds the 25 MiB safety limit";
    return bom;
  }

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

  const auto columns = mapEasyEdaColumns(rows.front());
  if (columns.name < 0 || columns.designator < 0 || columns.footprint < 0) {
    bom.error = "BOM is missing the Name, Value or Comment, Designator or Footprint column";
    return bom;
  }

  // Each reference designator may appear once across the whole export. A repeated reference would count one
  // physical part twice, so the whole file is refused and the message names the reference.
  unordered_map<string, size_t> designatorRows;
  for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
    const auto& row = rows[rowIndex];
    const auto sourceRow = rowIndex + 1;
    const auto rowLabel = "Row " + to_string(sourceRow) + ": ";

    BomLine line;
    line.sourceRow = sourceRow;
    if (!splitBomDesignators(csvCell(row, columns.designator), line.designators)) {
      bom.error = rowLabel + "too many designators on one row. Fix the BOM export and import it again.";
      return bom;
    }
    if (line.designators.empty()) {
      bom.error = rowLabel + "has no designator, so its parts cannot be placed. Fix the BOM export and import it again.";
      return bom;
    }
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

    for (const auto& designator : line.designators) {
      const auto [seen, inserted] = designatorRows.emplace(designator, sourceRow);
      if (!inserted) {
        bom.error = "Designator " + designator + " appears on rows " + to_string(seen->second) + " and " +
                    to_string(sourceRow) + ". Fix the BOM export and import it again.";
        return bom;
      }
    }

    const bool hasPartNumber = !line.supplierPartNumber.empty() || !line.manufacturerPartNumber.empty();
    const bool allNonOrderable = all_of(line.designators.begin(), line.designators.end(), [&](const string& ref) {
      return isNonOrderableDesignator(ref, line.footprint);
    });
    if (!hasPartNumber && (allNonOrderable || isNonElectricalFootprint(line.footprint))) {
      bom.warnings.push_back(rowLabel + line.designation + " is not orderable");
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
