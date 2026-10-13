// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - TME request encoding primitives.

#include "platform/tme/TmeEncoding.h"

#include <cstdint>

namespace inventatory {
namespace tme_encoding {

std::string base64Encode(const std::string& data) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve((data.size() + 2) / 3 * 4);
  for (std::size_t index = 0; index < data.size(); index += 3) {
    const std::uint32_t first = static_cast<unsigned char>(data[index]);
    const std::uint32_t second = index + 1 < data.size() ? static_cast<unsigned char>(data[index + 1]) : 0U;
    const std::uint32_t third = index + 2 < data.size() ? static_cast<unsigned char>(data[index + 2]) : 0U;
    const std::uint32_t triple = (first << 16) | (second << 8) | third;
    encoded.push_back(kAlphabet[(triple >> 18) & 0x3FU]);
    encoded.push_back(kAlphabet[(triple >> 12) & 0x3FU]);
    encoded.push_back(index + 1 < data.size() ? kAlphabet[(triple >> 6) & 0x3FU] : '=');
    encoded.push_back(index + 2 < data.size() ? kAlphabet[triple & 0x3FU] : '=');
  }
  return encoded;
}

std::string percentEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
                            byte == '~';
    if (unreserved) {
      encoded.push_back(character);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[byte >> 4U]);
      encoded.push_back(kHex[byte & 0x0FU]);
    }
  }
  return encoded;
}

}  // namespace tme_encoding
}  // namespace inventatory
