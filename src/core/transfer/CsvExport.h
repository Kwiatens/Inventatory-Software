// Inventatory - CSV text-cell formatting for spreadsheet-facing exports.

#pragma once

#include <string>

namespace inventatory {

namespace csv_export_detail {

inline bool formulaPrefixAt(const std::string& value, std::size_t offset) {
  if (offset >= value.size()) return false;
  const char ch = value[offset];
  if (ch == '=' || ch == '+' || ch == '-' || ch == '@') return true;

  // Full-width equals, plus, minus, and at signs can start formulas in some
  // spreadsheet locales. Compare their UTF-8 encodings without locale APIs.
  return value.compare(offset, 3, "\xEF\xBC\x9D") == 0 || value.compare(offset, 3, "\xEF\xBC\x8B") == 0 ||
         value.compare(offset, 3, "\xEF\xBC\x8D") == 0 || value.compare(offset, 3, "\xEF\xBC\xA0") == 0;
}

}  // namespace csv_export_detail

// Prefix spreadsheet formulas with a tab inside the quoted cell. The tab is
// retained in the CSV value, so this formatter is intended for human-facing
// spreadsheet exports rather than lossless programmatic interchange.
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
