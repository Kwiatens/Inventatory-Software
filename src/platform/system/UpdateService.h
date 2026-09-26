// Inventatory - GitHub release update discovery for public releases.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace inventatory {

struct UpdateCheckResult {
  bool completed = false;
  bool updateAvailable = false;
  std::string latestVersion;
  std::string releaseUrl;
  std::string releaseNotes;
};

using UpdateDownloadProgress =
    std::function<bool(const std::string& assetName, std::uint64_t downloadedBytes,
                       std::uint64_t totalBytes)>;

bool isUpdateCheckDue(bool enabled, std::int64_t lastCheckUnixSeconds, std::int64_t nowUnixSeconds);
bool isVersionNewer(const std::string& candidate, const std::string& installed);
double updateEtaSeconds(std::uint64_t downloadedBytes, std::uint64_t totalBytes, double bytesPerSecond);
UpdateCheckResult parseReleaseMetadata(const std::string& json, const std::string& installedVersion,
                                       const std::string& repository, bool requireUpdateAssets = true);
std::string buildReleaseAssetUrl(const std::string& repository, const std::string& tag,
                                 const std::string& assetName);
bool parseSha256Checksum(const std::string& checksums, const std::string& assetName,
                         std::string& expectedHash);
bool downloadReleaseAsset(const std::string& url, const std::filesystem::path& destination,
                          const UpdateDownloadProgress& progress, std::string& error);
bool verifyReleaseFileSha256(const std::filesystem::path& path, const std::string& expectedHash,
                             std::string& error);
bool launchUpdateInstaller(const std::filesystem::path& installerPath,
                           const std::filesystem::path& archivePath,
                           const std::filesystem::path& checksumsPath,
                           const std::filesystem::path& markerPath,
                           const std::filesystem::path& notesPath,
                           const std::string& releaseVersion,
                           std::string& error);
// Linux: replaces the exited application process with a waiter that lets the
// detached installer proceed, then execs the (updated or unchanged) executable
// in the same terminal once the completion marker leaves the pending state.
// Returns only when the relaunch could not be started. No-op on Windows, whose
// installer opens a new terminal itself.
void relaunchAfterUpdate(const std::filesystem::path& markerPath);
UpdateCheckResult checkLatestRelease(const std::string& installedVersion);

// Latest published Scan R1 firmware. `installedVersion` is the version the
// paired device last reported; an empty value still returns the latest release
// with `updateAvailable` false, because nothing is known to compare against.
UpdateCheckResult checkLatestScanFirmwareRelease(const std::string& installedVersion);

}  // namespace inventatory
