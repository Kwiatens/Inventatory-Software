// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - CSV text-cell formatting for spreadsheet-facing exports.

#pragma once

#include <string>

namespace inventatory {

namespace csv_export_detail {

inline bool formulaPrefixAt(const std::string& value, std::size_t offset) {
  if (offset >= value.size()) return false;
  const char ch = value[offset];
  if (ch == '=' || ch == '+' || ch == '-' || ch == '@') return true;

  // Full-width = + - @ start formulas in some locales. Compare UTF-8 bytes, no locale APIs.
  return value.compare(offset, 3, "\xEF\xBC\x9D") == 0 || value.compare(offset, 3, "\xEF\xBC\x8B") == 0 ||
         value.compare(offset, 3, "\xEF\xBC\x8D") == 0 || value.compare(offset, 3, "\xEF\xBC\xA0") == 0;
}

}  // namespace csv_export_detail

// Adds a leading tab inside the quotes for formula-like values. The tab is kept,
// so this is for spreadsheet exports, not lossless interchange.
inline std::string csvTextCell(const std::string& value) {
  std::size_t firstText = 0;
  while (firstText < value.size() && static_cast<unsigned char>(value[firstText]) <= 0x20U) ++firstText;

  const bool startsWithControl = !value.empty() && (value.front() == '\t' || value.front() == '\r' || value.front() == '\n');
  const bool formulaLike = firstText < value.size() && csv_export_detail::formulaPrefixAt(value, firstText);
  const auto& text = startsWithControl || formulaLike ? std::string("\t") + value : value;

  std::string escaped;
  escaped.reserve(text.size() + 2U);
  escaped.push_back('"');
  for (const char ch : text) {
    if (ch == '"') escaped.push_back('"');
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

}  // namespace inventatory
