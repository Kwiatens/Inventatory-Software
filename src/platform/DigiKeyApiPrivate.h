// Inventatory - Private DigiKey implementation contracts.

#pragma once

#include "platform/DigiKeyApi.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace inventatory {
namespace digikey_detail {

using std::initializer_list;
using std::optional;
using std::shared_ptr;
using std::string;
using std::vector;
using std::wstring;

inline constexpr std::size_t kMaximumDigiKeyResponseBytes = 4U * 1024U * 1024U;
inline constexpr unsigned kMaximumJsonDepth = 64;
inline constexpr std::size_t kMaximumJsonContainerEntries = 100000;
inline constexpr std::size_t kMaximumDigiKeyFieldBytes = 4096;
inline constexpr std::size_t kMaximumDigiKeyRequestBodyBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumDigiKeyUrlBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumCategoryPathEntries = 256;
inline constexpr unsigned kMaximumDigiKeyHttpAttempts = 2;

struct HttpResponse {
  std::uint32_t statusCode = 0;
  string body;
  std::uint32_t retryAfterSeconds = 0;
};

struct JsonValue {
  struct Number {
    string text;
  };
  using Object = std::unordered_map<string, shared_ptr<JsonValue>>;
  using Array = vector<shared_ptr<JsonValue>>;

  JsonValue() = default;
  explicit JsonValue(string text) : data(std::move(text)) {}
  explicit JsonValue(Number number) : data(std::move(number)) {}
  explicit JsonValue(bool flag) : data(flag) {}
  explicit JsonValue(Object object) : data(std::move(object)) {}
  explicit JsonValue(Array array) : data(std::move(array)) {}

  std::variant<std::nullptr_t, bool, string, Number, Object, Array> data = nullptr;
};

using JsonPtr = shared_ptr<JsonValue>;

struct SearchMatch {
  string productNumber;
  string manufacturerId;
  string manufacturerPartNumber;
  string productDescription;
  string detailedDescription;
};

string trimCopy(string value);
string encodeFormValue(const string& value);
string encodePathSegment(const string& value);
string encodeComponent(const string& value, bool formEncoding);
string escapeJsonString(const string& value);
wstring widen(const string& value);
string narrow(const wstring& value);
bool requestHttp(const wstring& method, const wstring& url, const wstring& headers, const string& body,
                 HttpResponse& response, string* error);

optional<JsonPtr> parseJson(const string& body, string* error);
const JsonValue* asValue(const JsonPtr& value);
const JsonValue::Object* asObject(const JsonPtr& value);
const JsonValue::Array* asArray(const JsonPtr& value);
string valueText(const JsonPtr& value);
const JsonPtr* findMember(const JsonPtr& object, const string& key);
optional<string> readPath(const JsonPtr& root, initializer_list<const char*> path);
optional<string> readStringPath(const JsonPtr& root, initializer_list<const char*> path);
optional<string> readFirstMember(const JsonPtr& root, initializer_list<const char*> keys);
vector<string> extractCategoryPath(const JsonPtr& product);

string normalizeParameterKey(const string& value);
bool looksLikeInductanceValue(const string& value);
optional<string> readParameterText(const JsonPtr& entry, const string& label);
bool looksLikePackagingType(const string& value);
optional<string> readParameterValue(const JsonPtr& product, initializer_list<const char*> names);
optional<string> extractInductanceFromText(const string& text);

optional<SearchMatch> resolveSearchResult(const JsonPtr& root, const string& query);
DigiKeyProductDetails parseProductDetails(const string& lookupKey, const JsonPtr& root);

bool isSafeHeaderValue(const string& value);
bool appendHeader(std::ostringstream& headers, const char* name, const string& value, string* error);
bool appendAuthorizationHeader(std::ostringstream& headers, const string& token, string* error);
bool isUnsignedDecimal(const string& value, unsigned long long maximum);
bool isFiniteDecimal(const string& value, double maximum);

}  // namespace digikey_detail
}  // namespace inventatory
