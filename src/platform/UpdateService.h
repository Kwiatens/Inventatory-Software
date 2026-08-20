// Inventatory - GitHub release update discovery for public releases.

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
UpdateCheckResult checkLatestRelease(const std::string& installedVersion);

// Latest published Scan R1 firmware. `installedVersion` is the version the
// paired device last reported; an empty value still returns the latest release
// with `updateAvailable` false, because nothing is known to compare against.
UpdateCheckResult checkLatestScanFirmwareRelease(const std::string& installedVersion);

}  // namespace inventatory
