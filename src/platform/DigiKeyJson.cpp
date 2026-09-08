// Inventatory - Bounded DigiKey JSON parser and structural accessors.

#include "platform/DigiKeyApiPrivate.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace inventatory {
using namespace std;
namespace digikey_detail {

class JsonParser {
 public:
  explicit JsonParser(string_view text) : text_(text) {}

  bool parse(JsonPtr& out, string* error) {
    skipWhitespace();
    out = parseValue(error);
    if (out == nullptr) {
      return false;
    }
    skipWhitespace();
    if (!eof()) {
      if (error != nullptr) {
        *error = "Unexpected trailing JSON content";
      }
      return false;
    }
    return true;
  }

 private:
  JsonPtr parseValue(string* error, unsigned depth = 0) {
    if (depth > kMaximumJsonDepth) {
      if (error != nullptr) *error = "JSON nesting exceeds the safety limit";
      return nullptr;
    }
    skipWhitespace();
    if (eof()) {
      if (error != nullptr) {
        *error = "Unexpected end of JSON";
      }
      return nullptr;
    }

    const char ch = peek();
    if (ch == '"') {
      string value;
      if (!parseString(value, error)) {
        return nullptr;
      }
      return make_shared<JsonValue>(move(value));
    }
    if (ch == '{') {
      return parseObject(error, depth);
    }
    if (ch == '[') {
      return parseArray(error, depth);
    }
    if (isdigit(static_cast<unsigned char>(ch)) || ch == '-') {
      string value;
      if (!parseNumber(value, error)) {
        return nullptr;
      }
      return make_shared<JsonValue>(JsonValue::Number{move(value)});
    }
    if (matchLiteral("true")) {
      return make_shared<JsonValue>(true);
    }
    if (matchLiteral("false")) {
      return make_shared<JsonValue>(false);
    }
    if (matchLiteral("null")) {
      return make_shared<JsonValue>();
    }

    if (error != nullptr) {
      *error = "Invalid JSON token";
    }
    return nullptr;
  }

  JsonPtr parseObject(string* error, unsigned depth) {
    if (!consume('{')) {
      return nullptr;
    }
    auto value = make_shared<JsonValue>();
    JsonValue::Object object;
    skipWhitespace();
    if (consume('}')) {
      value->data = move(object);
      return value;
    }

    for (;;) {
      string key;
      if (!parseString(key, error)) {
        return nullptr;
      }
      skipWhitespace();
      if (!consume(':')) {
        if (error != nullptr) {
          *error = "Expected ':' in JSON object";
        }
        return nullptr;
      }
      if (object.size() >= kMaximumJsonContainerEntries) {
        if (error != nullptr) *error = "JSON object has too many members";
        return nullptr;
      }
      auto child = parseValue(error, depth + 1U);
      if (child == nullptr) {
        return nullptr;
      }
      if (!object.emplace(move(key), move(child)).second) {
        if (error != nullptr) *error = "JSON object contains a duplicate key";
        return nullptr;
      }
      skipWhitespace();
      if (consume('}')) {
        value->data = move(object);
        return value;
      }
      if (!consume(',')) {
        if (error != nullptr) {
          *error = "Expected ',' or '}' in JSON object";
        }
        return nullptr;
      }
      skipWhitespace();
    }
  }

  JsonPtr parseArray(string* error, unsigned depth) {
    if (!consume('[')) {
      return nullptr;
    }
    auto value = make_shared<JsonValue>();
    JsonValue::Array array;
    skipWhitespace();
    if (consume(']')) {
      value->data = move(array);
      return value;
    }

    for (;;) {
      if (array.size() >= kMaximumJsonContainerEntries) {
        if (error != nullptr) *error = "JSON array has too many values";
        return nullptr;
      }
      auto child = parseValue(error, depth + 1U);
      if (child == nullptr) {
        return nullptr;
      }
      array.push_back(move(child));
      skipWhitespace();
      if (consume(']')) {
        value->data = move(array);
        return value;
      }
      if (!consume(',')) {
        if (error != nullptr) {
          *error = "Expected ',' or ']' in JSON array";
        }
        return nullptr;
      }
      skipWhitespace();
    }
  }

  bool parseString(string& out, string* error) {
    if (!consume('"')) {
      if (error != nullptr) {
        *error = "Expected JSON string";
      }
      return false;
    }

    out.clear();
    const auto append = [&](char value) {
      if (out.size() >= kMaximumDigiKeyFieldBytes) {
        if (error != nullptr) *error = "JSON string exceeds the 4 KiB field limit";
        return false;
      }
      out.push_back(value);
      return true;
    };
    while (!eof()) {
      const char ch = advance();
      if (ch == '"') {
        return true;
      }
      if (ch != '\\') {
        if (static_cast<unsigned char>(ch) < 0x20U) {
          if (error != nullptr) *error = "Unescaped control character in JSON string";
          return false;
        }
        if (!append(ch)) return false;
        continue;
      }

      if (eof()) {
        if (error != nullptr) {
          *error = "Invalid JSON escape";
        }
        return false;
      }

      const char escaped = advance();
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          if (!append(escaped)) return false;
          break;
        case 'b':
          if (!append('\b')) return false;
          break;
        case 'f':
          if (!append('\f')) return false;
          break;
        case 'n':
          if (!append('\n')) return false;
          break;
        case 'r':
          if (!append('\r')) return false;
          break;
        case 't':
          if (!append('\t')) return false;
          break;
        case 'u': {
          const auto hexDigit = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
          };
          auto readCodeUnit = [&](unsigned& codeUnit) {
            if (text_.size() - pos_ < 4U) return false;
            codeUnit = 0;
            for (unsigned index = 0; index < 4U; ++index) {
              const int digit = hexDigit(advance());
              if (digit < 0) return false;
              codeUnit = (codeUnit << 4U) | static_cast<unsigned>(digit);
            }
            return true;
          };
          unsigned codeUnit = 0;
          if (!readCodeUnit(codeUnit)) {
            if (error != nullptr) *error = "Invalid JSON Unicode escape";
            return false;
          }
          unsigned codePoint = codeUnit;
          if (codeUnit >= 0xD800U && codeUnit <= 0xDBFFU) {
            if (text_.size() - pos_ < 6U || text_[pos_] != '\\' || text_[pos_ + 1U] != 'u') {
              if (error != nullptr) *error = "JSON high surrogate is missing its pair";
              return false;
            }
            pos_ += 2U;
            unsigned low = 0;
            if (!readCodeUnit(low) || low < 0xDC00U || low > 0xDFFFU) {
              if (error != nullptr) *error = "Invalid JSON surrogate pair";
              return false;
            }
            codePoint = 0x10000U + ((codeUnit - 0xD800U) << 10U) + (low - 0xDC00U);
          } else if (codeUnit >= 0xDC00U && codeUnit <= 0xDFFFU) {
            if (error != nullptr) *error = "JSON string contains an unpaired low surrogate";
            return false;
          }

          if (codePoint <= 0x7FU) {
            if (!append(static_cast<char>(codePoint))) return false;
          } else if (codePoint <= 0x7FFU) {
            if (!append(static_cast<char>(0xC0U | (codePoint >> 6U))) ||
                !append(static_cast<char>(0x80U | (codePoint & 0x3FU)))) {
              return false;
            }
          } else if (codePoint <= 0xFFFFU) {
            if (!append(static_cast<char>(0xE0U | (codePoint >> 12U))) ||
                !append(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU))) ||
                !append(static_cast<char>(0x80U | (codePoint & 0x3FU)))) {
              return false;
            }
          } else {
            if (!append(static_cast<char>(0xF0U | (codePoint >> 18U))) ||
                !append(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU))) ||
                !append(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU))) ||
                !append(static_cast<char>(0x80U | (codePoint & 0x3FU)))) {
              return false;
            }
          }
          break;
        }
        default:
          if (error != nullptr) *error = "Invalid JSON escape";
          return false;
      }
    }

    if (error != nullptr) {
      *error = "Unterminated JSON string";
    }
    return false;
  }

  bool parseNumber(string& out, string* error) {
    const size_t start = pos_;
    if (peek() == '-') {
      advance();
    }
    if (peek() == '0') {
      advance();
      if (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
        if (error != nullptr) *error = "Invalid JSON number";
        return false;
      }
    } else {
      if (eof() || !isdigit(static_cast<unsigned char>(peek()))) {
        if (error != nullptr) *error = "Invalid JSON number";
        return false;
      }
      while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }
    if (!eof() && peek() == '.') {
      advance();
      if (eof() || !isdigit(static_cast<unsigned char>(peek()))) {
        if (error != nullptr) *error = "Invalid JSON number";
        return false;
      }
      while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }
    if (!eof() && (peek() == 'e' || peek() == 'E')) {
      advance();
      if (!eof() && (peek() == '+' || peek() == '-')) {
        advance();
      }
      if (eof() || !isdigit(static_cast<unsigned char>(peek()))) {
        if (error != nullptr) *error = "Invalid JSON number";
        return false;
      }
      while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }

    if (pos_ == start || (pos_ == start + 1U && text_[start] == '-')) {
      if (error != nullptr) {
        *error = "Invalid JSON number";
      }
      return false;
    }
    if (pos_ - start > kMaximumDigiKeyFieldBytes) {
      if (error != nullptr) *error = "JSON number exceeds the 4 KiB field limit";
      return false;
    }

    out.assign(text_.substr(start, pos_ - start));
    return true;
  }

  bool matchLiteral(string_view literal) {
    if (text_.substr(pos_, literal.size()) != literal) {
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  bool consume(char expected) {
    if (eof() || text_[pos_] != expected) {
      return false;
    }
    ++pos_;
    return true;
  }

  char advance() {
    return eof() ? '\0' : text_[pos_++];
  }

  char peek() const {
    return eof() ? '\0' : text_[pos_];
  }

  void skipWhitespace() {
    while (!eof() && isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  bool eof() const {
    return pos_ >= text_.size();
  }

  string_view text_;
  size_t pos_ = 0;
};

const JsonValue* asValue(const JsonPtr& value) {
  return value ? value.get() : nullptr;
}

const JsonValue::Object* asObject(const JsonPtr& value) {
  if (const auto* json = asValue(value); json != nullptr) {
    return get_if<JsonValue::Object>(&json->data);
  }
  return nullptr;
}

const JsonValue::Array* asArray(const JsonPtr& value) {
  if (const auto* json = asValue(value); json != nullptr) {
    return get_if<JsonValue::Array>(&json->data);
  }
  return nullptr;
}

string valueText(const JsonPtr& value) {
  if (const auto* json = asValue(value); json != nullptr) {
    if (const auto* text = get_if<string>(&json->data)) {
      if (text->size() > kMaximumDigiKeyFieldBytes) return {};
      return trimCopy(*text);
    }
    if (const auto* number = get_if<JsonValue::Number>(&json->data)) {
      if (number->text.size() > kMaximumDigiKeyFieldBytes) return {};
      return number->text;
    }
    if (const auto* flag = get_if<bool>(&json->data)) {
      return *flag ? "true" : "false";
    }
  }
  return {};
}

const JsonPtr* findMember(const JsonPtr& object, const string& key) {
  const auto* jsonObject = asObject(object);
  if (jsonObject == nullptr) {
    return nullptr;
  }
  const auto it = jsonObject->find(key);
  return it == jsonObject->end() ? nullptr : &it->second;
}

optional<string> readPath(const JsonPtr& root, initializer_list<const char*> path) {
  JsonPtr current = root;
  for (const auto* element : path) {
    const auto* next = findMember(current, element);
    if (next == nullptr) {
      return nullopt;
    }
    current = *next;
  }
  const auto text = valueText(current);
  if (text.empty()) {
    return nullopt;
  }
  return text;
}

optional<string> readStringPath(const JsonPtr& root, initializer_list<const char*> path) {
  JsonPtr current = root;
  for (const auto* element : path) {
    const auto* next = findMember(current, element);
    if (next == nullptr) return nullopt;
    current = *next;
  }
  const auto* json = asValue(current);
  const auto* text = json == nullptr ? nullptr : get_if<string>(&json->data);
  if (text == nullptr || text->empty() || text->size() > kMaximumDigiKeyFieldBytes) return nullopt;
  return trimCopy(*text);
}

optional<string> readFirstMember(const JsonPtr& root, initializer_list<const char*> keys) {
  for (const auto* key : keys) {
    if (const auto* member = findMember(root, key); member != nullptr) {
      const auto text = valueText(*member);
      if (!text.empty()) {
        return text;
      }
    }
  }
  return nullopt;
}

void appendCategoryPathFromNode(const JsonPtr& node, vector<string>& path) {
  if (node == nullptr || path.size() >= kMaximumCategoryPathEntries) {
    return;
  }
  if (const auto name = readFirstMember(node, {"Name", "CategoryName"}); name.has_value()) {
    if (path.empty() || path.back() != *name) {
      path.push_back(*name);
    }
  }
  if (const auto* children = asArray(findMember(node, "Children") == nullptr ? nullptr : *findMember(node, "Children"));
      children != nullptr) {
    for (const auto& child : *children) {
      if (path.size() >= kMaximumCategoryPathEntries) break;
      appendCategoryPathFromNode(child, path);
    }
  }
}

vector<string> extractCategoryPath(const JsonPtr& product) {
  for (const auto* key : {"Category", "ProductCategory"}) {
    if (const auto* node = findMember(product, key); node != nullptr) {
      vector<string> path;
      appendCategoryPathFromNode(*node, path);
      if (!path.empty()) {
        return path;
      }
    }
  }

  if (const auto* taxonomy = asArray(findMember(product, "LimitedTaxonomy") == nullptr
                                        ? nullptr
                                        : *findMember(product, "LimitedTaxonomy"));
      taxonomy != nullptr && !taxonomy->empty()) {
    vector<string> path;
    for (const auto& node : *taxonomy) {
      appendCategoryPathFromNode(node, path);
    }
    if (!path.empty()) {
      return path;
    }
  }

  if (const auto* classifications = asArray(findMember(product, "Classifications") == nullptr
                                               ? nullptr
                                               : *findMember(product, "Classifications"));
      classifications != nullptr && !classifications->empty()) {
    vector<string> path;
    for (const auto& node : *classifications) {
      appendCategoryPathFromNode(node, path);
    }
    if (!path.empty()) {
      return path;
    }
  }

  return {};
}

optional<JsonPtr> parseJson(const string& body, string* error) {
  if (body.size() > kMaximumDigiKeyResponseBytes) {
    if (error != nullptr) *error = "DigiKey response exceeds the 4 MiB safety limit";
    return nullopt;
  }
  JsonParser parser(body);
  JsonPtr root;
  if (!parser.parse(root, error)) {
    return nullopt;
  }
  return root;
}

}  // namespace digikey_detail
}  // namespace inventatory

#endif
