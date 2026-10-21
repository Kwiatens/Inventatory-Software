// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Request encoding and response checks shared by the WinHTTP and libcurl transports.

#include "platform/digikey/DigiKeyApiPrivate.h"
#include "core/parts/DecimalParse.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace inventatory {
namespace digikey_detail {

string encodeComponent(const string& value, bool formEncoding) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    const bool asciiAlphaNumeric = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                   (ch >= '0' && ch <= '9');
    if (asciiAlphaNumeric || ch == '-' || ch == '_' || ch == '.' || ch == '~') out << static_cast<char>(ch);
    else if (ch == ' ' && formEncoding) out << '+';
    else if (ch == ' ' && !formEncoding) out << "%20";
    else out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch)
             << std::nouppercase << std::dec;
  }
  return out.str();
}

string encodeFormValue(const string& value) { return encodeComponent(value, true); }
string encodePathSegment(const string& value) { return encodeComponent(value, false); }

string escapeJsonString(const string& value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20U) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
        else out << static_cast<char>(ch);
        break;
    }
  }
  return out.str();
}

bool isSafeHeaderValue(const string& value) {
  if (value.size() > kMaximumDigiKeyFieldBytes) return false;
  for (const unsigned char character : value) if (character < 0x20U || character > 0x7eU) return false;
  return true;
}

bool appendHeader(std::ostringstream& headers, const char* name, const string& value, string* error) {
  if (!isSafeHeaderValue(value)) {
    if (error != nullptr) *error = "DigiKey configuration contains invalid header characters";
    return false;
  }
  headers << name << ": " << value << "\r\n";
  return true;
}

bool appendAuthorizationHeader(std::ostringstream& headers, const string& token, string* error) {
  if (token.size() > 2048U || !isSafeHeaderValue(token)) {
    if (error != nullptr) *error = "DigiKey returned an invalid access token";
    return false;
  }
  headers << "Authorization: Bearer " << token << "\r\n";
  return true;
}

bool isUnsignedDecimal(const string& value, unsigned long long maximum) {
  const auto trimmed = trim(value);
  if (trimmed.empty()) return false;
  unsigned long long parsed = 0;
  for (const unsigned char character : trimmed) {
    if (character < '0' || character > '9') return false;
    const auto digit = static_cast<unsigned long long>(character - '0');
    if (parsed > (std::numeric_limits<unsigned long long>::max() - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
    if (parsed > maximum) return false;
  }
  return true;
}

bool isFiniteDecimal(const string& value, double maximum) {
  const auto trimmed = trim(value);
  if (trimmed.empty()) return false;
  double parsed = 0.0;
  return parseClassicDecimal(trimmed, parsed) && std::isfinite(parsed) && parsed >= 0.0 && parsed <= maximum;
}

}  // namespace digikey_detail
}  // namespace inventatory
