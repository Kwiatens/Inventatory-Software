// Inventatory - Hardware Inventory Management System
// DigiKey API response parsing and metadata synchronization.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/DigiKeyApi.h"
#include "app/AppSettings.h"
#include "platform/CredentialStore.h"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <memory>
#include <regex>
#include <string_view>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <variant>

#pragma comment(lib, "winhttp.lib")

namespace inventatory {

using namespace std;

namespace {

string trimCopy(string value) {
  return trim(value);
}

string encodeComponent(const string& value, bool formEncoding) {
  ostringstream out;
  for (unsigned char ch : value) {
    if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      out << static_cast<char>(ch);
    } else if (ch == ' ' && formEncoding) {
      out << '+';
    } else if (ch == ' ' && !formEncoding) {
      out << "%20";
    } else {
      out << '%' << uppercase << hex << setw(2) << setfill('0') << static_cast<int>(ch)
          << nouppercase << dec;
    }
  }
  return out.str();
}

string encodeFormValue(const string& value) {
  return encodeComponent(value, true);
}

string encodePathSegment(const string& value) {
  return encodeComponent(value, false);
}

string escapeJsonString(const string& value) {
  ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          out << "\\u" << hex << setw(4) << setfill('0') << static_cast<int>(ch) << dec;
        } else {
          out << static_cast<char>(ch);
        }
        break;
    }
  }
  return out.str();
}

wstring widen(const string& value) {
  if (value.empty()) {
    return {};
  }
  const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
  if (required <= 0) {
    return {};
  }
  wstring out(static_cast<size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), required);
  return out;
}

string narrow(const wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  string out(static_cast<size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), required, nullptr, nullptr);
  return out;
}

struct HttpResponse {
  DWORD statusCode = 0;
  string body;
};

string httpErrorMessage(const string& prefix) {
  const DWORD code = GetLastError();
  ostringstream out;
  out << prefix << " (Win32 " << code << ")";
  return out.str();
}

bool requestHttp(const wstring& method, const wstring& url, const wstring& headers, const string& body,
                 HttpResponse& response, string* error) {
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to parse DigiKey URL");
    }
    return false;
  }

  wstring host(components.lpszHostName, components.dwHostNameLength);
  wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.dwExtraInfoLength > 0) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }

  HINTERNET session = WinHttpOpen(L"Inventatory DigiKey client/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to open WinHTTP session");
    }
    return false;
  }

  WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);

  HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (connection == nullptr) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to connect to DigiKey");
    }
    WinHttpCloseHandle(session);
    return false;
  }

  const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(connection, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (request == nullptr) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to open DigiKey request");
    }
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  if (!headers.empty() &&
      !WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to apply DigiKey request headers");
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  const void* bodyData = body.empty() ? nullptr : body.data();
  const DWORD bodySize = static_cast<DWORD>(body.size());
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<void*>(bodyData), bodySize, bodySize, 0)) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to send DigiKey request");
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  if (!WinHttpReceiveResponse(request, nullptr)) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to receive DigiKey response");
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                           &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to query DigiKey response status");
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  string bodyText;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      if (error != nullptr) {
        *error = httpErrorMessage("Unable to read DigiKey response");
      }
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      return false;
    }

    if (available == 0) {
      break;
    }

    const size_t current = bodyText.size();
    bodyText.resize(current + available);
    DWORD read = 0;
    if (!WinHttpReadData(request, bodyText.data() + current, available, &read)) {
      if (error != nullptr) {
        *error = httpErrorMessage("Unable to copy DigiKey response");
      }
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      return false;
    }
    bodyText.resize(current + read);
  }

  response.statusCode = statusCode;
  response.body = move(bodyText);

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return true;
}

struct JsonValue {
  using Object = unordered_map<string, shared_ptr<JsonValue>>;
  using Array = vector<shared_ptr<JsonValue>>;

  JsonValue() = default;
  explicit JsonValue(string text) : data(move(text)) {}
  explicit JsonValue(bool flag) : data(flag) {}
  explicit JsonValue(Object object) : data(move(object)) {}
  explicit JsonValue(Array array) : data(move(array)) {}

  variant<nullptr_t, bool, string, Object, Array> data = nullptr;
};

using JsonPtr = shared_ptr<JsonValue>;

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
  JsonPtr parseValue(string* error) {
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
      return parseObject(error);
    }
    if (ch == '[') {
      return parseArray(error);
    }
    if (isdigit(static_cast<unsigned char>(ch)) || ch == '-') {
      string value;
      if (!parseNumber(value, error)) {
        return nullptr;
      }
      return make_shared<JsonValue>(move(value));
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

  JsonPtr parseObject(string* error) {
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
      auto child = parseValue(error);
      if (child == nullptr) {
        return nullptr;
      }
      object.emplace(move(key), move(child));
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

  JsonPtr parseArray(string* error) {
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
      auto child = parseValue(error);
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
    while (!eof()) {
      const char ch = advance();
      if (ch == '"') {
        return true;
      }
      if (ch != '\\') {
        out.push_back(ch);
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
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          for (int index = 0; index < 4 && !eof(); ++index) {
            advance();
          }
          break;
        default:
          out.push_back(escaped);
          break;
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
    while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
      advance();
    }
    if (!eof() && peek() == '.') {
      advance();
      while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }
    if (!eof() && (peek() == 'e' || peek() == 'E')) {
      advance();
      if (!eof() && (peek() == '+' || peek() == '-')) {
        advance();
      }
      while (!eof() && isdigit(static_cast<unsigned char>(peek()))) {
        advance();
      }
    }

    if (pos_ == start) {
      if (error != nullptr) {
        *error = "Invalid JSON number";
      }
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
      return trimCopy(*text);
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

optional<string> categoryNameFromNode(const JsonPtr& node) {
  if (node == nullptr) {
    return nullopt;
  }
  if (const auto name = readFirstMember(node, {"Name", "CategoryName"}); name.has_value()) {
    return name;
  }
  if (const auto* children = asArray(findMember(node, "Children") == nullptr ? nullptr : *findMember(node, "Children"));
      children != nullptr && !children->empty()) {
    return categoryNameFromNode(children->back());
  }
  return nullopt;
}

optional<string> extractCategoryName(const JsonPtr& product) {
  for (const auto* key : {"Category", "ProductCategory"}) {
    if (const auto* node = findMember(product, key); node != nullptr) {
      if (const auto category = categoryNameFromNode(*node); category.has_value()) {
        return category;
      }
    }
  }

  if (const auto* taxonomy = asArray(findMember(product, "LimitedTaxonomy") == nullptr
                                        ? nullptr
                                        : *findMember(product, "LimitedTaxonomy"));
      taxonomy != nullptr && !taxonomy->empty()) {
    return categoryNameFromNode(taxonomy->back());
  }

  if (const auto* classifications = asArray(findMember(product, "Classifications") == nullptr
                                               ? nullptr
                                               : *findMember(product, "Classifications"));
      classifications != nullptr && !classifications->empty()) {
    return categoryNameFromNode(classifications->back());
  }

  return nullopt;
}

string normalizeParameterKey(const string& value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return normalized;
}

bool looksLikeFrequencyValue(const string& value) {
  const auto normalized = normalizeParameterKey(value);
  return normalized.find("hz") != string::npos;
}

bool looksLikeInductanceValue(const string& value) {
  const auto normalized = normalizeParameterKey(value);
  if (normalized.empty() || normalized.find("hz") != string::npos) {
    return false;
  }

  if (normalized.find("uh") != string::npos || normalized.find("nh") != string::npos ||
      normalized.find("ph") != string::npos || normalized.find("henry") != string::npos) {
    return true;
  }

  return normalized.find_first_of("0123456789") != string::npos && !normalized.empty() && normalized.back() == 'h';
}

optional<string> readParameterText(const JsonPtr& entry, const string& label = {}) {
  vector<string> candidates;
  for (const auto* key : {"ParameterValue", "ValueText", "Value"}) {
    if (const auto* member = findMember(entry, key); member != nullptr) {
      const auto text = valueText(*member);
      if (!text.empty()) {
        candidates.push_back(text);
      }
    }
  }

  if (candidates.empty()) {
    return nullopt;
  }

  const auto normalizedLabel = normalizeParameterKey(label);
  const auto chooseFirstMatching = [&](auto predicate) -> optional<string> {
    for (const auto& candidate : candidates) {
      if (predicate(candidate)) {
        return candidate;
      }
    }
    return nullopt;
  };

  if (normalizedLabel.find("inductance") != string::npos || normalizedLabel == "l") {
    if (const auto inductance = chooseFirstMatching(looksLikeInductanceValue); inductance.has_value()) {
      return inductance;
    }
    if (const auto nonFrequency = chooseFirstMatching([&](const string& candidate) {
          return !looksLikeFrequencyValue(candidate);
        });
        nonFrequency.has_value()) {
      return nonFrequency;
    }
    return nullopt;
  }

  if (normalizedLabel.find("frequency") != string::npos || normalizedLabel == "f") {
    if (const auto frequency = chooseFirstMatching(looksLikeFrequencyValue); frequency.has_value()) {
      return frequency;
    }
  }

  return candidates.front();
}

bool looksLikePackagingType(const string& value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  if (normalized.empty()) {
    return false;
  }

  static const initializer_list<const char*> kPackagingTokens = {
      "tapeandreel", "cuttape", "digireel", "reel", "tube", "tray", "bulk", "bag", "strip", "ammo", "box",
      "loose", "pack"};
  return any_of(kPackagingTokens.begin(), kPackagingTokens.end(), [&](const char* token) {
    return normalized == token || normalized.find(token) != string::npos;
  });
}

optional<string> readParameterValue(const JsonPtr& product, initializer_list<const char*> names) {
  const auto* entries = asArray(findMember(product, "Parameters") == nullptr ? nullptr : *findMember(product, "Parameters"));
  if (entries == nullptr) {
    return nullopt;
  }

  const auto normalize = [](const string& value) {
    string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
      if (isalnum(ch)) {
        normalized.push_back(static_cast<char>(tolower(ch)));
      }
    }
    return normalized;
  };

  for (const auto& entry : *entries) {
    const auto label = readFirstMember(entry, {"Parameter", "ParameterText"});
    const auto value = readParameterText(entry, label.value_or(""));
    if (!label.has_value() || !value.has_value()) {
      continue;
    }

    const auto normalizedLabel = normalize(*label);
    for (const auto* name : names) {
      const auto normalizedNeedle = normalize(name);
      if (normalizedLabel == normalizedNeedle || normalizedLabel.find(normalizedNeedle) != string::npos ||
          normalizedNeedle.find(normalizedLabel) != string::npos) {
        const auto trimmed = trimCopy(*value);
        if (!trimmed.empty()) {
          return trimmed;
        }
      }
    }
  }

  return nullopt;
}

struct SearchMatch {
  string productNumber;
  string manufacturerId;
  string manufacturerPartNumber;
  string productDescription;
  string detailedDescription;
};

string normalizeSearchKey(string value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return normalized;
}

vector<string> tokenizeSearchKey(const string& value) {
  vector<string> tokens;
  string current;
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      current.push_back(static_cast<char>(tolower(ch)));
    } else if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

bool tokenAppears(const string& haystack, const string& needle) {
  const auto normalizedHaystack = normalizeSearchKey(haystack);
  const auto normalizedNeedle = normalizeSearchKey(needle);
  return !normalizedNeedle.empty() && normalizedHaystack.find(normalizedNeedle) != string::npos;
}

int scoreSearchMatch(const SearchMatch& match, const string& query, bool exactBucket) {
  const auto normalizedQuery = normalizeSearchKey(query);
  const auto queryTokens = tokenizeSearchKey(query);

  const auto scoreCandidate = [&](const string& candidate, int exactScore, int containsScore) {
    int score = 0;
    const auto normalizedCandidate = normalizeSearchKey(candidate);
    if (normalizedCandidate.empty()) {
      return score;
    }
    if (!normalizedQuery.empty() && normalizedCandidate == normalizedQuery) {
      score += exactScore;
    } else if (!normalizedQuery.empty() &&
               (normalizedCandidate.find(normalizedQuery) != string::npos ||
                normalizedQuery.find(normalizedCandidate) != string::npos)) {
      score += containsScore;
    }
    for (const auto& token : queryTokens) {
      if (token.size() >= 2 && normalizedCandidate.find(token) != string::npos) {
        score += 8;
      }
    }
    return score;
  };

  int score = exactBucket ? 20 : 0;
  score += scoreCandidate(match.productNumber, 120, 80);
  score += scoreCandidate(match.manufacturerPartNumber, 140, 90);
  score += scoreCandidate(match.productDescription, 40, 25);
  score += scoreCandidate(match.detailedDescription, 30, 20);

  for (const auto& token : queryTokens) {
    if (token.size() >= 2 &&
        (tokenAppears(match.productNumber, token) || tokenAppears(match.manufacturerPartNumber, token))) {
      score += 15;
    }
    if (token.size() >= 2 && tokenAppears(match.productDescription, token)) {
      score += 4;
    }
  }

  return score;
}

optional<SearchMatch> extractSearchMatch(const JsonPtr& product) {
  SearchMatch match;
  match.manufacturerId = readPath(product, {"Manufacturer", "Id"}).value_or("");
  match.manufacturerPartNumber = readPath(product, {"ManufacturerProductNumber"}).value_or("");
  match.productDescription = readPath(product, {"Description", "ProductDescription"}).value_or("");
  match.detailedDescription = readPath(product, {"Description", "DetailedDescription"}).value_or("");

  const auto* variations = asArray(findMember(product, "ProductVariations") == nullptr ? nullptr : *findMember(product, "ProductVariations"));
  if (variations != nullptr) {
    for (const auto& variation : *variations) {
      if (const auto number = readFirstMember(variation, {"DigiKeyProductNumber"}); number.has_value() && !number->empty()) {
        match.productNumber = *number;
        return match;
      }
    }
  }

  if (const auto directNumber = readFirstMember(product, {"DigiKeyProductNumber"}); directNumber.has_value() &&
                                                                     !directNumber->empty()) {
    match.productNumber = *directNumber;
    return match;
  }

  if (const auto mpn = readPath(product, {"ManufacturerProductNumber"}); mpn.has_value() && !mpn->empty()) {
    match.productNumber = *mpn;
    return match;
  }

  return nullopt;
}

optional<SearchMatch> resolveSearchResult(const JsonPtr& root, const string& query) {
  optional<SearchMatch> bestMatch;
  int bestScore = 0;

  const auto considerMatches = [&](const JsonValue::Array* products, bool exactBucket) {
    if (products == nullptr) {
      return;
    }
    for (const auto& product : *products) {
      if (const auto match = extractSearchMatch(product); match.has_value()) {
        const int score = scoreSearchMatch(*match, query, exactBucket);
        if (score > bestScore) {
          bestScore = score;
          bestMatch = move(*match);
        }
      }
    }
  };

  considerMatches(asArray(findMember(root, "ExactMatches") == nullptr ? nullptr : *findMember(root, "ExactMatches")), true);
  considerMatches(asArray(findMember(root, "Products") == nullptr ? nullptr : *findMember(root, "Products")), false);

  if (bestScore <= 0) {
    return nullopt;
  }
  return bestMatch;
}

optional<string> extractComponentPackageFromText(const string& text) {
  if (text.empty()) {
    return nullopt;
  }

  static const pair<const char*, const char*> kPatterns[] = {
      {R"(\b(AXIAL|RADIAL|THROUGH HOLE|SURFACE MOUNT|SMD|SMT|MODULE)\b)", "$1"},
      {R"(\b(01005|0201|0402|0603|0805|1206|1210|1812|2010|2512)\b)", "$1"},
      {R"(\b(SOT-?23(?:-?\d+)?)\b)", "$1"},
      {R"(\b(SOT-?223(?:-?\d+)?)\b)", "$1"},
      {R"(\b(SOIC-?\d+)\b)", "$1"},
      {R"(\b(TSSOP-?\d+)\b)", "$1"},
      {R"(\b(SSOP-?\d+)\b)", "$1"},
      {R"(\b(MSOP-?\d+)\b)", "$1"},
      {R"(\b(QFN-?\d+)\b)", "$1"},
      {R"(\b(DFN-?\d+)\b)", "$1"},
      {R"(\b(QFP-?\d+)\b)", "$1"},
      {R"(\b(TQFP-?\d+)\b)", "$1"},
      {R"(\b(LQFP-?\d+)\b)", "$1"},
      {R"(\b(DIP-?\d+)\b)", "$1"},
      {R"(\b(BGA-?\d+)\b)", "$1"},
      {R"(\b(LGA-?\d+)\b)", "$1"},
      {R"(\b(TO-?92(?:-?\d+)?)\b)", "$1"},
      {R"(\b(TO-?220(?:-?\d+)?)\b)", "$1"},
      {R"(\b(TO-?263(?:-?\d+)?)\b)", "$1"},
  };

  for (const auto& [pattern, replacement] : kPatterns) {
    regex re(pattern, regex_constants::icase);
    smatch match;
    if (regex_search(text, match, re) && match.size() > 1) {
      auto package = match[1].str();
      return package;
    }
  }

  return nullopt;
}

bool looksLikeInductorText(const string& text) {
  const auto normalized = normalizeParameterKey(text);
  return normalized.find("inductor") != string::npos || normalized.find("fixedind") != string::npos ||
         normalized.find("choke") != string::npos || normalized.find("coil") != string::npos;
}

string canonicalInductanceUnit(string unit) {
  transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  if (unit == "uh") {
    return "uH";
  }
  if (unit == "nh") {
    return "nH";
  }
  if (unit == "mh") {
    return "mH";
  }
  if (unit == "ph") {
    return "pH";
  }
  return "H";
}

optional<string> extractInductanceFromText(const string& text) {
  if (!looksLikeInductorText(text)) {
    return nullopt;
  }

  regex valuePattern(R"(\b(\d+(?:\.\d+)?|\d+[rR]\d+)\s*([munp]?h)\b)", regex_constants::icase);
  smatch match;
  if (regex_search(text, match, valuePattern) && match.size() > 2) {
    auto number = match[1].str();
    replace(number.begin(), number.end(), 'R', '.');
    replace(number.begin(), number.end(), 'r', '.');
    return number + canonicalInductanceUnit(match[2].str());
  }

  return nullopt;
}

optional<string> extractComponentPackage(const JsonPtr& product) {
  const auto fromParameter = [&](initializer_list<const char*> names) -> optional<string> {
    if (const auto value = readParameterValue(product, names); value.has_value() && !looksLikePackagingType(*value)) {
      return value;
    }
    return nullopt;
  };

  if (const auto package = fromParameter({"Package / Case", "Package Case", "Case / Package", "Case Package",
                                          "Supplier Device Package", "Device Package"});
      package.has_value()) {
    return package;
  }

  if (const auto package = fromParameter({"Package"}); package.has_value()) {
    return package;
  }

  const auto description = readPath(product, {"Description", "ProductDescription"}).value_or("");
  const auto detailedDescription = readPath(product, {"Description", "DetailedDescription"}).value_or("");
  const auto manufacturerPartNumber = readPath(product, {"ManufacturerProductNumber"}).value_or("");
  const auto combinedText = description + " " + detailedDescription + " " + manufacturerPartNumber;
  if (const auto inferred = extractComponentPackageFromText(combinedText); inferred.has_value()) {
    return inferred;
  }

  return nullopt;
}

vector<Parameter> extractParameters(const JsonPtr& product) {
  vector<Parameter> parameters;
  const auto* entries = asArray(findMember(product, "Parameters") == nullptr ? nullptr : *findMember(product, "Parameters"));
  if (entries == nullptr) {
    return parameters;
  }

  for (const auto& entry : *entries) {
    const auto label = readFirstMember(entry, {"Parameter", "ParameterText"});
    const auto value = readParameterText(entry, label.value_or(""));
    if (label.has_value() && value.has_value()) {
      auto labelText = trimCopy(*label);
      auto valueText = trimCopy(*value);
      if (toLower(labelText) == "package") {
        labelText = "Packaging";
      }
      if (!labelText.empty() && !valueText.empty()) {
        parameters.push_back({move(labelText), move(valueText)});
      }
    }
  }

  return parameters;
}

void upsertExtractedInductance(vector<Parameter>& parameters, const string& value) {
  const auto trimmed = trimCopy(value);
  if (trimmed.empty() || !looksLikeInductanceValue(trimmed)) {
    return;
  }

  for (auto& parameter : parameters) {
    const auto label = normalizeParameterKey(parameter.name);
    if (label.find("inductance") != string::npos) {
      if (!looksLikeInductanceValue(parameter.value)) {
        parameter.value = trimmed;
      }
      return;
    }
  }

  parameters.push_back({"Inductance", trimmed});
}

optional<string> extractPackagingType(const JsonPtr& product) {
  if (const auto* variations = asArray(findMember(product, "ProductVariations") == nullptr ? nullptr : *findMember(product, "ProductVariations"));
      variations != nullptr && !variations->empty()) {
    const auto package = readPath((*variations)[0], {"PackageType", "Name"});
    if (package.has_value() && !trimCopy(*package).empty()) {
      return package;
    }
  }
  return nullopt;
}

DigiKeyProductDetails parseProductDetails(const string& lookupKey, const JsonPtr& root) {
  DigiKeyProductDetails details;
  details.lookupKey = lookupKey;

  const auto* product = findMember(root, "Product");
  const JsonPtr productNode = product != nullptr ? *product : root;

  details.manufacturerName = readPath(productNode, {"Manufacturer", "Name"}).value_or("");
  details.manufacturerPartNumber = readPath(productNode, {"ManufacturerProductNumber"}).value_or("");
  if (const auto category = extractCategoryName(productNode); category.has_value()) {
    details.categoryName = *category;
  }
  details.productDescription = readPath(productNode, {"Description", "ProductDescription"}).value_or("");
  details.detailedDescription = readPath(productNode, {"Description", "DetailedDescription"}).value_or("");
  details.productUrl = readPath(productNode, {"ProductUrl"}).value_or("");
  details.datasheetUrl = readPath(productNode, {"DatasheetUrl"}).value_or("");
  if (details.datasheetUrl.empty()) {
    details.datasheetUrl = readPath(productNode, {"PrimaryDatasheet"}).value_or("");
  }
  details.rohsStatus = readPath(productNode, {"RoHSStatus"}).value_or("");
  details.leadStatus = readPath(productNode, {"LeadStatus"}).value_or("");
  details.productStatus = readPath(productNode, {"ProductStatus", "Status"}).value_or("");
  details.manufacturerLeadWeeks = readPath(productNode, {"ManufacturerLeadWeeks"}).value_or("");
  details.quantityAvailable = readPath(productNode, {"QuantityAvailable"}).value_or("");

  if (const auto package = extractPackagingType(productNode); package.has_value()) {
    details.packagingType = *package;
  }

  if (const auto package = extractComponentPackage(productNode); package.has_value()) {
    details.packageName = *package;
  }

  const auto standardPricing = findMember(productNode, "StandardPricing");
  if (const auto* pricing = asArray(standardPricing == nullptr ? nullptr : *standardPricing); pricing != nullptr &&
      !pricing->empty()) {
    details.unitPrice = readPath((*pricing)[0], {"UnitPrice"}).value_or("");
  } else {
    details.unitPrice = readPath(productNode, {"UnitPrice"}).value_or("");
  }

  details.parameters = extractParameters(productNode);
  const auto combinedText = details.productDescription + " " + details.detailedDescription + " " +
                            details.manufacturerPartNumber;
  if (const auto inductance = extractInductanceFromText(combinedText); inductance.has_value()) {
    upsertExtractedInductance(details.parameters, *inductance);
  }

  if (!details.packageName.empty()) {
    const auto packageExists = any_of(details.parameters.begin(), details.parameters.end(), [](const Parameter& parameter) {
      return toLower(parameter.name) == "package" || toLower(parameter.name) == "package / case" ||
             toLower(parameter.name) == "supplier device package";
    });
    if (!packageExists) {
      details.parameters.push_back({"Package", details.packageName});
    }
  }

  return details;
}

optional<JsonPtr> parseJson(const string& body, string* error) {
  JsonParser parser(body);
  JsonPtr root;
  if (!parser.parse(root, error)) {
    return nullopt;
  }
  return root;
}

}  // namespace

bool DigiKeyConfig::valid() const {
  return !trimCopy(clientId).empty() && !trimCopy(clientSecret).empty();
}

bool loadEnvironmentFile(const filesystem::path& path) {
  error_code error;
  if (!filesystem::exists(path, error)) {
    return false;
  }

  ifstream file(path);
  if (!file) {
    return false;
  }

  string line;
  while (getline(file, line)) {
    line = trimCopy(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    const auto equalsPos = line.find('=');
    if (equalsPos == string::npos) {
      continue;
    }

    string key = trimCopy(line.substr(0, equalsPos));
    string value = trimCopy(line.substr(equalsPos + 1));
    if (!value.empty() && value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                                (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }

    if (!key.empty()) {
      _putenv_s(key.c_str(), value.c_str());
    }
  }

  return true;
}

DigiKeyConfig loadDigiKeyConfig() {
  DigiKeyConfig config;
  if (const char* value = getenv("DIGIKEY_CLIENT_ID"); value != nullptr) {
    config.clientId = value;
  }
  if (const char* value = getenv("DIGIKEY_CLIENT_SECRET"); value != nullptr) {
    config.clientSecret = value;
  }
  if (const char* value = getenv("DIGIKEY_ACCOUNT_ID"); value != nullptr) {
    config.accountId = value;
  }
  if (const char* value = getenv("DIGIKEY_SITE"); value != nullptr && *value != '\0') {
    config.site = value;
  }
  if (const char* value = getenv("DIGIKEY_LANGUAGE"); value != nullptr && *value != '\0') {
    config.language = value;
  }
  if (const char* value = getenv("DIGIKEY_CURRENCY"); value != nullptr && *value != '\0') {
    config.currency = value;
  }
  AppSettings settings;
  if (loadAppSettings(appSettingsPath(), settings)) {
    if (!settings.digiKeyClientId.empty()) config.clientId = settings.digiKeyClientId;
    if (!settings.digiKeyAccountId.empty()) config.accountId = settings.digiKeyAccountId;
    if (!settings.digiKeySite.empty()) config.site = settings.digiKeySite;
    if (!settings.digiKeyLanguage.empty()) config.language = settings.digiKeyLanguage;
    if (!settings.digiKeyCurrency.empty()) config.currency = settings.digiKeyCurrency;
  }
  if (const auto secret = CredentialStore::read("digikey-client-secret"); secret.has_value()) {
    config.clientSecret = *secret;
  }
  return config;
}

DigiKeyApiClient::DigiKeyApiClient(DigiKeyConfig config) : config_(move(config)) {}

bool DigiKeyApiClient::testConnection(string* error) {
  return ensureAccessToken(error);
}

optional<string> DigiKeyApiClient::requestToken(string* error) {
  const wstring url = L"https://api.digikey.com/v1/oauth2/token";
  ostringstream body;
  body << "client_id=" << encodeFormValue(config_.clientId) << "&client_secret=" << encodeFormValue(config_.clientSecret)
       << "&grant_type=client_credentials";

  HttpResponse response;
  if (!requestHttp(L"POST", url, L"Content-Type: application/x-www-form-urlencoded\r\n", body.str(), response, error)) {
    return nullopt;
  }
  if (response.statusCode < 200 || response.statusCode >= 300) {
    if (error != nullptr) {
      ostringstream out;
      out << "DigiKey token request failed with HTTP " << response.statusCode;
      if (!response.body.empty()) {
        out << ": " << response.body;
      }
      *error = out.str();
    }
    return nullopt;
  }

  string parseError;
  const auto root = parseJson(response.body, &parseError);
  if (!root.has_value()) {
    if (error != nullptr) {
      *error = "Unable to parse DigiKey token response: " + parseError;
    }
    return nullopt;
  }

  const auto token = readPath(*root, {"access_token"});
  if (!token.has_value() || token->empty()) {
    if (error != nullptr) {
      *error = "DigiKey token response did not include an access token";
    }
    return nullopt;
  }

  tokenExpiresAt_ = time(nullptr) + 540;
  return token;
}

bool DigiKeyApiClient::ensureAccessToken(string* error) {
  if (!accessToken_.empty() && time(nullptr) < tokenExpiresAt_) {
    return true;
  }

  const auto token = requestToken(error);
  if (!token.has_value()) {
    return false;
  }

  accessToken_ = *token;
  return true;
}

optional<string> DigiKeyApiClient::requestProductDetails(const string& productNumber,
                                                                   string* error,
                                                                   const string& manufacturerId) {
  if (!ensureAccessToken(error)) {
    return nullopt;
  }

  ostringstream url;
  url << "https://api.digikey.com/products/v4/search/" << encodePathSegment(productNumber) << "/productdetails";
  if (!trimCopy(manufacturerId).empty()) {
    url << "?manufacturerId=" << encodeComponent(manufacturerId, false);
  }

  ostringstream headers;
  headers << "Authorization: Bearer " << accessToken_ << "\r\n";
  headers << "X-DIGIKEY-Client-Id: " << config_.clientId << "\r\n";
  headers << "X-DIGIKEY-Locale-Language: " << config_.language << "\r\n";
  headers << "X-DIGIKEY-Locale-Currency: " << config_.currency << "\r\n";
  headers << "X-DIGIKEY-Locale-Site: " << config_.site << "\r\n";
  if (!config_.accountId.empty()) {
    headers << "X-DIGIKEY-Account-Id: " << config_.accountId << "\r\n";
  }

  HttpResponse response;
  if (!requestHttp(L"GET", widen(url.str()), widen(headers.str()), "", response, error)) {
    return nullopt;
  }

  if (response.statusCode < 200 || response.statusCode >= 300) {
    if (error != nullptr) {
      ostringstream out;
      out << "DigiKey details request failed with HTTP " << response.statusCode;
      if (!response.body.empty()) {
        out << ": " << response.body;
      }
      *error = out.str();
    }
    return nullopt;
  }

  return response.body;
}

optional<string> DigiKeyApiClient::requestKeywordSearch(const string& keywords, string* error) {
  if (!ensureAccessToken(error)) {
    return nullopt;
  }

  ostringstream body;
  body << "{\"Keywords\":\"" << escapeJsonString(keywords) << "\",\"Limit\":10,\"Offset\":0}";

  ostringstream headers;
  headers << "Authorization: Bearer " << accessToken_ << "\r\n";
  headers << "X-DIGIKEY-Client-Id: " << config_.clientId << "\r\n";
  headers << "X-DIGIKEY-Locale-Language: " << config_.language << "\r\n";
  headers << "X-DIGIKEY-Locale-Currency: " << config_.currency << "\r\n";
  headers << "X-DIGIKEY-Locale-Site: " << config_.site << "\r\n";
  if (!config_.accountId.empty()) {
    headers << "X-DIGIKEY-Account-Id: " << config_.accountId << "\r\n";
  }
  headers << "Content-Type: application/json\r\n";

  HttpResponse response;
  if (!requestHttp(L"POST", L"https://api.digikey.com/products/v4/search/keyword", widen(headers.str()), body.str(),
                   response, error)) {
    return nullopt;
  }

  if (response.statusCode < 200 || response.statusCode >= 300) {
    if (error != nullptr) {
      ostringstream out;
      out << "DigiKey keyword search failed with HTTP " << response.statusCode;
      if (!response.body.empty()) {
        out << ": " << response.body;
      }
      *error = out.str();
    }
    return nullopt;
  }

  return response.body;
}

optional<DigiKeyProductDetails> DigiKeyApiClient::fetchProductDetails(const string& productNumber,
                                                                           string* error) {
  const auto parseDetails = [](const string& lookupKey, const string& bodyText, string* parseError) {
    string bodyParseError;
    const auto root = parseJson(bodyText, &bodyParseError);
    if (!root.has_value()) {
      if (parseError != nullptr) {
        *parseError = "Unable to parse DigiKey details response: " + bodyParseError;
      }
      return optional<DigiKeyProductDetails>{};
    }

    auto details = parseProductDetails(lookupKey, *root);
    if (details.productDescription.empty() && details.parameters.empty()) {
      if (parseError != nullptr) {
        *parseError = "DigiKey returned an empty details payload";
      }
      return optional<DigiKeyProductDetails>{};
    }

    return optional<DigiKeyProductDetails>{move(details)};
  };

  string directError;
  if (const auto body = requestProductDetails(productNumber, &directError); body.has_value()) {
    string parseError;
    if (const auto details = parseDetails(productNumber, *body, &parseError); details.has_value()) {
      return details;
    }
    directError = move(parseError);
  }

  string keywordError;
  const auto keywordBody = requestKeywordSearch(productNumber, &keywordError);
  if (!keywordBody.has_value()) {
    if (error != nullptr) {
      *error = directError.empty() ? keywordError : directError + " | " + keywordError;
    }
    return nullopt;
  }

  string searchParseError;
  const auto searchRoot = parseJson(*keywordBody, &searchParseError);
  if (!searchRoot.has_value()) {
    if (error != nullptr) {
      *error = "Unable to parse DigiKey keyword response: " + searchParseError;
    }
    return nullopt;
  }

  const auto match = resolveSearchResult(*searchRoot, productNumber);
  if (!match.has_value()) {
    if (error != nullptr) {
      *error = directError.empty() ? "DigiKey keyword search did not return a usable match"
                                   : directError + " | DigiKey keyword search did not return a usable match";
    }
    return nullopt;
  }

  string resolvedError;
  if (const auto resolvedBody = requestProductDetails(match->productNumber, &resolvedError, match->manufacturerId);
      resolvedBody.has_value()) {
    string parseError;
    if (const auto details = parseDetails(match->productNumber, *resolvedBody, &parseError); details.has_value()) {
      return details;
    }
    resolvedError = move(parseError);
  }

  if (error != nullptr) {
    *error = directError.empty() ? resolvedError : directError + " | " + resolvedError;
  }
  return nullopt;
}

}  // namespace inventatory

#else

namespace inventatory {

bool DigiKeyConfig::valid() const {
  return false;
}

bool loadEnvironmentFile(const filesystem::path&) {
  return false;
}

DigiKeyConfig loadDigiKeyConfig() {
  return {};
}

DigiKeyApiClient::DigiKeyApiClient(DigiKeyConfig config) : config_(move(config)) {}

bool DigiKeyApiClient::testConnection(string* error) {
  if (error != nullptr) *error = "DigiKey integration is only available on Windows";
  return false;
}

optional<DigiKeyProductDetails> DigiKeyApiClient::fetchProductDetails(const string&, string*) {
  return nullopt;
}

}  // namespace inventatory

#endif

