// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Row-level helpers shared by every BOM dialect: designator lists and count cells.

#pragma once

#include "import/kicad/KicadBom.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace inventatory {

using std::optional;
using std::size_t;
using std::string;
using std::vector;

// Rows naming more designators than this are rejected as malformed.
constexpr size_t kMaximumDesignatorsPerRow = 10000;

// Splits "C1, C15 C16" on commas, semicolons, spaces or tabs. False when over the limit.
bool splitBomDesignators(const string& value, vector<string>& designators);

// A whole, non-negative count such as "6". Nullopt for blank, signed, fractional or overflowing text.
optional<int> parseBomCount(const string& value);

// Reads BOM text into CSV rows, checking the size limit and that a header and a component row exist.
// backslashQuotes keeps a \" inside a quoted cell, as Altium 365 writes inch marks. rowLines gets
// the file line each row starts on, for row labels in messages. False with bom.error set.
bool readBomRows(const string& text, bool backslashQuotes, vector<vector<string>>& rows,
                 vector<size_t>& rowLines, KicadBomFile& bom);

// For dialects that refuse a malformed row (EasyEDA, Altium). Each returns false with bom.error set.
// Reads the row's designators into the line; a row needs at least one.
bool readRequiredBomDesignators(const string& cell, const string& rowLabel, BomLine& line, KicadBomFile& bom);
// Records the line's designators. One already seen on an earlier row would count a part twice.
bool claimBomDesignators(const BomLine& line, std::unordered_map<string, size_t>& designatorRows,
                         KicadBomFile& bom);
// Demand is at least the designator count. A Quantity that disagrees is kept and needs confirmation.
bool applyBomQuantity(const string& quantityText, const string& rowLabel, BomLine& line, KicadBomFile& bom);

// Where the parts sit in a BOM table, found from the cells instead of the header names, so a BOM
// exported in any language or with renamed or extra columns still reads. A column index is -1 when unknown.
struct BomLayout {
  int designator = -1;
  int footprint = -1;
  int value = -1;
  int quantity = -1;
};

// rows[0] is the header. Known English header names are used as hints; every other column is judged
// by its cells (reference lists, counts that equal the reference count, footprint-looking text).
// False unless both a designator and a value column were found.
bool inferBomLayout(const vector<vector<string>>& rows, BomLayout& layout);

// Marks the BOM ok, or fails it when no orderable line was found.
void finishBom(KicadBomFile& bom);

}  // namespace inventatory
