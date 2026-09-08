// Inventatory - bounded JSON parsing and encoding for Scan R1 messages.

#include "core/InventatoryScanProtocolPrivate.h"

#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory::scan_protocol_detail {

using namespace std;


optional<string> jsonString(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition || *valuePosition >= body.size() || body[*valuePosition] != '"') return nullopt;
  auto position = *valuePosition;
  string value;
  bool escaped = false;
  for (++position; position < body.size(); ++position) {
    const char ch = body[position];
    if (escaped) {
      switch (ch) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u':
          if (position + 4 < body.size()) {
            const auto hi = hexDigit(body[position + 1]);
            const auto h2 = hexDigit(body[position + 2]);
            const auto h3 = hexDigit(body[position + 3]);
            const auto lo = hexDigit(body[position + 4]);
            if (hi >= 0 && h2 >= 0 && h3 >= 0 && lo >= 0) {
              const auto codepoint = static_cast<unsigned>(hi << 12 | h2 << 8 | h3 << 4 | lo);
              if (codepoint <= 0xFFU) {
                value.push_back(static_cast<char>(codepoint));
                position += 4;
                escaped = false;
                continue;
              }
            }
          }
          value.push_back(ch);
          break;
        default: value.push_back(ch); break;
      }
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return value;
    } else {
      value.push_back(ch);
    }
  }
  return nullopt;
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

optional<bool> jsonBool(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition) return nullopt;
  auto position = *valuePosition;
  while (position < body.size() && isspace(static_cast<unsigned char>(body[position]))) ++position;
  const auto validTerminator = [&](size_t end) {
    return end == body.size() || body[end] == ',' || body[end] == '}' || body[end] == ']' ||
           isspace(static_cast<unsigned char>(body[end])) != 0;
  };
  if (body.compare(position, 4, "true") == 0 && validTerminator(position + 4U)) return true;
  if (body.compare(position, 5, "false") == 0 && validTerminator(position + 5U)) return false;
  return nullopt;
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
    if ((*array)[position] != '"') return nullopt;
    string value;
    bool escaped = false;
    bool closed = false;
    size_t index = position + 1;
    for (; index < array->size(); ++index) {
      const char ch = (*array)[index];
      if (escaped) {
        if (ch == 'u') {
          if (index + 4 >= array->size() || hexDigit((*array)[index + 1]) < 0 ||
              hexDigit((*array)[index + 2]) < 0 || hexDigit((*array)[index + 3]) < 0 ||
              hexDigit((*array)[index + 4]) < 0) return nullopt;
          value.push_back(ch);
          index += 4;
        } else if (ch != '"' && ch != '\\' && ch != '/' && ch != 'b' && ch != 'f' && ch != 'n' &&
                   ch != 'r' && ch != 't') {
          return nullopt;
        } else {
          value.push_back(ch);
        }
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        closed = true;
        ++index;
        break;
      } else {
        if (static_cast<unsigned char>(ch) < 0x20U) return nullopt;
        value.push_back(ch);
      }
    }
    if (!closed || escaped) return nullopt;
    values.push_back(move(value));
    position = index;
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
