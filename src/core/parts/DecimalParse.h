// Inventatory - Hardware Inventory Management System
// Locale-independent decimal parsing for machine-format text.

#pragma once

#include <locale>
#include <sstream>
#include <string>

namespace inventatory {

// Parses a complete dot-decimal literal such as "0.1" or "1e-3" without
// consulting the process locale. The application calls setlocale(LC_ALL, ""),
// and std::stod would then stop at the '.' under comma-decimal locales. Returns
// false unless the whole string is consumed as one number.
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
