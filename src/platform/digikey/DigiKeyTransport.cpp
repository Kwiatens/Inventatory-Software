// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// DigiKey URL, WinHTTP, and header helpers.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/digikey/DigiKeyApiPrivate.h"
#include "platform/system/WindowsText.h"
#include "core/parts/DecimalParse.h"

#ifdef _WIN32

#include "platform/system/Environment.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

#pragma comment(lib, "winhttp.lib")

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace inventatory {
using namespace std;
namespace digikey_detail {

wstring widen(const string& value) { return utf8ToWide(value); }

string narrow(const wstring& value) { return wideToUtf8(value); }

string httpErrorMessage(const string& prefix) {
  const DWORD code = GetLastError();
  ostringstream out;
  out << prefix << " (Win32 " << code << ")";
  return out.str();
}

bool requestHttpOnce(const wstring& method, const wstring& url, const wstring& headers, const string& body,
                     HttpResponse& response, string* error, const std::string& service) {
  response = {};
  if (url.empty() || url.size() > kMaximumDigiKeyUrlBytes ||
      url.size() > static_cast<size_t>(numeric_limits<DWORD>::max()) ||
      url.size() > static_cast<size_t>(numeric_limits<int>::max()) ||
      headers.size() > static_cast<size_t>(numeric_limits<int>::max())) {
    if (error != nullptr) *error = service + " request URL or headers are too large";
    return false;
  }
  if (body.size() > kMaximumDigiKeyRequestBodyBytes) {
    if (error != nullptr) *error = service + " request body is too large";
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
      *error = httpErrorMessage("Unable to parse " + service + " URL");
    }
    return false;
  }
  // Credentials travel in these requests, so plain HTTP is refused, as on Linux.
  if (components.nScheme != INTERNET_SCHEME_HTTPS) {
    if (error != nullptr) *error = service + " requests require HTTPS";
    return false;
  }

  wstring host(components.lpszHostName, components.dwHostNameLength);
  wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.dwExtraInfoLength > 0) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }

  // Automatic proxy (WPAD/PAC) first; fall back to the default WinHTTP proxy.
  const auto userAgent = widen("Inventatory " + service + " client/1.0");
  HINTERNET session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
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
      *error = httpErrorMessage("Unable to connect to " + service);
    }
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET request = WinHttpOpenRequest(connection, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (request == nullptr) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to open " + service + " request");
    }
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  // No redirects: the bearer token and client secret must not reach another host.
  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

  if (!headers.empty() &&
      !WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to apply " + service + " request headers");
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
      *error = httpErrorMessage("Unable to send " + service + " request");
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  if (!WinHttpReceiveResponse(request, nullptr)) {
    if (error != nullptr) {
      *error = httpErrorMessage("Unable to receive " + service + " response");
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
      *error = httpErrorMessage("Unable to query " + service + " response status");
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  // Retry-After may be an HTTP date. Accept only a small decimal delay, capped at 2 seconds.
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

  // Rate-limit headers; absent ones leave the fields at -1.
  struct RateHeader {
    const wchar_t* name;
    const char* lowerName;
  };
  static const RateHeader kRateHeaders[] = {{L"Retry-After", "retry-after"},
                                            {L"X-RateLimit-Limit", "x-ratelimit-limit"},
                                            {L"X-RateLimit-Remaining", "x-ratelimit-remaining"},
                                            {L"X-RateLimit-Reset", "x-ratelimit-reset"},
                                            {L"X-BurstLimit-Reset", "x-burstlimit-reset"}};
  for (const auto& header : kRateHeaders) {
    wchar_t value[64]{};
    DWORD valueSize = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, header.name, value, &valueSize,
                             WINHTTP_NO_HEADER_INDEX)) {
      continue;
    }
    readRateLimitHeader(header.lowerName, narrow(value), response);
  }

  string bodyText;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      if (error != nullptr) {
        *error = httpErrorMessage("Unable to read " + service + " response");
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
      if (error != nullptr) *error = service + " response exceeds the 4 MiB safety limit";
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
        *error = httpErrorMessage("Unable to copy " + service + " response");
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
                 HttpResponse& response, string* error, const string& service) {
  // Only GET is retried: DigiKey publishes no idempotency contract for its POST endpoints.
  const bool retryableMethod = method == L"GET";
  for (unsigned attempt = 0; attempt < kMaximumDigiKeyHttpAttempts; ++attempt) {
    string attemptError;
    // A reached vendor limit is answered here as a local 429; nothing is sent.
    if (!rateLimitAdmit(service, response)) return true;
    if (!requestHttpOnce(method, url, headers, body, response, &attemptError, service)) {
      if (error != nullptr) *error = move(attemptError);
      return false;
    }
    rateLimitNote(service, response);
    if (!retryableMethod || !isRetryableHttpStatus(response.statusCode) ||
        attempt + 1U >= kMaximumDigiKeyHttpAttempts) {
      return true;
    }

    const DWORD delayMs = response.retryAfterSeconds > 0 ? response.retryAfterSeconds * 1000U : 250U;
    Sleep(delayMs);
  }
  return false;
}

}  // namespace digikey_detail
}  // namespace inventatory

#endif
