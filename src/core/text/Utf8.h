// Inventatory - Hardware Inventory Management System
// Strict UTF-8 encoding and decoding shared by the CSV, JSON and DigiKey code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace inventatory {

// Appends `codePoint` as UTF-8. The caller supplies a valid scalar value (at most 0x10FFFF).
inline void appendUtf8(std::string& out, std::uint32_t codePoint) {
  if (codePoint < 0x80U) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else if (codePoint < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  }
}

// Decodes the code point at `offset` and advances past it. Returns false, leaving `offset` past the
// bytes it examined, for a truncated sequence, a stray continuation byte, an overlong form, a
// surrogate or a value above 0x10FFFF.
inline bool nextUtf8CodePoint(const std::string& input, std::size_t& offset, std::uint32_t& codePoint) {
  if (offset >= input.size()) return false;
  const auto first = static_cast<unsigned char>(input[offset++]);
  if (first <= 0x7FU) {
    codePoint = first;
    return true;
  }
  unsigned int continuationCount = 0;
  if ((first & 0xE0U) == 0xC0U) { codePoint = first & 0x1FU; continuationCount = 1; }
  else if ((first & 0xF0U) == 0xE0U) { codePoint = first & 0x0FU; continuationCount = 2; }
  else if ((first & 0xF8U) == 0xF0U) { codePoint = first & 0x07U; continuationCount = 3; }
  else return false;
  if (input.size() - offset < continuationCount) return false;
  for (unsigned int index = 0; index < continuationCount; ++index) {
    const auto continuation = static_cast<unsigned char>(input[offset++]);
    if ((continuation & 0xC0U) != 0x80U) return false;
    codePoint = (codePoint << 6U) | (continuation & 0x3FU);
  }
  if ((continuationCount == 1U && codePoint < 0x80U) ||
      (continuationCount == 2U && codePoint < 0x800U) ||
      (continuationCount == 3U && codePoint < 0x10000U) || codePoint > 0x10FFFFU ||
      (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) return false;
  return true;
}

}  // namespace inventatory
