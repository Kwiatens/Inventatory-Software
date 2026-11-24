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

bool readBomRows(const string& text, bool backslashQuotes, vector<vector<string>>& rows,
                 vector<size_t>& rowLines, KicadBomFile& bom) {
  if (text.size() > kMaximumCsvInputBytes) {
    bom.error = "BOM import exceeds the 25 MiB safety limit";
    return false;
  }
  const auto cleaned = stripByteOrderMark(text);
  const auto delimiter = sniffDelimiter(cleaned);
  string parseError;
  rows = backslashQuotes ? parseCsvBackslashQuotes(cleaned, delimiter, parseError, &rowLines)
                         : parseCsv(cleaned, delimiter, parseError, (numeric_limits<size_t>::max)(), &rowLines);
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

namespace {

bool looksLikeDesignator(const string& token) {
  if (token == "REF**") return true;
  size_t index = token.size() > 1 && token[0] == '#' ? 1U : 0U;
  const size_t letters = index;
  while (index < token.size() && isalpha(static_cast<unsigned char>(token[index]))) ++index;
  if (index == letters || index - letters > 6U) return false;
  const size_t digits = index;
  while (index < token.size() && isdigit(static_cast<unsigned char>(token[index]))) ++index;
  if (index == digits) return false;
  return index == token.size() || (index + 1U == token.size() && isalpha(static_cast<unsigned char>(token[index])));
}

bool looksLikeFootprint(const string& cell) {
  return cell.find('_') != string::npos || cell.find(':') != string::npos;
}

bool looksLikeFiller(const string& cell) {
  return cell == "~" || cell.rfind("http", 0) == 0 || cell.rfind("www.", 0) == 0;
}

}  // namespace

bool inferBomLayout(const vector<vector<string>>& rows, BomLayout& layout) {
  if (rows.size() < 2) return false;
  size_t width = 0;
  for (const auto& row : rows) width = max(width, row.size());
  const int columns = static_cast<int>(width);
  const auto& header = rows.front();

  layout.designator = findColumn(header, {"Designator", "Designators", "Reference", "References", "Ref", "Refs"});
  layout.footprint = findColumn(header, {"Footprint", "Package", "Footprint Name"});
  layout.value = findColumn(header, {"Designation", "Value", "Comment"});
  layout.quantity = findColumn(header, {"Quantity", "Qty", "Quantity Per PCB"});

  const auto taken = [&](int column) {
    return column == layout.designator || column == layout.footprint || column == layout.value ||
           column == layout.quantity;
  };

  if (layout.designator < 0) {
    double best = 0.0;
    for (int column = 0; column < columns; ++column) {
      if (taken(column)) continue;
      size_t filled = 0;
      size_t matching = 0;
      for (size_t r = 1; r < rows.size(); ++r) {
        vector<string> tokens;
        if (!splitBomDesignators(csvCell(rows[r], column), tokens) || tokens.empty()) continue;
        ++filled;
        if (all_of(tokens.begin(), tokens.end(), looksLikeDesignator)) ++matching;
      }
      const double score = filled == 0 ? 0.0 : static_cast<double>(matching) / static_cast<double>(filled);
      if (score >= 0.8 && score > best) {
        best = score;
        layout.designator = column;
      }
    }
  }
  if (layout.designator < 0) return false;

  if (layout.quantity < 0) {
    double best = 0.0;
    for (int column = 0; column < columns; ++column) {
      if (taken(column)) continue;
      size_t filled = 0;
      size_t equal = 0;
      for (size_t r = 1; r < rows.size(); ++r) {
        vector<string> refs;
        if (!splitBomDesignators(csvCell(rows[r], layout.designator), refs) || refs.empty()) continue;
        ++filled;
        const auto count = parseBomCount(csvCell(rows[r], column));
        if (count && static_cast<size_t>(*count) == refs.size()) ++equal;
      }
      const double score = filled == 0 ? 0.0 : static_cast<double>(equal) / static_cast<double>(filled);
      if (score >= 0.8 && score > best) {
        best = score;
        layout.quantity = column;
      }
    }
  }

  if (layout.footprint < 0) {
    double best = 0.0;
    for (int column = 0; column < columns; ++column) {
      if (taken(column)) continue;
      size_t filled = 0;
      size_t matching = 0;
      for (size_t r = 1; r < rows.size(); ++r) {
        const auto cell = csvCell(rows[r], column);
        if (cell.empty()) continue;
        ++filled;
        if (looksLikeFootprint(cell) && !looksLikeFiller(cell)) ++matching;
      }
      const double score = filled == 0 ? 0.0 : static_cast<double>(matching) / static_cast<double>(filled);
      if (score >= 0.5 && score > best) {
        best = score;
        layout.footprint = column;
      }
    }
  }

  if (layout.value < 0) {
    size_t bestFilled = 0;
    for (int column = 0; column < columns; ++column) {
      if (taken(column)) continue;
      size_t filled = 0;
      size_t numeric = 0;
      size_t filler = 0;
      for (size_t r = 1; r < rows.size(); ++r) {
        const auto cell = csvCell(rows[r], column);
        if (cell.empty()) continue;
        ++filled;
        if (parseBomCount(cell)) ++numeric;
        if (looksLikeFiller(cell)) ++filler;
      }
      // A running index, a datasheet link or an empty column is not the value.
      if (filled == 0 || numeric == filled || filler * 2U > filled) continue;
      if (filled > bestFilled) {
        bestFilled = filled;
        layout.value = column;
      }
    }
  }
  return layout.value >= 0;
}

void finishBom(KicadBomFile& bom) {
  if (bom.lines.empty()) {
    bom.error = "BOM headers were recognized, but no orderable component rows were found";
  } else {
    bom.ok = true;
  }
}

}  // namespace inventatory
