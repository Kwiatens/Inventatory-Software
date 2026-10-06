// Inventatory - GitHub release update discovery for public releases.

#pragma once

#include <chrono>
#include <cstddef>
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
// A new, empty, private download folder for one update ("Inventatory-update-*" under `parent`, or the
// system temporary folder when `parent` is empty). Never reuses an existing folder; empty on failure.
// On Linux its mode is 0700. The installer removes it once the update has been applied or has failed.
std::filesystem::path createUpdateDownloadDirectory(const std::filesystem::path& parent = {});
// Removes "Inventatory-update-*" folders under `parent` (the system temporary folder when empty) that were
// last modified more than `minimumAge` ago: leftovers of an update that was abandoned, crashed or whose
// installer never ran. Only real folders owned by the current user are touched, and a fresh folder that an
// update in progress still uses is never old enough to match. Returns how many folders were removed.
std::size_t removeStaleUpdateDownloadDirectories(const std::filesystem::path& parent = {},
                                                 std::chrono::seconds minimumAge = std::chrono::hours(24));
// Whether the folder holding `executable` can be written, so an update can replace it. Always true
// on Windows, whose installer reports its own failures; true when the path is unknown.
bool isInstallDirectoryWritable(const std::filesystem::path& executable);
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
