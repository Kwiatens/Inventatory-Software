// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Row-level helpers shared by every BOM dialect: designator lists and count cells.

#include "import/bom/BomRows.h"

#include "core/inventory/Inventory.h"
#include "import/csv/CsvReader.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace inventatory {

using namespace std;

bool splitBomDesignators(const string& value, vector<string>& designators) {
  string current;
  const auto flush = [&] {
    const auto trimmed = trim(current);
    if (!trimmed.empty()) {
      if (designators.size() >= kMaximumDesignatorsPerRow) {
        return false;
      }
      designators.push_back(trimmed);
    }
    current.clear();
    return true;
  };

  for (const char ch : value) {
    if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t') {
      if (!flush()) return false;
    } else {
      current.push_back(ch);
    }
  }
  return flush();
}

optional<int> parseBomCount(const string& value) {
  const auto trimmed = trim(value);
  if (trimmed.empty()) {
    return nullopt;
  }
  int result = 0;
  for (unsigned char ch : trimmed) {
    if (!isdigit(ch)) {
      return nullopt;
    }
    const int digit = static_cast<int>(ch - '0');
    if (result > (2147483647 - digit) / 10) {
      return nullopt;
    }
    result = result * 10 + digit;
  }
  return result;
}

bool readBomRows(const string& text, bool backslashQuotes, vector<vector<string>>& rows, KicadBomFile& bom) {
  if (text.size() > kMaximumCsvInputBytes) {
    bom.error = "BOM import exceeds the 25 MiB safety limit";
    return false;
  }
  const auto cleaned = stripByteOrderMark(text);
  const auto delimiter = sniffDelimiter(cleaned);
  string parseError;
  rows = backslashQuotes ? parseCsvBackslashQuotes(cleaned, delimiter, parseError)
                         : parseCsv(cleaned, delimiter, parseError);
  if (!parseError.empty()) {
    bom.error = parseError;
    return false;
  }
  if (rows.size() < 2) {
    bom.error = "BOM does not contain a header and component rows";
    return false;
  }
  return true;
}

bool readRequiredBomDesignators(const string& cell, const string& rowLabel, BomLine& line, KicadBomFile& bom) {
  if (!splitBomDesignators(cell, line.designators)) {
    bom.error = rowLabel + "too many designators on one row. Fix the BOM export and import it again.";
    return false;
  }
  if (line.designators.empty()) {
    bom.error = rowLabel + "has no designator, so its parts cannot be placed. Fix the BOM export and import it again.";
    return false;
  }
  return true;
}

bool claimBomDesignators(const BomLine& line, unordered_map<string, size_t>& designatorRows, KicadBomFile& bom) {
  for (const auto& designator : line.designators) {
    const auto [seen, inserted] = designatorRows.emplace(designator, line.sourceRow);
    if (!inserted) {
      bom.error = "Designator " + designator + " appears on rows " + to_string(seen->second) + " and " +
                  to_string(line.sourceRow) + ". Fix the BOM export and import it again.";
      return false;
    }
  }
  return true;
}

bool applyBomQuantity(const string& quantityText, const string& rowLabel, BomLine& line, KicadBomFile& bom) {
  const int designatorCount = static_cast<int>(line.designators.size());
  int requested = 0;
  if (!trim(quantityText).empty()) {
    const auto parsed = parseBomCount(quantityText);
    if (!parsed) {
      bom.error = rowLabel + "Quantity \"" + quantityText +
                  "\" is not a whole number. Fix the BOM export and import it again.";
      return false;
    }
    requested = *parsed;
    if (requested != designatorCount) {
      line.quantityConflict = true;
      bom.warnings.push_back(rowLabel + line.designation + " lists " + to_string(designatorCount) +
                             " designators but Quantity " + to_string(requested) + "; confirm the match");
    }
  }
  line.quantityPerBoard = max(requested, designatorCount);
  return true;
}

void finishBom(KicadBomFile& bom) {
  if (bom.lines.empty()) {
    bom.error = "BOM headers were recognized, but no orderable component rows were found";
  } else {
    bom.ok = true;
  }
}

}  // namespace inventatory
