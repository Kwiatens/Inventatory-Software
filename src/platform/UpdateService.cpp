// Inventatory - GitHub release update discovery for authenticated/private beta installs.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/UpdateService.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <vector>

namespace inventatory {

namespace {

constexpr std::int64_t kUpdateCheckIntervalSeconds = 24 * 60 * 60;

std::string trimVersionPrefix(std::string value) {
  if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.erase(value.begin());
  return value;
}

std::vector<int> versionParts(const std::string& value) {
  std::vector<int> parts;
  std::istringstream input(trimVersionPrefix(value));
  std::string part;
  while (std::getline(input, part, '.')) {
    if (part.empty() || !std::all_of(part.begin(), part.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
      return {};
    }
    parts.push_back(std::stoi(part));
  }
  return parts;
}

std::string jsonStringValue(const std::string& json, const std::string& key) {
  const std::string marker = "\"" + key + "\"";
  const auto keyPos = json.find(marker);
  if (keyPos == std::string::npos) return {};
  const auto colon = json.find(':', keyPos + marker.size());
  const auto quote = colon == std::string::npos ? std::string::npos : json.find('"', colon + 1);
  if (quote == std::string::npos) return {};
  std::string value;
  bool escaped = false;
  for (size_t index = quote + 1; index < json.size(); ++index) {
    const char ch = json[index];
    if (escaped) {
      value.push_back(ch);
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return value;
    } else {
      value.push_back(ch);
    }
  }
  return {};
}

std::string fetchLatestReleaseJson(const wchar_t* repository) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &security, 0) || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
    if (readPipe != nullptr) CloseHandle(readPipe);
    if (writePipe != nullptr) CloseHandle(writePipe);
    return {};
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdOutput = writePipe;
  startup.hStdError = writePipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  // CreateProcessW may modify the command line in place, so keep it writable.
  std::wstring command = std::wstring(L"gh api repos/") + repository + L"/releases/latest";
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process)) {
    CloseHandle(readPipe);
    CloseHandle(writePipe);
    return {};
  }
  CloseHandle(writePipe);
  if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    return {};
  }
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exitCode != 0) {
    CloseHandle(readPipe);
    return {};
  }
  std::string body;
  std::array<char, 4096> chunk{};
  DWORD bytesRead = 0;
  while (ReadFile(readPipe, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr) && bytesRead != 0 &&
         body.size() < 128 * 1024) {
    body.append(chunk.data(), bytesRead);
  }
  CloseHandle(readPipe);
  return body;
}

UpdateCheckResult latestRelease(const wchar_t* repository, const std::string& installedVersion) {
  const auto body = fetchLatestReleaseJson(repository);
  if (body.empty()) return {};
  const auto latestVersion = jsonStringValue(body, "tag_name");
  const auto releaseUrl = jsonStringValue(body, "html_url");
  if (latestVersion.empty() || releaseUrl.empty()) return {};
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

UpdateCheckResult checkLatestPrivateBetaRelease(const std::string& installedVersion) {
  return latestRelease(L"Kwiatens/Inventatory-Software", installedVersion);
}

UpdateCheckResult checkLatestScanFirmwareRelease(const std::string& installedVersion) {
  return latestRelease(L"Kwiatens/Inventatory-Hardware", installedVersion);
}

}  // namespace inventatory
