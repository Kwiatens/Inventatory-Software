// Inventatory - Hardware Inventory Management System
// DigiKey API response parsing and metadata synchronization.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/DigiKeyApi.h"

#include "platform/Environment.h"
#include "app/AppSettings.h"
#include "platform/CredentialStore.h"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <iomanip>
#include <memory>
#include <regex>
#include <string_view>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

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
    const bool asciiAlphaNumeric = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                   (ch >= '0' && ch <= '9');
    if (asciiAlphaNumeric || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
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
  if (value.empty() || value.size() > static_cast<size_t>(numeric_limits<int>::max())) {
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
  DWORD retryAfterSeconds = 0;
};

constexpr size_t kMaximumDigiKeyResponseBytes = 4U * 1024U * 1024U;
constexpr unsigned kMaximumJsonDepth = 64;
constexpr size_t kMaximumJsonContainerEntries = 100000;
constexpr size_t kMaximumDigiKeyFieldBytes = 4096;
constexpr size_t kMaximumDigiKeyRequestBodyBytes = 64U * 1024U;
constexpr size_t kMaximumDigiKeyUrlBytes = 64U * 1024U;
constexpr size_t kMaximumCategoryPathEntries = 256;
constexpr unsigned kMaximumDigiKeyHttpAttempts = 2;

string httpErrorMessage(const string& prefix) {
  const DWORD code = GetLastError();
  ostringstream out;
  out << prefix << " (Win32 " << code << ")";
  return out.str();
}

bool requestHttpOnce(const wstring& method, const wstring& url, const wstring& headers, const string& body,
                     HttpResponse& response, string* error) {
  response = {};
  if (url.empty() || url.size() > kMaximumDigiKeyUrlBytes ||
      url.size() > static_cast<size_t>(numeric_limits<DWORD>::max()) ||
      url.size() > static_cast<size_t>(numeric_limits<int>::max()) ||
      headers.size() > static_cast<size_t>(numeric_limits<int>::max())) {
    if (error != nullptr) *error = "DigiKey request URL or headers are too large";
    return false;
  }
  if (body.size() > kMaximumDigiKeyRequestBodyBytes) {
    if (error != nullptr) *error = "DigiKey request body is too large";
    return false;
  }
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

  // Retry-After is advisory and may also be an HTTP date. Accept only a
  // small decimal delay so a hostile or broken server cannot hold the caller
  // indefinitely. The retry loop remains bounded regardless of this header.
  if (statusCode == 429 || statusCode >= 500) {
    wchar_t retryAfter[32]{};
    DWORD retryAfterSize = sizeof(retryAfter);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, retryAfter,
                            &retryAfterSize, WINHTTP_NO_HEADER_INDEX)) {
      size_t characters = retryAfterSize / sizeof(wchar_t);
      if (characters > 0 && retryAfter[characters - 1] == L'\0') {
        --characters;
      }
      unsigned long long seconds = 0;
      bool valid = characters > 0 && characters < _countof(retryAfter);
      for (size_t index = 0; valid && index < characters; ++index) {
        const auto character = retryAfter[index];
        if (character < L'0' || character > L'9') {
          valid = false;
          break;
        }
        const auto digit = static_cast<unsigned long long>(character - L'0');
        if (seconds > (numeric_limits<unsigned long long>::max() - digit) / 10U) {
          valid = false;
          break;
        }
        seconds = seconds * 10U + digit;
        if (seconds > 2U) {
          seconds = 2U;
          break;
        }
      }
      if (valid) {
        response.retryAfterSeconds = static_cast<DWORD>(seconds);
      }
    }
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

    if (available > kMaximumDigiKeyResponseBytes || bodyText.size() > kMaximumDigiKeyResponseBytes - available) {
      if (error != nullptr) *error = "DigiKey response exceeds the 4 MiB safety limit";
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connection);
      WinHttpCloseHandle(session);
      return false;
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

bool isRetryableHttpStatus(DWORD statusCode) {
  return statusCode == 408 || statusCode == 425 || statusCode == 429 || statusCode == 500 || statusCode == 502 ||
         statusCode == 503 || statusCode == 504;
}

bool requestHttp(const wstring& method, const wstring& url, const wstring& headers, const string& body,
                 HttpResponse& response, string* error) {
  for (unsigned attempt = 0; attempt < kMaximumDigiKeyHttpAttempts; ++attempt) {
    string attemptError;
    if (!requestHttpOnce(method, url, headers, body, response, &attemptError)) {
      if (error != nullptr) *error = move(attemptError);
      return false;
    }
    if (!isRetryableHttpStatus(response.statusCode) || attempt + 1U >= kMaximumDigiKeyHttpAttempts) {
      return true;
    }

    const DWORD delayMs = response.retryAfterSeconds > 0 ? response.retryAfterSeconds * 1000U : 250U;
    Sleep(delayMs);
  }
  return false;
}

struct JsonValue {
  struct Number {
    string text;
  };
  using Object = unordered_map<string, shared_ptr<JsonValue>>;
  using Array = vector<shared_ptr<JsonValue>>;

  JsonValue() = default;
  explicit JsonValue(string text) : data(move(text)) {}
  explicit JsonValue(Number number) : data(move(number)) {}
  explicit JsonValue(bool flag) : data(flag) {}
  explicit JsonValue(Object object) : data(move(object)) {}
  explicit JsonValue(Array array) : data(move(array)) {}

  variant<nullptr_t, bool, string, Number, Object, Array> data = nullptr;
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
    if (parameters.size() >= 256U) break;
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
  const auto categoryPath = extractCategoryPath(productNode);

  details.manufacturerName = readPath(productNode, {"Manufacturer", "Name"}).value_or("");
  details.manufacturerPartNumber = readPath(productNode, {"ManufacturerProductNumber"}).value_or("");
  if (!categoryPath.empty()) {
    details.categoryName = categoryPath.back();
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

  // Keep the catalog response in the common provider shape.  The category ID
  // is optional here because older DigiKey responses do not always expose it.
  details.vendorMetadata.provider = "digikey";
  details.vendorMetadata.providerProductNumber = lookupKey;
  details.vendorMetadata.manufacturerPartNumber = details.manufacturerPartNumber;
  details.vendorMetadata.categoryId = details.categoryName;
  details.vendorMetadata.categoryPath = categoryPath;
  details.vendorMetadata.title = details.productDescription;
  details.vendorMetadata.detailedDescription = details.detailedDescription;
  details.vendorMetadata.parameters = details.parameters;
  details.vendorMetadata.productUrl = details.productUrl;

  return details;
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

bool isSafeHeaderValue(const string& value) {
  if (value.size() > kMaximumDigiKeyFieldBytes) return false;
  for (const unsigned char character : value) {
    // Reject all controls, not only CR/LF. WinHTTP header parsing has had
    // different tolerance across Windows versions, and accepting a control
    // byte here would make configuration an HTTP request-structure input.
    if (character < 0x20U || character > 0x7EU) return false;
  }
  return true;
}

bool appendHeader(ostringstream& headers, const char* name, const string& value, string* error) {
  if (!isSafeHeaderValue(value)) {
    if (error != nullptr) *error = "DigiKey configuration contains invalid header characters";
    return false;
  }
  headers << name << ": " << value << "\r\n";
  return true;
}

bool appendAuthorizationHeader(ostringstream& headers, const string& token, string* error) {
  if (token.size() > 2048U || !isSafeHeaderValue(token)) {
    if (error != nullptr) *error = "DigiKey returned an invalid access token";
    return false;
  }
  headers << "Authorization: Bearer " << token << "\r\n";
  return true;
}

bool isUnsignedDecimal(const string& value, unsigned long long maximum) {
  const auto trimmed = trimCopy(value);
  if (trimmed.empty()) return false;
  unsigned long long parsed = 0;
  for (const unsigned char character : trimmed) {
    if (character < '0' || character > '9') return false;
    const auto digit = static_cast<unsigned long long>(character - '0');
    if (parsed > (numeric_limits<unsigned long long>::max() - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
    if (parsed > maximum) return false;
  }
  return true;
}

bool isFiniteDecimal(const string& value, double maximum) {
  const auto trimmed = trimCopy(value);
  if (trimmed.empty()) return false;
  try {
    size_t consumed = 0;
    const double parsed = stod(trimmed, &consumed);
    return consumed == trimmed.size() && isfinite(parsed) && parsed >= 0.0 && parsed <= maximum;
  } catch (...) {
    return false;
  }
}

}  // namespace

bool DigiKeyConfig::valid() const {
  return !trimCopy(clientId).empty() && !trimCopy(clientSecret).empty() &&
         clientId.size() <= kMaximumDigiKeyFieldBytes && clientSecret.size() <= kMaximumDigiKeyFieldBytes &&
         accountId.size() <= kMaximumDigiKeyFieldBytes && site.size() <= kMaximumDigiKeyFieldBytes &&
         language.size() <= kMaximumDigiKeyFieldBytes && currency.size() <= kMaximumDigiKeyFieldBytes &&
         isSafeHeaderValue(clientId) && isSafeHeaderValue(accountId) && isSafeHeaderValue(site) &&
         isSafeHeaderValue(language) && isSafeHeaderValue(currency);
}

DigiKeyConfig loadDigiKeyConfig() {
  DigiKeyConfig config;
  if (const auto value = environmentValue("DIGIKEY_CLIENT_ID"); value.has_value()) {
    config.clientId = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_ACCOUNT_ID"); value.has_value()) {
    config.accountId = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_SITE"); value.has_value() && !value->empty()) {
    config.site = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_LANGUAGE"); value.has_value() && !value->empty()) {
    config.language = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_CURRENCY"); value.has_value() && !value->empty()) {
    config.currency = *value;
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
  if (!config_.valid()) {
    if (error != nullptr) *error = "DigiKey configuration is incomplete or too large";
    return nullopt;
  }
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

  const auto token = readStringPath(*root, {"access_token"});
  if (!token.has_value() || token->empty() || token->size() > 2048U) {
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
  if (trimCopy(productNumber).empty() || productNumber.size() > kMaximumDigiKeyFieldBytes ||
      manufacturerId.size() > kMaximumDigiKeyFieldBytes) {
    if (error != nullptr) *error = "DigiKey product identifier is empty or too large";
    return nullopt;
  }
  if (!ensureAccessToken(error)) {
    return nullopt;
  }

  ostringstream url;
  url << "https://api.digikey.com/products/v4/search/" << encodePathSegment(productNumber) << "/productdetails";
  if (!trimCopy(manufacturerId).empty()) {
    url << "?manufacturerId=" << encodeComponent(manufacturerId, false);
  }

  ostringstream headers;
  if (!appendAuthorizationHeader(headers, accessToken_, error) ||
      !appendHeader(headers, "X-DIGIKEY-Client-Id", config_.clientId, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Language", config_.language, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Currency", config_.currency, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Site", config_.site, error)) {
    return nullopt;
  }
  if (!config_.accountId.empty() && !appendHeader(headers, "X-DIGIKEY-Account-Id", config_.accountId, error)) {
    return nullopt;
  }

  HttpResponse response;
  if (!requestHttp(L"GET", widen(url.str()), widen(headers.str()), "", response, error)) {
    return nullopt;
  }

  if (response.statusCode < 200 || response.statusCode >= 300) {
    if (error != nullptr) {
      ostringstream out;
      out << "DigiKey details request failed with HTTP " << response.statusCode;
      *error = out.str();
    }
    return nullopt;
  }

  return response.body;
}

optional<string> DigiKeyApiClient::requestKeywordSearch(const string& keywords, string* error) {
  if (trimCopy(keywords).empty() || keywords.size() > kMaximumDigiKeyFieldBytes) {
    if (error != nullptr) *error = "DigiKey search keywords are empty or too large";
    return nullopt;
  }
  if (!ensureAccessToken(error)) {
    return nullopt;
  }

  ostringstream body;
  body << "{\"Keywords\":\"" << escapeJsonString(keywords) << "\",\"Limit\":10,\"Offset\":0}";

  ostringstream headers;
  if (!appendAuthorizationHeader(headers, accessToken_, error) ||
      !appendHeader(headers, "X-DIGIKEY-Client-Id", config_.clientId, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Language", config_.language, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Currency", config_.currency, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Site", config_.site, error)) {
    return nullopt;
  }
  if (!config_.accountId.empty() && !appendHeader(headers, "X-DIGIKEY-Account-Id", config_.accountId, error)) {
    return nullopt;
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
      *error = out.str();
    }
    return nullopt;
  }

  return response.body;
}

optional<DigiKeyProductDetails> DigiKeyApiClient::fetchProductDetails(const string& productNumber,
                                                                           string* error) {
  if (trimCopy(productNumber).empty() || productNumber.size() > kMaximumDigiKeyFieldBytes) {
    if (error != nullptr) *error = "DigiKey product identifier is empty or too large";
    return nullopt;
  }
  const auto parseDetails = [this](const string& lookupKey, const string& bodyText, string* parseError) {
    string bodyParseError;
    const auto root = parseJson(bodyText, &bodyParseError);
    if (!root.has_value()) {
      if (parseError != nullptr) {
        *parseError = "Unable to parse DigiKey details response: " + bodyParseError;
      }
      return optional<DigiKeyProductDetails>{};
    }

    const auto* rootObject = asObject(*root);
    if (rootObject == nullptr) {
      if (parseError != nullptr) *parseError = "DigiKey details response root is not an object";
      return optional<DigiKeyProductDetails>{};
    }
    if (const auto* product = findMember(*root, "Product"); product != nullptr && asObject(*product) == nullptr) {
      if (parseError != nullptr) *parseError = "DigiKey details response has an invalid Product object";
      return optional<DigiKeyProductDetails>{};
    }

    auto details = parseProductDetails(lookupKey, *root);
    details.vendorMetadata.locale = config_.language;
    if (details.productDescription.empty() && details.parameters.empty()) {
      if (parseError != nullptr) {
        *parseError = "DigiKey returned an empty details payload";
      }
      return optional<DigiKeyProductDetails>{};
    }

    if ((!details.quantityAvailable.empty() && !isUnsignedDecimal(details.quantityAvailable, 1000000000000ULL)) ||
        (!details.manufacturerLeadWeeks.empty() &&
         !isUnsignedDecimal(details.manufacturerLeadWeeks, 1000000ULL)) ||
        (!details.unitPrice.empty() && !isFiniteDecimal(details.unitPrice, 1000000000000.0))) {
      if (parseError != nullptr) *parseError = "DigiKey details response contains an invalid numeric field";
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
