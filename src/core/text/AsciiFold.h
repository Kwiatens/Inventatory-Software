// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Folds vendor text to ASCII for the terminal: "Würth Łódź" -> "Wurth Lodz". The UI shows ASCII only, so a
// letter with a mark keeps its base letter instead of turning into "?".

#pragma once

#include "core/text/Utf8.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace inventatory {

// Latin-1 Supplement, U+00C0 to U+00FF. Entries that expand to several letters are listed in kFoldSpecial.
inline constexpr char kFoldLatin1[] =
    "AAAAAA?CEEEEIIII"  // C0-CF
    "DNOOOOOxOUUUUY??"  // D0-DF
    "aaaaaa?ceeeeiiii"  // E0-EF
    "dnooooo/ouuuuy?y"; // F0-FF
static_assert(sizeof(kFoldLatin1) - 1 == 64, "Latin-1 fold table must cover U+00C0 to U+00FF");

// Latin Extended-A, U+0100 to U+017F.
inline constexpr char kFoldLatinExtA[] =
    "AaAaAa"          // 0100-0105
    "CcCcCcCc"        // 0106-010D
    "DdDd"            // 010E-0111
    "EeEeEeEeEe"      // 0112-011B
    "GgGgGgGg"        // 011C-0123
    "HhHh"            // 0124-0127
    "IiIiIiIiIi"      // 0128-0131
    "??"              // 0132-0133 (IJ, ij)
    "Jj"              // 0134-0135
    "Kkk"             // 0136-0138
    "LlLlLlLlLl"      // 0139-0142
    "NnNnNnnNn"       // 0143-014B
    "OoOoOo"          // 014C-0151
    "??"              // 0152-0153 (OE, oe)
    "RrRrRr"          // 0154-0159
    "SsSsSsSs"        // 015A-0161
    "TtTtTt"          // 0162-0167
    "UuUuUuUuUuUu"    // 0168-0173
    "Ww"              // 0174-0175
    "YyY"             // 0176-0178
    "ZzZzZz"          // 0179-017E
    "s";              // 017F
static_assert(sizeof(kFoldLatinExtA) - 1 == 128, "Latin Extended-A fold table must cover U+0100 to U+017F");

struct FoldEntry {
  std::uint32_t codePoint;
  const char* ascii;
};

// Letters that expand to several characters, and symbols used in vendor text.
inline constexpr FoldEntry kFoldSpecial[] = {
    {0x00A0, " "},    {0x00B0, " deg"}, {0x00B1, "+/-"}, {0x00B2, "2"},   {0x00B3, "3"},    {0x00B5, "u"},
    {0x00B9, "1"},    {0x00BD, "1/2"},  {0x00C6, "AE"},  {0x00D7, "x"},   {0x00DE, "TH"},   {0x00DF, "ss"},
    {0x00E6, "ae"},   {0x00FE, "th"},   {0x0132, "IJ"},  {0x0133, "ij"},  {0x0152, "OE"},   {0x0153, "oe"},
    {0x03A9, "Ohm"},  {0x03BC, "u"},    {0x2013, "-"},   {0x2014, "-"},   {0x2018, "'"},    {0x2019, "'"},
    {0x201A, "'"},    {0x201C, "\""},   {0x201D, "\""},  {0x201E, "\""},  {0x2026, "..."},  {0x2126, "Ohm"},
    {0x2212, "-"},    {0x20AC, "EUR"},
};

// The ASCII text for one code point above 0x7F. Anything without a Latin base letter becomes "?".
inline std::string foldedCodePoint(std::uint32_t codePoint) {
  for (const auto& entry : kFoldSpecial) {
    if (entry.codePoint == codePoint) return entry.ascii;
  }
  if (codePoint >= 0xC0 && codePoint <= 0xFF) return std::string(1, kFoldLatin1[codePoint - 0xC0]);
  if (codePoint >= 0x100 && codePoint <= 0x17F) return std::string(1, kFoldLatinExtA[codePoint - 0x100]);
  return "?";
}

// Returns `text` in ASCII. Malformed UTF-8 becomes "?" one byte at a time.
inline std::string foldToAscii(const std::string& text) {
  std::string out;
  std::size_t at = 0;
  while (at < text.size()) {
    std::uint32_t codePoint = 0;
    if (!nextUtf8CodePoint(text, at, codePoint)) {
      out.push_back('?');
      continue;
    }
    if (codePoint < 0x80U) {
      out.push_back(static_cast<char>(codePoint));
    } else {
      out += foldedCodePoint(codePoint);
    }
  }
  return out;
}

}  // namespace inventatory
