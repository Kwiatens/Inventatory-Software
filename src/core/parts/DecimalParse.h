// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Locale-independent decimal parsing for machine-format text.

#pragma once

#include <locale>
#include <sstream>
#include <string>

namespace inventatory {

// Parses a dot-decimal literal regardless of locale: the app calls setlocale(LC_ALL, ""), and
// std::stod would stop at '.' under comma-decimal locales. Fails unless the whole string is
// one number.
inline bool parseClassicDecimal(const std::string& text, double& value) {
  std::istringstream stream(text);
  stream.imbue(std::locale::classic());
  double parsed = 0.0;
  stream >> parsed;
  if (stream.fail() || stream.peek() != std::char_traits<char>::eof()) return false;
  value = parsed;
  return true;
}

}  // namespace inventatory
