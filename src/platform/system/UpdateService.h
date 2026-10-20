// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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
// New, empty, private "Inventatory-update-*" folder under `parent` (system temp folder when empty).
// Never reuses a folder; empty on failure. Mode 0700 on Linux. The installer removes it when done.
std::filesystem::path createUpdateDownloadDirectory(const std::filesystem::path& parent = {});
// Removes "Inventatory-update-*" folders under `parent` not modified for `minimumAge`: leftovers of
// abandoned or crashed updates. Only current-user folders are touched. Returns the count removed.
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
// Linux: replaces the process with a waiter that lets the detached installer run, then execs the
// executable in the same terminal once the marker leaves pending. Returns only if that fails.
// No-op on Windows, whose installer opens its own terminal.
void relaunchAfterUpdate(const std::filesystem::path& markerPath);
UpdateCheckResult checkLatestRelease(const std::string& installedVersion);

// Latest Scan R1 firmware. An empty `installedVersion` still returns the release, with
// `updateAvailable` false.
UpdateCheckResult checkLatestScanFirmwareRelease(const std::string& installedVersion);

}  // namespace inventatory
