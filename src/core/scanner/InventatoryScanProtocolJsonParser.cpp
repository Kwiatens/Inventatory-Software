// Inventatory - bounded JSON syntax and member-location parsing.

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

}  // namespace inventatory::scan_protocol_detail
