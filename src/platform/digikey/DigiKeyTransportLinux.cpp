// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Linux DigiKey HTTPS transport and shared request helpers.

#include "platform/digikey/DigiKeyApiPrivate.h"
#include "core/parts/DecimalParse.h"
#include "core/text/Utf8.h"

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

std::once_flag gCurlInitialized;
bool gCurlAvailable = false;

void initializeCurl() {
  gCurlAvailable = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

struct CurlResponse {
  std::string body;
  std::uint32_t retryAfterSeconds = 0;
  HttpResponse rate;  // Only the rate-limit header fields are used
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
  readRateLimitHeader(name, line.substr(colon + 1U), response->rate);
  if (name != "retry-after") return bytes;
  std::string value = trim(line.substr(colon + 1U));
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
                     const std::string& body, HttpResponse& response, std::string* error, const std::string& service) {
  response = {};
  if (url.empty() || url.size() > kMaximumDigiKeyUrlBytes || body.size() > kMaximumDigiKeyRequestBodyBytes) {
    if (error != nullptr) *error = service + " request exceeds the configured size limit";
    return false;
  }
  if (url.rfind("https://", 0) != 0) {
    if (error != nullptr) *error = service + " requests require HTTPS";
    return false;
  }
  CURL* handle = curl_easy_init();
  if (handle == nullptr) {
    if (error != nullptr) *error = "Unable to initialize the " + service + " HTTPS request";
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
        if (error != nullptr) *error = "Unable to prepare " + service + " request headers";
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
  const auto userAgent = "Inventatory " + service + " client/1.0";
  curl_easy_setopt(handle, CURLOPT_USERAGENT, userAgent.c_str());
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  // A broad keyword search can take DigiKey several seconds to answer.
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 20000L);
  // Worker threads share libcurl; NOSIGNAL stops it installing signal handlers.
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  // No redirects, so authenticated headers never leave the HTTPS origin.
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
      *error = received.oversized                  ? service + " response exceeds the 4 MiB safety limit"
               : result == CURLE_OPERATION_TIMEDOUT ? service + " did not answer in time"
                                                    : "Unable to complete the " + service + " HTTPS request";
    }
    return false;
  }
  response.statusCode = static_cast<std::uint32_t>(std::max(0L, status));
  response.retryAfterSeconds = received.retryAfterSeconds;
  response.retryAfterFullSeconds = received.rate.retryAfterFullSeconds;
  response.rateLimitLimit = received.rate.rateLimitLimit;
  response.rateLimitRemaining = received.rate.rateLimitRemaining;
  response.rateLimitResetSeconds = received.rate.rateLimitResetSeconds;
  response.body = std::move(received.body);
  return true;
}

}  // namespace

wstring widen(const string& value) {
  std::wstring output;
  size_t offset = 0;
  while (offset < value.size()) {
    std::uint32_t codePoint = 0;
    if (!nextUtf8CodePoint(value, offset, codePoint)) return {};
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
                 HttpResponse& response, string* error, const string& service) {
  std::call_once(gCurlInitialized, initializeCurl);
  if (!gCurlAvailable) {
    if (error != nullptr) *error = "Unable to initialize the Linux HTTPS transport";
    return false;
  }
  const auto encodedMethod = narrow(method);
  const auto encodedUrl = narrow(url);
  const auto encodedHeaders = narrow(headers);
  if (encodedMethod.empty() || encodedUrl.empty() || (headers.size() != 0 && encodedHeaders.empty())) {
    if (error != nullptr) *error = service + " request contains invalid UTF-8";
    return false;
  }
  const bool retryableMethod = encodedMethod == "GET";
  for (unsigned attempt = 0; attempt < kMaximumDigiKeyHttpAttempts; ++attempt) {
    // A reached vendor limit is answered here as a local 429; nothing is sent.
    if (!rateLimitAdmit(service, response)) return true;
    if (!requestHttpOnce(encodedMethod, encodedUrl, encodedHeaders, body, response, error, service)) return false;
    rateLimitNote(service, response);
    if (!retryableMethod || !retryableStatus(response.statusCode) || attempt + 1U >= kMaximumDigiKeyHttpAttempts) {
      return true;
    }
    const auto delay = response.retryAfterSeconds > 0 ? response.retryAfterSeconds * 1000U : 250U;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
  return false;
}

}  // namespace digikey_detail
}  // namespace inventatory
