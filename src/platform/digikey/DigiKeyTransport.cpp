// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// DigiKey URL, WinHTTP, and header helpers.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/digikey/DigiKeyApiPrivate.h"
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

  // Automatic proxy (WPAD/PAC) first; fall back to the default WinHTTP proxy.
  HINTERNET session = WinHttpOpen(L"Inventatory DigiKey client/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    session = WinHttpOpen(L"Inventatory DigiKey client/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
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

  // No redirects: the bearer token and client secret must not reach another host.
  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

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
  // Only GET is retried: DigiKey publishes no idempotency contract for its POST endpoints.
  const bool retryableMethod = method == L"GET";
  for (unsigned attempt = 0; attempt < kMaximumDigiKeyHttpAttempts; ++attempt) {
    string attemptError;
    if (!requestHttpOnce(method, url, headers, body, response, &attemptError)) {
      if (error != nullptr) *error = move(attemptError);
      return false;
    }
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
