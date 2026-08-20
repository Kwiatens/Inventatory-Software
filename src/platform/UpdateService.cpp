// Inventatory - GitHub release update discovery for public releases.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/UpdateService.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace inventatory {
namespace {

constexpr std::int64_t kUpdateCheckIntervalSeconds = 24 * 60 * 60;
constexpr size_t kMaximumReleaseMetadataBytes = 1U * 1024U * 1024U;
constexpr char kReleaseRepository[] = Inventatory_RELEASE_REPOSITORY;
constexpr char kScanFirmwareRepository[] = Inventatory_SCAN_FIRMWARE_REPOSITORY;

std::string trimVersionPrefix(std::string value) {
  if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.erase(value.begin());
  return value;
}

std::vector<int> versionParts(const std::string& value) {
  std::vector<int> parts;
  std::istringstream input(trimVersionPrefix(value));
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
      default: return false;
    }
  }
  return false;
}

std::string jsonStringValue(const std::string& json, const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  size_t found = std::string::npos;
  for (size_t search = 0; (search = json.find(marker, search)) != std::string::npos; search += marker.size()) {
    const auto before = search == 0 ? ' ' : json[search - 1];
    const auto after = search + marker.size() < json.size() ? json[search + marker.size()] : ' ';
    if ((before == '{' || before == ',') && (after == ':' || std::isspace(static_cast<unsigned char>(after)) != 0)) {
      if (found != std::string::npos) return {};
      found = search;
    }
  }
  if (found == std::string::npos) return {};
  size_t position = found + marker.size();
  while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0) ++position;
  if (position >= json.size() || json[position++] != ':') return {};
  while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0) ++position;
  std::string value;
  return parseJsonString(json, position, value) ? value : std::string{};
}

std::string fetchLatestReleaseJson(const std::string& repository) {
  if (!validRepository(repository)) return {};
  HINTERNET session = WinHttpOpen(L"Inventatory updater/0.1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) return {};
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);
  HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (connection == nullptr) { WinHttpCloseHandle(session); return {}; }
  const std::wstring repositoryWide(repository.begin(), repository.end());
  const std::wstring path = L"/repos/" + repositoryWide + L"/releases/latest";
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

UpdateCheckResult latestRelease(const std::string& repository, const std::string& installedVersion) {
  const auto body = fetchLatestReleaseJson(repository);
  const auto latestVersion = jsonStringValue(body, "tag_name");
  const auto releaseUrl = jsonStringValue(body, "html_url");
  if (latestVersion.empty() || releaseUrl.rfind("https://", 0) != 0) return {};
  return {true, isVersionNewer(latestVersion, installedVersion), latestVersion, releaseUrl};
}

}  // namespace

bool isUpdateCheckDue(bool enabled, std::int64_t lastCheckUnixSeconds, std::int64_t nowUnixSeconds) {
  return enabled && (lastCheckUnixSeconds <= 0 || nowUnixSeconds - lastCheckUnixSeconds >= kUpdateCheckIntervalSeconds);
}

bool isVersionNewer(const std::string& candidate, const std::string& installed) {
  const auto candidateParts = versionParts(candidate);
  const auto installedParts = versionParts(installed);
  if (candidateParts.empty() || installedParts.empty()) return false;
  const auto count = std::max(candidateParts.size(), installedParts.size());
  for (size_t index = 0; index < count; ++index) {
    const int candidatePart = index < candidateParts.size() ? candidateParts[index] : 0;
    const int installedPart = index < installedParts.size() ? installedParts[index] : 0;
    if (candidatePart != installedPart) return candidatePart > installedPart;
  }
  return false;
}

UpdateCheckResult checkLatestRelease(const std::string& installedVersion) { return latestRelease(kReleaseRepository, installedVersion); }
UpdateCheckResult checkLatestScanFirmwareRelease(const std::string& installedVersion) { return latestRelease(kScanFirmwareRepository, installedVersion); }

}  // namespace inventatory
