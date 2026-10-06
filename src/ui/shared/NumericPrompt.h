// Inventatory - digit entry for numeric prompts that start with a default value.
//
// A prompt that is pre-filled with the current or suggested quantity behaves
// like a selected field: the first digit replaces the default instead of being
// appended to it ("5" then "10" must give 10, never 510). Backspace edits the
// default as ordinary text and ends the replace behaviour.

#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace inventatory::numeric_prompt {

// Appends `ch` when it is a digit and the buffer has room. While
// `replacePending` is set the buffer is replaced instead. Returns true when
// the buffer changed.
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

// Backspace on a pre-filled default edits it character by character.
inline bool applyBackspace(std::string& buffer, bool& replacePending) {
  replacePending = false;
  if (buffer.empty()) return false;
  buffer.pop_back();
  return true;
}

}  // namespace inventatory::numeric_prompt
