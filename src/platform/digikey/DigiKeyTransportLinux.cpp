// Inventatory - Linux DigiKey HTTPS transport and shared request helpers.

#include "platform/digikey/DigiKeyApiPrivate.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include <curl/curl.h>

namespace inventatory {
namespace digikey_detail {
namespace {

constexpr size_t kMaximumDigiKeyResponseBytes = 4U * 1024U * 1024U;
std::once_flag gCurlInitialized;
bool gCurlAvailable = false;

void initializeCurl() {
  gCurlAvailable = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void appendUtf8(std::string& output, std::uint32_t codePoint) {
  if (codePoint <= 0x7fU) output.push_back(static_cast<char>(codePoint));
  else if (codePoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  } else if (codePoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  }
}

bool nextCodePoint(const std::string& input, size_t& offset, std::uint32_t& codePoint) {
  if (offset >= input.size()) return false;
  const auto first = static_cast<unsigned char>(input[offset++]);
  if (first <= 0x7fU) {
    codePoint = first;
    return true;
  }
  unsigned int continuationCount = 0;
  if ((first & 0xe0U) == 0xc0U) { codePoint = first & 0x1fU; continuationCount = 1; }
  else if ((first & 0xf0U) == 0xe0U) { codePoint = first & 0x0fU; continuationCount = 2; }
  else if ((first & 0xf8U) == 0xf0U) { codePoint = first & 0x07U; continuationCount = 3; }
  else return false;
  if (input.size() - offset < continuationCount) return false;
  for (unsigned int index = 0; index < continuationCount; ++index) {
    const auto continuation = static_cast<unsigned char>(input[offset++]);
    if ((continuation & 0xc0U) != 0x80U) return false;
    codePoint = (codePoint << 6U) | (continuation & 0x3fU);
  }
  if ((continuationCount == 1U && codePoint < 0x80U) ||
      (continuationCount == 2U && codePoint < 0x800U) ||
      (continuationCount == 3U && codePoint < 0x10000U) || codePoint > 0x10ffffU ||
      (codePoint >= 0xd800U && codePoint <= 0xdfffU)) return false;
  return true;
}

struct CurlResponse {
  std::string body;
  std::uint32_t retryAfterSeconds = 0;
  bool oversized = false;
};

size_t receiveBody(char* data, size_t size, size_t count, void* context) {
  auto* response = static_cast<CurlResponse*>(context);
  if (size != 0 && count > static_cast<size_t>(-1) / size) return 0;
  const auto bytes = size * count;
  if (bytes > kMaximumDigiKeyResponseBytes || response->body.size() > kMaximumDigiKeyResponseBytes - bytes) {
    response->oversized = true;
    return 0;
  }
  response->body.append(data, bytes);
  return bytes;
}

size_t receiveHeader(char* data, size_t size, size_t count, void* context) {
  if (size != 0 && count > static_cast<size_t>(-1) / size) return 0;
  const auto bytes = size * count;
  auto* response = static_cast<CurlResponse*>(context);
  std::string line(data, bytes);
  const auto colon = line.find(':');
  if (colon == std::string::npos) return bytes;
  std::string name = line.substr(0, colon);
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (name != "retry-after") return bytes;
  std::string value = trimCopy(line.substr(colon + 1U));
  if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
    return bytes;
  }
  unsigned seconds = 0;
  for (const unsigned char ch : value) seconds = std::min(2U, seconds * 10U + static_cast<unsigned>(ch - '0'));
  response->retryAfterSeconds = seconds;
  return bytes;
}

bool retryableStatus(std::uint32_t status) {
  return status == 408 || status == 425 || status == 429 || status == 500 || status == 502 || status == 503 ||
         status == 504;
}

bool requestHttpOnce(const std::string& method, const std::string& url, const std::string& headers,
                     const std::string& body, HttpResponse& response, std::string* error) {
  response = {};
  if (url.empty() || url.size() > kMaximumDigiKeyUrlBytes || body.size() > kMaximumDigiKeyRequestBodyBytes) {
    if (error != nullptr) *error = "DigiKey request exceeds the configured size limit";
    return false;
  }
  if (url.rfind("https://", 0) != 0) {
    if (error != nullptr) *error = "DigiKey requests require HTTPS";
    return false;
  }
  CURL* handle = curl_easy_init();
  if (handle == nullptr) {
    if (error != nullptr) *error = "Unable to initialize the DigiKey HTTPS request";
    return false;
  }
  CurlResponse received;
  curl_slist* headerList = nullptr;
  size_t begin = 0;
  while (begin < headers.size()) {
    const auto end = headers.find("\r\n", begin);
    const auto line = headers.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!line.empty()) {
      auto* appendedHeaders = curl_slist_append(headerList, line.c_str());
      if (appendedHeaders == nullptr) {
        curl_slist_free_all(headerList);
        curl_easy_cleanup(handle);
        if (error != nullptr) *error = "Unable to prepare DigiKey request headers";
        return false;
      }
      headerList = appendedHeaders;
    }
    if (end == std::string::npos) break;
    begin = end + 2U;
  }
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "Inventatory DigiKey client/1.0");
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 8000L);
  // DigiKey's API endpoints do not require redirects. Keep the authenticated
  // custom headers bound to the HTTPS origin instead of forwarding them to a
  // redirect target.
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receiveBody);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &received);
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, receiveHeader);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &received);
  if (!body.empty()) {
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  }
  const auto result = curl_easy_perform(handle);
  long status = 0;
  if (result == CURLE_OK) curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headerList);
  curl_easy_cleanup(handle);
  if (result != CURLE_OK) {
    if (error != nullptr) {
      *error = received.oversized ? "DigiKey response exceeds the 4 MiB safety limit"
                                  : "Unable to complete the DigiKey HTTPS request";
    }
    return false;
  }
  response.statusCode = static_cast<std::uint32_t>(std::max(0L, status));
  response.retryAfterSeconds = received.retryAfterSeconds;
  response.body = std::move(received.body);
  return true;
}

}  // namespace

string trimCopy(string value) { return trim(std::move(value)); }

string encodeComponent(const string& value, bool formEncoding) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    const bool asciiAlphaNumeric = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                   (ch >= '0' && ch <= '9');
    if (asciiAlphaNumeric || ch == '-' || ch == '_' || ch == '.' || ch == '~') out << static_cast<char>(ch);
    else if (ch == ' ' && formEncoding) out << '+';
    else if (ch == ' ' && !formEncoding) out << "%20";
    else out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch)
             << std::nouppercase << std::dec;
  }
  return out.str();
}

string encodeFormValue(const string& value) { return encodeComponent(value, true); }
string encodePathSegment(const string& value) { return encodeComponent(value, false); }

string escapeJsonString(const string& value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20U) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
        else out << static_cast<char>(ch);
        break;
    }
  }
  return out.str();
}

wstring widen(const string& value) {
  std::wstring output;
  size_t offset = 0;
  while (offset < value.size()) {
    std::uint32_t codePoint = 0;
    if (!nextCodePoint(value, offset, codePoint)) return {};
    output.push_back(static_cast<wchar_t>(codePoint));
  }
  return output;
}

string narrow(const wstring& value) {
  string output;
  for (const wchar_t character : value) {
    const auto codePoint = static_cast<std::uint32_t>(character);
    if (codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)) return {};
    appendUtf8(output, codePoint);
  }
  return output;
}

bool requestHttp(const wstring& method, const wstring& url, const wstring& headers, const string& body,
                 HttpResponse& response, string* error) {
  std::call_once(gCurlInitialized, initializeCurl);
  if (!gCurlAvailable) {
    if (error != nullptr) *error = "Unable to initialize the Linux HTTPS transport";
    return false;
  }
  const auto encodedMethod = narrow(method);
  const auto encodedUrl = narrow(url);
  const auto encodedHeaders = narrow(headers);
  if (encodedMethod.empty() || encodedUrl.empty() || (headers.size() != 0 && encodedHeaders.empty())) {
    if (error != nullptr) *error = "DigiKey request contains invalid UTF-8";
    return false;
  }
  const bool retryableMethod = encodedMethod == "GET";
  for (unsigned attempt = 0; attempt < kMaximumDigiKeyHttpAttempts; ++attempt) {
    if (!requestHttpOnce(encodedMethod, encodedUrl, encodedHeaders, body, response, error)) return false;
    if (!retryableMethod || !retryableStatus(response.statusCode) || attempt + 1U >= kMaximumDigiKeyHttpAttempts) {
      return true;
    }
    const auto delay = response.retryAfterSeconds > 0 ? response.retryAfterSeconds * 1000U : 250U;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
  return false;
}

bool isSafeHeaderValue(const string& value) {
  if (value.size() > kMaximumDigiKeyFieldBytes) return false;
  for (const unsigned char character : value) if (character < 0x20U || character > 0x7eU) return false;
  return true;
}

bool appendHeader(std::ostringstream& headers, const char* name, const string& value, string* error) {
  if (!isSafeHeaderValue(value)) {
    if (error != nullptr) *error = "DigiKey configuration contains invalid header characters";
    return false;
  }
  headers << name << ": " << value << "\r\n";
  return true;
}

bool appendAuthorizationHeader(std::ostringstream& headers, const string& token, string* error) {
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
    if (parsed > (std::numeric_limits<unsigned long long>::max() - digit) / 10U) return false;
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
    const double parsed = std::stod(trimmed, &consumed);
    return consumed == trimmed.size() && std::isfinite(parsed) && parsed >= 0.0 && parsed <= maximum;
  } catch (...) {
    return false;
  }
}

}  // namespace digikey_detail
}  // namespace inventatory
