// Inventatory - GitHub release update discovery for authenticated/private beta installs.

#pragma once

#include <cstdint>
#include <string>

namespace inventatory {

struct UpdateCheckResult {
  bool completed = false;
  bool updateAvailable = false;
  std::string latestVersion;
  std::string releaseUrl;
};

bool isUpdateCheckDue(bool enabled, std::int64_t lastCheckUnixSeconds, std::int64_t nowUnixSeconds);
bool isVersionNewer(const std::string& candidate, const std::string& installed);
UpdateCheckResult checkLatestPrivateBetaRelease(const std::string& installedVersion);

}  // namespace inventatory
