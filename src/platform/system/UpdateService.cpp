// Inventatory - GitHub release update discovery for public releases.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/system/UpdateService.h"

#include "core/transfer/InventoryTransferPrivate.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
constexpr const char* kApplicationArchiveName = "Inventatory-win-x64.zip";
constexpr const char* kApplicationChecksumsName = "SHA256SUMS.txt";
constexpr const char* kApplicationInstallerName = "Install-Inventatory.ps1";
#else
constexpr const char* kApplicationArchiveName = "Inventatory-linux-x64.tar.gz";
constexpr const char* kApplicationChecksumsName = "SHA256SUMS-linux.txt";
constexpr const char* kApplicationInstallerName = "Install-Inventatory.sh";
#endif

namespace inventatory {
namespace {

constexpr std::int64_t kUpdateCheckIntervalSeconds = 24 * 60 * 60;
constexpr size_t kMaximumReleaseMetadataBytes = 1U * 1024U * 1024U;
constexpr size_t kMaximumReleaseNotesBytes = 64U * 1024U;
constexpr std::uint64_t kMaximumUpdateAssetBytes = 512ULL * 1024ULL * 1024ULL;
constexpr char kReleaseRepository[] = Inventatory_RELEASE_REPOSITORY;
constexpr char kScanFirmwareRepository[] = Inventatory_SCAN_FIRMWARE_REPOSITORY;

#ifdef _WIN32
std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (sizeNeeded <= 0) return {};
  std::wstring result(static_cast<size_t>(sizeNeeded), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), sizeNeeded) <= 0) {
    return {};
  }
  return result;
}

std::wstring quoteWindowsArgument(const std::wstring& value) {
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      quoted.append(backslashes * 2U + 1U, L'\\');
      quoted.push_back(L'\"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2U, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}
#endif

bool isHexDigest(const std::string& value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool allowedReleaseAsset(const std::string& assetName) {
#ifdef _WIN32
  return assetName == "Inventatory-win-x64.zip" || assetName == "SHA256SUMS.txt" ||
         assetName == "Install-Inventatory.ps1" || assetName == "Install-Inventatory.cmd";
#else
  return assetName == "Inventatory-linux-x64.tar.gz" || assetName == "SHA256SUMS-linux.txt" ||
         assetName == "Install-Inventatory.sh";
#endif
}

std::string trimVersionPrefix(std::string value) {
  if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.erase(value.begin());
  return value;
}

std::string versionPrerelease(const std::string& value) {
  const auto normalized = trimVersionPrefix(value);
  const auto separator = normalized.find('-');
  return separator == std::string::npos ? std::string{} : normalized.substr(separator + 1U);
}

bool validPrerelease(const std::string& value) {
  if (value.empty()) return true;
  if (!std::isalnum(static_cast<unsigned char>(value.front())) ||
      !std::isalnum(static_cast<unsigned char>(value.back()))) {
    return false;
  }
  bool previousSeparator = false;
  for (const unsigned char ch : value) {
    if (std::isalnum(ch)) {
      previousSeparator = false;
      continue;
    }
    if (ch != '.' && ch != '-' || previousSeparator) return false;
    previousSeparator = true;
  }
  return true;
}

std::vector<int> versionParts(const std::string& value) {
  std::vector<int> parts;
  auto normalized = trimVersionPrefix(value);
  const auto separator = normalized.find('-');
  if (separator != std::string::npos) normalized.resize(separator);
  std::istringstream input(normalized);
  std::string part;
  while (std::getline(input, part, '.')) {
    if (part.empty() || parts.size() == 4U || !std::all_of(part.begin(), part.end(), [](unsigned char ch) {
          return std::isdigit(ch) != 0;
        })) return {};
    int parsed = 0;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), parsed);
    if (error != std::errc{} || end != part.data() + part.size() || parsed < 0) return {};
    parts.push_back(parsed);
  }
  return parts;
}

bool validReleaseTag(const std::string& tag) {
  const auto parts = versionParts(tag);
  return tag.size() >= 6U && tag.front() == 'v' && parts.size() == 3U && validPrerelease(versionPrerelease(tag));
}

bool validRepository(const std::string& repository) {
  const auto slash = repository.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= repository.size() ||
      repository.find('/', slash + 1) != std::string::npos) return false;
  return std::all_of(repository.begin(), repository.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '/' || ch == '-' || ch == '_' || ch == '.';
  });
}

bool parseJsonString(const std::string& text, size_t& position, std::string& output) {
  if (position >= text.size() || text[position++] != '"') return false;
  output.clear();
  while (position < text.size()) {
    const unsigned char ch = static_cast<unsigned char>(text[position++]);
    if (ch == '"') return true;
    if (ch < 0x20U) return false;
    if (ch != '\\') { output.push_back(static_cast<char>(ch)); continue; }
    if (position >= text.size()) return false;
    switch (text[position++]) {
      case '"': output.push_back('"'); break;
      case '\\': output.push_back('\\'); break;
      case '/': output.push_back('/'); break;
      case 'b': output.push_back('\b'); break;
      case 'f': output.push_back('\f'); break;
      case 'n': output.push_back('\n'); break;
      case 'r': output.push_back('\r'); break;
      case 't': output.push_back('\t'); break;
      case 'u': {
        if (position + 4U > text.size()) return false;
        unsigned int codePoint = 0;
        for (size_t index = 0; index < 4U; ++index) {
          const unsigned char digit = static_cast<unsigned char>(text[position++]);
          codePoint <<= 4U;
          if (digit >= '0' && digit <= '9') codePoint += digit - '0';
          else if (digit >= 'a' && digit <= 'f') codePoint += digit - 'a' + 10U;
          else if (digit >= 'A' && digit <= 'F') codePoint += digit - 'A' + 10U;
          else return false;
        }
        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
          if (position + 6U > text.size() || text[position] != '\\' || text[position + 1U] != 'u') return false;
          position += 2U;
          unsigned int low = 0;
          for (size_t index = 0; index < 4U; ++index) {
            const unsigned char digit = static_cast<unsigned char>(text[position++]);
            low <<= 4U;
            if (digit >= '0' && digit <= '9') low += digit - '0';
            else if (digit >= 'a' && digit <= 'f') low += digit - 'a' + 10U;
            else if (digit >= 'A' && digit <= 'F') low += digit - 'A' + 10U;
            else return false;
          }
          if (low < 0xDC00U || low > 0xDFFFU) return false;
          codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
        } else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
          return false;
        }
        if (codePoint <= 0x7FU) {
          output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
          output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
          output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0xFFFFU) {
          output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
          output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
          output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else {
          output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
          output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
          output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
          output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        break;
      }
      default: return false;
    }
  }
  return false;
}

bool jsonKeyCandidate(const std::string& json, size_t search, const std::string& marker, size_t& valuePosition) {
  size_t before = search;
  while (before > 0 && std::isspace(static_cast<unsigned char>(json[before - 1U])) != 0) --before;
  if (before == 0 || (json[before - 1U] != '{' && json[before - 1U] != ',' && json[before - 1U] != '[')) {
    return false;
  }
  valuePosition = search + marker.size();
  while (valuePosition < json.size() && std::isspace(static_cast<unsigned char>(json[valuePosition])) != 0) ++valuePosition;
  return valuePosition < json.size() && json[valuePosition] == ':';
}

bool jsonRootStringOrNullValuePresent(const std::string& json, const std::string& key,
                                      std::string& value, bool& isNull) {
  size_t position = 0;
  while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0) ++position;
  if (position >= json.size() || json[position++] != '{') return false;

  int depth = 1;
  bool found = false;
  isNull = false;
  while (position < json.size()) {
    const char ch = json[position];
    if (ch == '"') {
      size_t afterKey = position;
      std::string candidateKey;
      if (!parseJsonString(json, afterKey, candidateKey)) return false;
      if (depth == 1) {
        size_t valuePosition = afterKey;
        while (valuePosition < json.size() &&
               std::isspace(static_cast<unsigned char>(json[valuePosition])) != 0) {
          ++valuePosition;
        }
        if (valuePosition < json.size() && json[valuePosition] == ':' && candidateKey == key) {
          if (found) return false;
          found = true;
          ++valuePosition;
          while (valuePosition < json.size() &&
                 std::isspace(static_cast<unsigned char>(json[valuePosition])) != 0) {
            ++valuePosition;
          }
          if (valuePosition < json.size() && json[valuePosition] == '"') {
            if (!parseJsonString(json, valuePosition, value)) return false;
            isNull = false;
            position = valuePosition;
            continue;
          }
          if (json.compare(valuePosition, 4U, "null") == 0) {
            const auto afterNull = valuePosition + 4U;
            if (afterNull < json.size() && json[afterNull] != ',' && json[afterNull] != '}' &&
                std::isspace(static_cast<unsigned char>(json[afterNull])) == 0) {
              return false;
            }
            value.clear();
            isNull = true;
            position = afterNull;
            continue;
          }
          return false;
        }
      }
      position = afterKey;
      continue;
    }
    if (ch == '{' || ch == '[') {
      ++depth;
      ++position;
      continue;
    }
    if (ch == '}' || ch == ']') {
      if (depth <= 0) return false;
      --depth;
      ++position;
      if (depth == 0) break;
      continue;
    }
    ++position;
  }
  while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0) ++position;
  return depth == 0 && position == json.size() && found;
}

bool jsonRootStringValuePresent(const std::string& json, const std::string& key, std::string& value) {
  bool isNull = false;
  return jsonRootStringOrNullValuePresent(json, key, value, isNull) && !isNull;
}

bool jsonContainsStringValue(const std::string& json, const std::string& key, const std::string& expected) {
  const std::string marker = "\"" + key + "\"";
  for (size_t search = 0; (search = json.find(marker, search)) != std::string::npos; search += marker.size()) {
    size_t position = 0;
    if (!jsonKeyCandidate(json, search, marker, position)) continue;
    ++position;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0) ++position;
    std::string value;
    if (parseJsonString(json, position, value) && value == expected) return true;
  }
  return false;
}

#ifdef _WIN32
std::string fetchLatestReleaseJson(const std::string& repository) {
  if (!validRepository(repository)) return {};
  HINTERNET session = WinHttpOpen(L"Inventatory updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) return {};
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);
  HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (connection == nullptr) { WinHttpCloseHandle(session); return {}; }
  const std::wstring repositoryWide(repository.begin(), repository.end());
  // The `latest` endpoint intentionally excludes prereleases. The public beta
  // updater must be able to exercise GitHub prereleases as well, so request
  // the newest published release entry instead.
  const std::wstring path = L"/repos/" + repositoryWide + L"/releases?per_page=1";
  HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (request == nullptr ||
      !WinHttpAddRequestHeaders(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD) ||
      !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    if (request != nullptr) WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return {};
  }
  DWORD status = 0, statusSize = sizeof(status);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
      status < 200 || status >= 300) {
    WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return {};
  }
  std::string body;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available) || available > kMaximumReleaseMetadataBytes ||
        body.size() > kMaximumReleaseMetadataBytes - available) { body.clear(); break; }
    if (available == 0) break;
    const auto current = body.size();
    body.resize(current + available);
    DWORD read = 0;
    if (!WinHttpReadData(request, body.data() + current, available, &read)) { body.clear(); break; }
    body.resize(current + read);
  }
  WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
  return body;
}

bool parseHttpsUrl(const std::string& url, URL_COMPONENTS& components, std::wstring& wideUrl) {
  wideUrl = widen(url);
  if (wideUrl.empty()) return false;
  components = {};
  components.dwStructSize = sizeof(components);
  // A null component pointer is an output request only when its length is set
  // to -1. Without these sentinels WinHTTP accepts the URL but leaves the
  // host/path lengths empty, which makes every valid asset URL look invalid.
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  return WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components) != FALSE &&
         components.nScheme == INTERNET_SCHEME_HTTPS && components.lpszHostName != nullptr &&
         components.dwHostNameLength > 0 && components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0;
}

std::string urlComponent(const wchar_t* value, DWORD length) {
  if (value == nullptr || length == 0) return {};
  const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  if (sizeNeeded <= 0) return {};
  std::string result(static_cast<size_t>(sizeNeeded), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), result.data(), sizeNeeded, nullptr, nullptr);
  return result;
}

bool queryContentLength(HINTERNET request, std::uint64_t& total) {
  DWORD length = 0;
  DWORD lengthSize = sizeof(length);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
    total = 0;
    return true;
  }
  total = length;
  return total <= kMaximumUpdateAssetBytes;
}
#else
struct ReleaseMetadataResponse {
  std::string body;
  bool oversized = false;
};

size_t receiveReleaseMetadata(char* data, size_t size, size_t count, void* context) {
  auto* response = static_cast<ReleaseMetadataResponse*>(context);
  if (size != 0 && count > static_cast<size_t>(-1) / size) return 0;
  const auto bytes = size * count;
  if (bytes > kMaximumReleaseMetadataBytes || response->body.size() > kMaximumReleaseMetadataBytes - bytes) {
    response->oversized = true;
    return 0;
  }
  response->body.append(data, bytes);
  return bytes;
}

std::string fetchLatestReleaseJson(const std::string& repository) {
  if (!validRepository(repository)) return {};
  static const bool curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  if (!curlReady) return {};
  const std::string url = "https://api.github.com/repos/" + repository + "/releases?per_page=1";
  CURL* request = curl_easy_init();
  if (request == nullptr) return {};
  ReleaseMetadataResponse response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  curl_easy_setopt(request, CURLOPT_URL, url.c_str());
  curl_easy_setopt(request, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(request, CURLOPT_USERAGENT, "Inventatory updater");
  curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(request, CURLOPT_TIMEOUT_MS, 8000L);
  curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(request, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, receiveReleaseMetadata);
  curl_easy_setopt(request, CURLOPT_WRITEDATA, &response);
  const auto transfer = curl_easy_perform(request);
  long status = 0;
  if (transfer == CURLE_OK) curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(request);
  if (transfer != CURLE_OK || status < 200 || status >= 300 || response.oversized) return {};
  return response.body;
}
#endif

UpdateCheckResult latestRelease(const std::string& repository, const std::string& installedVersion) {
  const auto body = fetchLatestReleaseJson(repository);
  return parseReleaseMetadata(body, installedVersion, repository, repository == kReleaseRepository);
}

}  // namespace

bool isUpdateCheckDue(bool enabled, std::int64_t lastCheckUnixSeconds, std::int64_t nowUnixSeconds) {
  return enabled && (lastCheckUnixSeconds <= 0 || nowUnixSeconds - lastCheckUnixSeconds >= kUpdateCheckIntervalSeconds);
}

bool isVersionNewer(const std::string& candidate, const std::string& installed) {
  const auto candidateParts = versionParts(candidate);
  const auto installedParts = versionParts(installed);
  const auto candidatePrerelease = versionPrerelease(candidate);
  const auto installedPrerelease = versionPrerelease(installed);
  if (candidateParts.empty() || installedParts.empty() || !validPrerelease(candidatePrerelease) ||
      !validPrerelease(installedPrerelease)) {
    return false;
  }
  const auto count = std::max(candidateParts.size(), installedParts.size());
  for (size_t index = 0; index < count; ++index) {
    const int candidatePart = index < candidateParts.size() ? candidateParts[index] : 0;
    const int installedPart = index < installedParts.size() ? installedParts[index] : 0;
    if (candidatePart != installedPart) return candidatePart > installedPart;
  }
  if (candidatePrerelease.empty() != installedPrerelease.empty()) return candidatePrerelease.empty();
  if (candidatePrerelease == installedPrerelease) return false;

  std::istringstream candidateInput(candidatePrerelease);
  std::istringstream installedInput(installedPrerelease);
  std::string candidateIdentifier;
  std::string installedIdentifier;
  while (std::getline(candidateInput, candidateIdentifier, '.') &&
         std::getline(installedInput, installedIdentifier, '.')) {
    const bool candidateNumeric = !candidateIdentifier.empty() &&
                                  std::all_of(candidateIdentifier.begin(), candidateIdentifier.end(), [](unsigned char ch) {
                                    return std::isdigit(ch) != 0;
                                  });
    const bool installedNumeric = !installedIdentifier.empty() &&
                                  std::all_of(installedIdentifier.begin(), installedIdentifier.end(), [](unsigned char ch) {
                                    return std::isdigit(ch) != 0;
                                  });
    if (candidateNumeric && installedNumeric) {
      const auto candidateFirstDigit = candidateIdentifier.find_first_not_of('0');
      const auto installedFirstDigit = installedIdentifier.find_first_not_of('0');
      const auto candidateNormalized = candidateFirstDigit == std::string::npos
                                           ? std::string("0")
                                           : candidateIdentifier.substr(candidateFirstDigit);
      const auto installedNormalized = installedFirstDigit == std::string::npos
                                           ? std::string("0")
                                           : installedIdentifier.substr(installedFirstDigit);
      if (candidateNormalized.size() != installedNormalized.size()) {
        return candidateNormalized.size() > installedNormalized.size();
      }
      if (candidateNormalized != installedNormalized) return candidateNormalized > installedNormalized;
    } else if (candidateNumeric != installedNumeric) {
      return !candidateNumeric;
    } else if (candidateIdentifier != installedIdentifier) {
      return candidateIdentifier > installedIdentifier;
    }
  }
  const bool candidateRemaining = static_cast<bool>(std::getline(candidateInput, candidateIdentifier, '.'));
  const bool installedRemaining = static_cast<bool>(std::getline(installedInput, installedIdentifier, '.'));
  if (candidateRemaining != installedRemaining) return candidateRemaining;
  return false;
}

double updateEtaSeconds(std::uint64_t downloadedBytes, std::uint64_t totalBytes, double bytesPerSecond) {
  if (totalBytes == 0 || downloadedBytes >= totalBytes || !(bytesPerSecond > 0.0) || !std::isfinite(bytesPerSecond)) {
    return 0.0;
  }
  return static_cast<double>(totalBytes - downloadedBytes) / bytesPerSecond;
}

UpdateCheckResult parseReleaseMetadata(const std::string& json, const std::string& installedVersion,
                                       const std::string& repository, bool requireUpdateAssets) {
  if (json.empty() || json.size() > kMaximumReleaseMetadataBytes || !validRepository(repository)) return {};
  std::string releaseObject;
  size_t start = 0;
  while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])) != 0) ++start;
  if (start < json.size() && json[start] == '{') {
    releaseObject = json.substr(start);
  } else if (start < json.size() && json[start] == '[') {
    ++start;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])) != 0) ++start;
    if (start >= json.size()) return {};
    if (json[start] == ']') {
      // An optional channel, such as Scan R1 firmware before its first
      // published release, is healthy but has nothing to compare against.
      if (requireUpdateAssets) return {};
      return {true, false, {}, {}, {}};
    }
    if (json[start] != '{') return {};
    const auto objectStart = start;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; start < json.size(); ++start) {
      const char ch = json[start];
      if (inString) {
        if (escaped) escaped = false;
        else if (ch == '\\') escaped = true;
        else if (ch == '"') inString = false;
        continue;
      }
      if (ch == '"') inString = true;
      else if (ch == '{') ++depth;
      else if (ch == '}' && --depth == 0) {
        releaseObject = json.substr(objectStart, start - objectStart + 1U);
        break;
      }
    }
  }
  if (releaseObject.empty()) return {};
  std::string latestVersion;
  std::string releaseUrl;
  std::string releaseNotes;
  const bool hasTag = jsonRootStringValuePresent(releaseObject, "tag_name", latestVersion);
  const bool hasUrl = jsonRootStringValuePresent(releaseObject, "html_url", releaseUrl);
  bool bodyIsNull = false;
  const bool hasBody = jsonRootStringOrNullValuePresent(releaseObject, "body", releaseNotes, bodyIsNull);
  if (!hasTag || !hasUrl || (!hasBody && !bodyIsNull)) {
    return {};
  }
  const auto expectedReleasePrefix = "https://github.com/" + repository + "/releases/tag/" + latestVersion;
  if (!validReleaseTag(latestVersion) || releaseUrl != expectedReleasePrefix ||
      releaseNotes.size() > kMaximumReleaseNotesBytes) {
    return {};
  }
  const bool hasArchive = jsonContainsStringValue(releaseObject, "name", kApplicationArchiveName);
  const bool hasChecksums = jsonContainsStringValue(releaseObject, "name", kApplicationChecksumsName);
  const bool hasInstaller = jsonContainsStringValue(releaseObject, "name", kApplicationInstallerName);
  if (requireUpdateAssets && (!hasArchive || !hasChecksums || !hasInstaller)) {
    return {};
  }
  return {true, isVersionNewer(latestVersion, installedVersion), latestVersion, releaseUrl, releaseNotes};
}

std::string buildReleaseAssetUrl(const std::string& repository, const std::string& tag,
                                 const std::string& assetName) {
  if (!validRepository(repository) || !validReleaseTag(tag) || !allowedReleaseAsset(assetName)) {
    return {};
  }
  return "https://github.com/" + repository + "/releases/download/" + tag + "/" + assetName;
}

bool parseSha256Checksum(const std::string& checksums, const std::string& assetName,
                         std::string& expectedHash) {
  expectedHash.clear();
  if (!allowedReleaseAsset(assetName) || checksums.size() > kMaximumReleaseMetadataBytes) return false;
  std::istringstream input(checksums);
  std::string line;
  bool found = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::istringstream row(line);
    std::string digest;
    std::string file;
    if (!(row >> digest >> file) || digest.size() != 64U || !isHexDigest(digest)) continue;
    std::string extra;
    if (row >> extra) continue;
    if (!file.empty() && file.front() == '*') file.erase(file.begin());
    if (file != assetName) continue;
    if (found) return false;
    expectedHash = lowercaseAscii(digest);
    found = true;
  }
  return found;
}

bool downloadReleaseAsset(const std::string& url, const std::filesystem::path& destination,
                          const UpdateDownloadProgress& progress, std::string& error) {
  error.clear();
#ifdef _WIN32
  URL_COMPONENTS components{};
  std::wstring wideUrl;
  if (!parseHttpsUrl(url, components, wideUrl)) {
    error = "The update asset URL is invalid";
    return false;
  }
  const auto host = urlComponent(components.lpszHostName, components.dwHostNameLength);
  const auto path = urlComponent(components.lpszUrlPath, components.dwUrlPathLength);
  if (host != "github.com" || path.empty()) {
    error = "The update asset URL is not an approved GitHub download";
    return false;
  }
  // WinHTTP returns component pointers into the original URL. They are
  // length-delimited, not null-terminated, so passing them directly to the
  // LPCWSTR APIs can make the host include the following URL path.
  const auto wideHost = widen(host);
  const auto widePath = widen(path + urlComponent(components.lpszExtraInfo, components.dwExtraInfoLength));
  if (wideHost.empty() || widePath.empty()) {
    error = "The update asset URL is invalid";
    return false;
  }

  std::error_code filesystemError;
  std::filesystem::create_directories(destination.parent_path(), filesystemError);
  if (filesystemError) {
    error = "Could not create the update download folder";
    return false;
  }
  HINTERNET session = WinHttpOpen(L"Inventatory updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) { error = "Could not start the update download"; return false; }
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 15000);
  HINTERNET connection = WinHttpConnect(session, wideHost.c_str(), components.nPort, 0);
  HINTERNET request = connection == nullptr
                          ? nullptr
                          : WinHttpOpenRequest(connection, L"GET", widePath.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
  const bool sent = request != nullptr &&
                    WinHttpAddRequestHeaders(request, L"Accept: application/octet-stream\r\n",
                                              static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD) &&
                    WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                    WinHttpReceiveResponse(request, nullptr);
  if (!sent) {
    error = "Could not connect to the GitHub update asset";
    if (request != nullptr) WinHttpCloseHandle(request);
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }
  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  std::uint64_t total = 0;
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
      status < 200 || status >= 300 || !queryContentLength(request, total)) {
    error = "GitHub did not return a valid update asset";
    WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    return false;
  }

  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Could not create the downloaded update asset";
    WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    return false;
  }
  std::uint64_t downloaded = 0;
  bool successful = true;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) { successful = false; break; }
    if (available == 0) break;
    if (downloaded > kMaximumUpdateAssetBytes - available) { successful = false; break; }
    std::vector<char> buffer(available);
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0) {
      successful = false;
      break;
    }
    output.write(buffer.data(), static_cast<std::streamsize>(read));
    if (!output) { successful = false; break; }
    downloaded += read;
    if (progress && !progress(std::filesystem::path(path).filename().string(), downloaded, total)) {
      error = "Update download cancelled";
      successful = false;
      break;
    }
  }
  output.close();
  WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
  if (!successful) {
    if (error.empty()) error = "The update download was interrupted";
    std::filesystem::remove(destination, filesystemError);
    return false;
  }
  if (total != 0 && downloaded != total) {
    error = "The downloaded update asset was incomplete";
    std::filesystem::remove(destination, filesystemError);
    return false;
  }
  if (progress && !progress(std::filesystem::path(path).filename().string(), downloaded, total)) {
    error = "Update download cancelled";
    std::filesystem::remove(destination, filesystemError);
    return false;
  }
  return true;
#else
  constexpr const char* kDownloadPrefix = "https://github.com/";
  const std::string prefix(kDownloadPrefix);
  const auto repositoryBegin = prefix.size();
  const auto downloadMarker = url.find("/releases/download/", repositoryBegin);
  if (url.rfind(prefix, 0) != 0) {
    error = "The update asset URL is not an approved GitHub download";
    return false;
  }
  if (downloadMarker == std::string::npos) {
    error = "The update asset URL is invalid";
    return false;
  }
  const auto repository = url.substr(repositoryBegin, downloadMarker - repositoryBegin);
  const auto tagBegin = downloadMarker + std::string("/releases/download/").size();
  const auto tagEnd = url.find('/', tagBegin);
  if (tagEnd == std::string::npos) {
    error = "The update asset URL is invalid";
    return false;
  }
  const auto tag = url.substr(tagBegin, tagEnd - tagBegin);
  const auto assetName = url.substr(tagEnd + 1U);
  if (assetName.find('/') != std::string::npos || buildReleaseAssetUrl(repository, tag, assetName) != url) {
    error = "The update asset URL is not an approved GitHub download";
    return false;
  }
  static const bool curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  if (!curlReady) {
    error = "Could not start the update download";
    return false;
  }
  std::error_code filesystemError;
  if (!destination.parent_path().empty()) {
    std::filesystem::create_directories(destination.parent_path(), filesystemError);
    if (filesystemError) {
      error = "Could not create the update download folder";
      return false;
    }
  }
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Could not create the downloaded update asset";
    return false;
  }
  struct DownloadContext {
    std::ofstream* output = nullptr;
    const UpdateDownloadProgress* progress = nullptr;
    std::string assetName;
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    bool cancelled = false;
    bool oversized = false;
  } context{&output, &progress, assetName};
  CURL* request = curl_easy_init();
  if (request == nullptr) {
    output.close();
    std::filesystem::remove(destination, filesystemError);
    error = "Could not start the update download";
    return false;
  }
  const auto receive = +[](char* data, size_t size, size_t count, void* userData) -> size_t {
    auto* item = static_cast<DownloadContext*>(userData);
    if (size != 0 && count > static_cast<size_t>(-1) / size) return 0;
    const auto bytes = size * count;
    if (bytes > kMaximumUpdateAssetBytes || item->downloaded > kMaximumUpdateAssetBytes - bytes) {
      item->oversized = true;
      return 0;
    }
    item->output->write(data, static_cast<std::streamsize>(bytes));
    if (!*item->output) return 0;
    item->downloaded += bytes;
    return bytes;
  };
  const auto reportProgress = +[](void* userData, curl_off_t total, curl_off_t downloaded, curl_off_t,
                                  curl_off_t) -> int {
    auto* item = static_cast<DownloadContext*>(userData);
    if (downloaded < 0 || total < 0 || static_cast<std::uint64_t>(downloaded) > kMaximumUpdateAssetBytes ||
        static_cast<std::uint64_t>(total) > kMaximumUpdateAssetBytes) {
      item->oversized = true;
      return 1;
    }
    item->total = static_cast<std::uint64_t>(total);
    if (item->progress != nullptr && *item->progress &&
        !(*item->progress)(item->assetName, static_cast<std::uint64_t>(downloaded), item->total)) {
      item->cancelled = true;
      return 1;
    }
    return 0;
  };
  curl_easy_setopt(request, CURLOPT_URL, url.c_str());
  curl_easy_setopt(request, CURLOPT_USERAGENT, "Inventatory updater");
  curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(request, CURLOPT_TIMEOUT_MS, 15000L);
  curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(request, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(request, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaximumUpdateAssetBytes));
  curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(request, CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, reportProgress);
  curl_easy_setopt(request, CURLOPT_XFERINFODATA, &context);
  const auto transfer = curl_easy_perform(request);
  long status = 0;
  curl_off_t total = 0;
  if (transfer == CURLE_OK) {
    curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(request, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total);
  }
  curl_easy_cleanup(request);
  output.close();
  if (transfer != CURLE_OK || status < 200 || status >= 300 || !output || context.oversized || context.cancelled) {
    std::filesystem::remove(destination, filesystemError);
    if (context.cancelled) error = "Update download cancelled";
    else if (context.oversized) error = "The update asset exceeds the 512 MiB safety limit";
    else error = "Could not complete the GitHub update download";
    return false;
  }
  context.total = total > 0 ? static_cast<std::uint64_t>(total) : context.total;
  if (context.total != 0 && context.downloaded != context.total) {
    std::filesystem::remove(destination, filesystemError);
    error = "The downloaded update asset was incomplete";
    return false;
  }
  if (progress && !progress(assetName, context.downloaded, context.total)) {
    std::filesystem::remove(destination, filesystemError);
    error = "Update download cancelled";
    return false;
  }
  return true;
#endif
}

bool verifyReleaseFileSha256(const std::filesystem::path& path, const std::string& expectedHash,
                             std::string& error) {
  if (!isHexDigest(expectedHash)) {
    error = "The release checksum is invalid";
    return false;
  }
  std::string actualHash;
  if (!inventory_transfer_detail::sha256File(path, actualHash, error)) return false;
  if (lowercaseAscii(actualHash) != lowercaseAscii(expectedHash)) {
    error = "The downloaded update failed checksum verification";
    return false;
  }
  return true;
}

bool launchUpdateInstaller(const std::filesystem::path& installerPath,
                           const std::filesystem::path& archivePath,
                           const std::filesystem::path& checksumsPath,
                           const std::filesystem::path& markerPath,
                           const std::filesystem::path& notesPath,
                           const std::string& releaseVersion,
                           std::string& error) {
  std::error_code filesystemError;
  if (!std::filesystem::is_regular_file(installerPath, filesystemError) ||
      !std::filesystem::is_regular_file(archivePath, filesystemError) ||
      !std::filesystem::is_regular_file(checksumsPath, filesystemError)) {
    error = "The verified update package is incomplete";
    return false;
  }
#ifdef _WIN32
  wchar_t systemDirectory[MAX_PATH]{};
  const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) { error = "Could not locate PowerShell"; return false; }
  const std::filesystem::path powershell = std::filesystem::path(systemDirectory) / "WindowsPowerShell" /
                                           "v1.0" / "powershell.exe";
  const auto repository = widen(Inventatory_RELEASE_REPOSITORY);
  const auto version = widen(releaseVersion);
  if (repository.empty() || version.empty()) { error = "The update release identity is invalid"; return false; }
  std::wstring command = quoteWindowsArgument(powershell.wstring()) +
                         L" -NoLogo -NoProfile -ExecutionPolicy Bypass -File " + quoteWindowsArgument(installerPath.wstring()) +
                         L" -Repository " + quoteWindowsArgument(repository) + L" -UpdateMode -ArchivePath " +
                         quoteWindowsArgument(archivePath.wstring()) + L" -ChecksumsPath " +
                         quoteWindowsArgument(checksumsPath.wstring()) + L" -CompletionPath " +
                         quoteWindowsArgument(markerPath.wstring()) + L" -NotesPath " + quoteWindowsArgument(notesPath.wstring()) +
                         L" -ReleaseVersion " + quoteWindowsArgument(version) + L" -ParentProcessId " +
                         std::to_wstring(GetCurrentProcessId());
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(powershell.wstring().c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    error = "Could not start the update installer";
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#else
  if (releaseVersion.empty() || releaseVersion.size() > 128U ||
      std::any_of(releaseVersion.begin(), releaseVersion.end(), [](unsigned char ch) {
        return !(std::isalnum(ch) || ch == '.' || ch == '-' || ch == '+');
      })) {
    error = "The update release identity is invalid";
    return false;
  }
  int handshake[2]{-1, -1};
  if (pipe(handshake) != 0 || fcntl(handshake[1], F_SETFD, FD_CLOEXEC) != 0) {
    if (handshake[0] >= 0) close(handshake[0]);
    if (handshake[1] >= 0) close(handshake[1]);
    error = "Could not start the Linux update installer";
    return false;
  }
  const auto installer = installerPath.u8string();
  const auto archive = archivePath.u8string();
  const auto checksums = checksumsPath.u8string();
  const auto marker = markerPath.u8string();
  const auto notes = notesPath.u8string();
  const auto version = releaseVersion;
  const auto parent = std::to_string(static_cast<unsigned long long>(getpid()));
  const auto child = fork();
  if (child < 0) {
    close(handshake[0]);
    close(handshake[1]);
    error = "Could not start the Linux update installer";
    return false;
  }
  if (child == 0) {
    close(handshake[0]);
    if (setsid() < 0) {
      const int failure = errno;
      (void)write(handshake[1], &failure, sizeof(failure));
      _exit(127);
    }
    const int nullDevice = open("/dev/null", O_RDWR);
    if (nullDevice >= 0) {
      dup2(nullDevice, STDIN_FILENO);
      dup2(nullDevice, STDOUT_FILENO);
      dup2(nullDevice, STDERR_FILENO);
      if (nullDevice > STDERR_FILENO) close(nullDevice);
    }
    execl("/bin/sh", "sh", installer.c_str(), "--update", archive.c_str(), checksums.c_str(), marker.c_str(),
          notes.c_str(), version.c_str(), parent.c_str(), static_cast<char*>(nullptr));
    const int failure = errno;
    (void)write(handshake[1], &failure, sizeof(failure));
    _exit(127);
  }
  close(handshake[1]);
  int childError = 0;
  ssize_t count = 0;
  do {
    count = read(handshake[0], &childError, sizeof(childError));
  } while (count < 0 && errno == EINTR);
  close(handshake[0]);
  if (count > 0) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    error = "Could not start the Linux update installer";
    return false;
  }
  if (count < 0) {
    error = "Could not confirm the Linux update installer started";
    return false;
  }
  return true;
#endif
}

UpdateCheckResult checkLatestRelease(const std::string& installedVersion) { return latestRelease(kReleaseRepository, installedVersion); }
UpdateCheckResult checkLatestScanFirmwareRelease(const std::string& installedVersion) {
  if (!validRepository(kScanFirmwareRepository)) return {true, false, {}, {}, {}};
  return latestRelease(kScanFirmwareRepository, installedVersion);
}

}  // namespace inventatory
