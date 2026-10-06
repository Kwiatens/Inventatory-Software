// Inventatory - bounded JSON parsing and encoding for Scan R1 messages.

#include "core/scanner/InventatoryScanProtocolPrivate.h"

#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory::scan_protocol_detail {

using namespace std;


namespace {

optional<unsigned> hexQuad(const string& text, size_t at) {
  if (at + 4U > text.size()) return nullopt;
  unsigned value = 0;
  for (size_t offset = 0; offset < 4U; ++offset) {
    const int digit = hexDigit(text[at + offset]);
    if (digit < 0) return nullopt;
    value = (value << 4U) | static_cast<unsigned>(digit);
  }
  return value;
}

void appendUtf8(string& out, unsigned codepoint) {
  if (codepoint < 0x80U) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else if (codepoint < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
  }
}

}  // namespace

// The one JSON string decoder for Scan R1 messages. `position` is the opening quote; on success it is
// moved past the closing quote. Escapes decode to UTF-8 (surrogate pairs included). Raw control
// characters, a NUL escape (it would truncate the value at the SQLite binding), unpaired surrogates and
// unknown escapes are rejected rather than altered.
optional<string> decodeJsonStringLiteral(const string& text, size_t& position) {
  if (position >= text.size() || text[position] != '"') return nullopt;
  string value;
  for (size_t index = position + 1U; index < text.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(text[index]);
    if (ch == '"') {
      position = index + 1U;
      return value;
    }
    if (ch < 0x20U) return nullopt;
    if (ch != '\\') {
      value.push_back(static_cast<char>(ch));
      continue;
    }
    if (++index >= text.size()) return nullopt;
    switch (text[index]) {
      case '"': value.push_back('"'); break;
      case '\\': value.push_back('\\'); break;
      case '/': value.push_back('/'); break;
      case 'b': value.push_back('\b'); break;
      case 'f': value.push_back('\f'); break;
      case 'n': value.push_back('\n'); break;
      case 'r': value.push_back('\r'); break;
      case 't': value.push_back('\t'); break;
      case 'u': {
        const auto unit = hexQuad(text, index + 1U);
        if (!unit) return nullopt;
        index += 4U;
        unsigned codepoint = *unit;
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
          if (index + 2U >= text.size() || text[index + 1U] != '\\' || text[index + 2U] != 'u') return nullopt;
          const auto low = hexQuad(text, index + 3U);
          if (!low || *low < 0xDC00U || *low > 0xDFFFU) return nullopt;
          index += 6U;
          codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (*low - 0xDC00U);
        } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
          return nullopt;
        }
        if (codepoint == 0U) return nullopt;
        appendUtf8(value, codepoint);
        break;
      }
      default: return nullopt;
    }
  }
  return nullopt;
}

optional<string> jsonString(const string& body, const string& key) {
  auto position = jsonMemberValuePosition(body, key);
  if (!position) return nullopt;
  return decodeJsonStringLiteral(body, *position);
}

optional<int> jsonInt(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition) return nullopt;
  auto position = *valuePosition;
  while (position < body.size() && isspace(static_cast<unsigned char>(body[position]))) ++position;
  const auto begin = position;
  if (position < body.size() && (body[position] == '-' || body[position] == '+')) ++position;
  while (position < body.size() && isdigit(static_cast<unsigned char>(body[position]))) ++position;
  if (position == begin || (position == begin + 1 && (body[begin] == '-' || body[begin] == '+'))) return nullopt;
  if (position < body.size() && body[position] != ',' && body[position] != '}' && body[position] != ']' &&
      isspace(static_cast<unsigned char>(body[position])) == 0) return nullopt;
  try {
    const auto parsed = stoll(body.substr(begin, position - begin));
    if (parsed < numeric_limits<int>::min() || parsed > numeric_limits<int>::max()) return nullopt;
    return static_cast<int>(parsed);
  } catch (...) {
    return nullopt;
  }
}

optional<string> jsonArrayBody(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition || *valuePosition >= body.size() || body[*valuePosition] != '[') return nullopt;
  const auto begin = *valuePosition;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  for (auto index = begin; index < body.size(); ++index) {
    const char ch = body[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == '[') ++depth;
    else if (ch == ']' && --depth == 0) return body.substr(begin + 1, index - begin - 1);
  }
  return nullopt;
}

optional<string> jsonObjectBody(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition || *valuePosition >= body.size() || body[*valuePosition] != '{') return nullopt;
  const auto begin = *valuePosition;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  for (auto index = begin; index < body.size(); ++index) {
    const char ch = body[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == '{') ++depth;
    else if (ch == '}' && --depth == 0) return body.substr(begin, index - begin + 1);
  }
  return nullopt;
}

optional<vector<string>> jsonObjectArray(const string& body, const string& key) {
  const auto array = jsonArrayBody(body, key);
  if (!array) return nullopt;
  vector<string> objects;
  size_t position = 0;
  const auto skipWhitespace = [&]() {
    while (position < array->size() && isspace(static_cast<unsigned char>((*array)[position])) != 0) ++position;
  };
  skipWhitespace();
  if (position == array->size()) return objects;
  while (position < array->size()) {
    if ((*array)[position] != '{') return nullopt;
    const size_t begin = position;
    vector<char> nesting;
    bool inString = false;
    bool escaped = false;
    for (; position < array->size(); ++position) {
      const char ch = (*array)[position];
      if (inString) {
        if (escaped) {
          escaped = false;
        } else if (ch == '\\') {
          escaped = true;
        } else if (ch == '"') {
          inString = false;
        }
        continue;
      }
      if (ch == '"') {
        inString = true;
      } else if (ch == '{' || ch == '[') {
        if (nesting.size() >= kMaxJsonNestingDepth) return nullopt;
        nesting.push_back(ch);
      } else if (ch == '}' || ch == ']') {
        if (nesting.empty() || (ch == '}' && nesting.back() != '{') ||
            (ch == ']' && nesting.back() != '[')) {
          return nullopt;
        }
        nesting.pop_back();
        if (nesting.empty()) {
          ++position;
          break;
        }
      }
    }
    if (inString || escaped || !nesting.empty() || position <= begin) return nullopt;
    objects.push_back(array->substr(begin, position - begin));
    skipWhitespace();
    if (position == array->size()) return objects;
    if ((*array)[position] != ',') return nullopt;
    ++position;
    skipWhitespace();
    if (position == array->size()) return nullopt;
  }
  return objects;
}

optional<vector<string>> jsonStringArray(const string& body, const string& key) {
  const auto array = jsonArrayBody(body, key);
  if (!array) return nullopt;
  vector<string> values;
  size_t position = 0;
  const auto skipWhitespace = [&]() {
    while (position < array->size() && isspace(static_cast<unsigned char>((*array)[position])) != 0) ++position;
  };
  skipWhitespace();
  if (position == array->size()) return values;
  while (position < array->size()) {
    auto value = decodeJsonStringLiteral(*array, position);
    if (!value) return nullopt;
    values.push_back(move(*value));
    skipWhitespace();
    if (position == array->size()) return values;
    if ((*array)[position] != ',') return nullopt;
    ++position;
    skipWhitespace();
    if (position == array->size()) return nullopt;
  }
  return values;
}

}  // namespace inventatory::scan_protocol_detail
