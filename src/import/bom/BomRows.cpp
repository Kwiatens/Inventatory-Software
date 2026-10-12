// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Row-level helpers shared by every BOM dialect: designator lists and count cells.

#include "import/bom/BomRows.h"

#include "core/inventory/Inventory.h"

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

}  // namespace inventatory
