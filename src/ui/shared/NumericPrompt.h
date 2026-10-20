// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Digit entry for numeric prompts with a pre-filled default. The first digit
// replaces the default ("5" then "10" gives 10, not 510).

#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace inventatory::numeric_prompt {

// Returns false when `ch` is not a digit or the buffer is full.
inline bool applyDigit(std::string& buffer, char ch, bool& replacePending, std::size_t maxDigits) {
  if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return false;
  if (replacePending) {
    buffer.assign(1, ch);
    replacePending = false;
    return true;
  }
  if (buffer.size() >= maxDigits) return false;
  buffer.push_back(ch);
  return true;
}

// Backspace ends replace mode and edits the default as ordinary text.
inline bool applyBackspace(std::string& buffer, bool& replacePending) {
  replacePending = false;
  if (buffer.empty()) return false;
  buffer.pop_back();
  return true;
}

}  // namespace inventatory::numeric_prompt
