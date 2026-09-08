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

constexpr size_t kMaxJsonNestingDepth = 32;
constexpr size_t kMaxJsonBodyBytes = 64U * 1024U;

int hexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

string jsonEscape(const string& value) {
  ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20U) {
          static constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4U) & 0x0fU] << kHex[ch & 0x0fU];
        } else {
          out << static_cast<char>(ch);
        }
        break;
    }
  }
  return out.str();
}

class JsonSyntaxParser {
 public:
  explicit JsonSyntaxParser(const string& input) : input_(input) {}

  bool parseObjectDocument() {
    skipWhitespace();
    if (!parseObject(0)) return false;
    skipWhitespace();
    return position_ == input_.size();
  }

 private:
  void skipWhitespace() {
    while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' ||
                                         input_[position_] == '\r' || input_[position_] == '\n')) {
      ++position_;
    }
  }

  bool parseString() {
    if (position_ >= input_.size() || input_[position_] != '"') return false;
    ++position_;
    while (position_ < input_.size()) {
      const unsigned char ch = static_cast<unsigned char>(input_[position_++]);
      if (ch == '"') return true;
      if (ch < 0x20U) return false;
      if (ch != '\\') continue;
      if (position_ >= input_.size()) return false;
      const char escaped = input_[position_++];
      if (escaped == 'u') {
        if (position_ + 4U > input_.size() || hexDigit(input_[position_]) < 0 ||
            hexDigit(input_[position_ + 1U]) < 0 || hexDigit(input_[position_ + 2U]) < 0 ||
            hexDigit(input_[position_ + 3U]) < 0) {
          return false;
        }
        position_ += 4U;
      } else if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' && escaped != 'f' &&
                 escaped != 'n' && escaped != 'r' && escaped != 't') {
        return false;
      }
    }
    return false;
  }

  bool parseNumber() {
    const auto begin = position_;
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ >= input_.size()) return false;
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && isdigit(static_cast<unsigned char>(input_[position_]))) return false;
    } else {
      if (!isdigit(static_cast<unsigned char>(input_[position_])) || input_[position_] == '0') return false;
      while (position_ < input_.size() && isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      const auto fractionBegin = position_;
      while (position_ < input_.size() && isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
      if (position_ == fractionBegin) return false;
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
      const auto exponentBegin = position_;
      while (position_ < input_.size() && isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
      if (position_ == exponentBegin) return false;
    }
    return position_ > begin;
  }

  bool parseLiteral(const char* literal) {
    const size_t length = strlen(literal);
    if (input_.compare(position_, length, literal) != 0) return false;
    position_ += length;
    return true;
  }

  bool parseValue(size_t depth) {
    skipWhitespace();
    if (position_ >= input_.size()) return false;
    switch (input_[position_]) {
      case '{': return parseObject(depth);
      case '[': return parseArray(depth);
      case '"': return parseString();
      case 't': return parseLiteral("true");
      case 'f': return parseLiteral("false");
      case 'n': return parseLiteral("null");
      default:
        return input_[position_] == '-' || isdigit(static_cast<unsigned char>(input_[position_]))
                   ? parseNumber()
                   : false;
    }
  }

  bool parseObject(size_t depth) {
    if (depth >= kMaxJsonNestingDepth || position_ >= input_.size() || input_[position_] != '{') return false;
    ++position_;
    skipWhitespace();
    if (position_ < input_.size() && input_[position_] == '}') {
      ++position_;
      return true;
    }
    while (true) {
      if (!parseString()) return false;
      skipWhitespace();
      if (position_ >= input_.size() || input_[position_] != ':') return false;
      ++position_;
      if (!parseValue(depth + 1U)) return false;
      skipWhitespace();
      if (position_ >= input_.size()) return false;
      if (input_[position_] == '}') {
        ++position_;
        return true;
      }
      if (input_[position_] != ',') return false;
      ++position_;
      skipWhitespace();
    }
  }

  bool parseArray(size_t depth) {
    if (depth >= kMaxJsonNestingDepth || position_ >= input_.size() || input_[position_] != '[') return false;
    ++position_;
    skipWhitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      if (!parseValue(depth + 1U)) return false;
      skipWhitespace();
      if (position_ >= input_.size()) return false;
      if (input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (input_[position_] != ',') return false;
      ++position_;
      skipWhitespace();
    }
  }

  const string& input_;
  size_t position_ = 0;
};

bool jsonObjectIsComplete(const string& body) {
  if (body.size() > kMaxJsonBodyBytes) return false;
  return JsonSyntaxParser(body).parseObjectDocument();
}

optional<size_t> jsonMemberValuePosition(const string& body, const string& key) {
  if (!jsonObjectIsComplete(body)) {
    return nullopt;
  }

  int objectDepth = 0;
  int arrayDepth = 0;
  optional<size_t> valuePosition;
  for (size_t index = 0; index < body.size(); ++index) {
    const char ch = body[index];
    if (ch == '{') {
      ++objectDepth;
      continue;
    }
    if (ch == '}') {
      --objectDepth;
      continue;
    }
    if (ch == '[') {
      ++arrayDepth;
      continue;
    }
    if (ch == ']') {
      --arrayDepth;
      continue;
    }
    if (ch != '"') {
      continue;
    }

    const auto stringStart = index + 1;
    bool escaped = false;
    for (++index; index < body.size(); ++index) {
      if (escaped) {
        escaped = false;
      } else if (body[index] == '\\') {
        escaped = true;
      } else if (body[index] == '"') {
        break;
      }
    }
    if (index >= body.size()) return nullopt;

    size_t afterName = index + 1;
    while (afterName < body.size() && isspace(static_cast<unsigned char>(body[afterName]))) ++afterName;
    if (objectDepth != 1 || arrayDepth != 0 || afterName >= body.size() || body[afterName] != ':' ||
        body.substr(stringStart, index - stringStart) != key) {
      continue;
    }
    ++afterName;
    while (afterName < body.size() && isspace(static_cast<unsigned char>(body[afterName]))) ++afterName;
    if (valuePosition.has_value()) {
      return nullopt;
    }
    valuePosition = afterName;
  }
  return valuePosition;
}

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
