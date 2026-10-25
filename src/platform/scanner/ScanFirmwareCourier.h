// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Offers one checked Scan R1 firmware package to the paired scanner and serves its chunks.

#pragma once

#include "core/scanner/InventatoryScanFirmware.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace inventatory {

struct ScanFirmwareDownloadResult {
  bool completed = false;
  ScanFirmwarePackage package;
  std::string error;
};

// How far the offered package has got with the scanner, for the progress row.
struct ScanFirmwareTransfer {
  std::string version;                  // The offered version. Empty when no package is held.
  std::uint64_t imageBytes = 0;         // Size of the image the scanner takes in chunks.
  std::uint64_t sentBytes = 0;          // Furthest byte handed out. A retried chunk does not move it back.
  std::optional<std::chrono::steady_clock::time_point> lastChunkAt;  // Empty until the first chunk.
  ScanFirmwareStatusReport status;      // The scanner's last report for this package. Empty before one.
};

// The kept copy of the latest release package, inside the app data folder.
std::filesystem::path scanFirmwarePackagePath(const std::filesystem::path& dataPath);

class ScanFirmwareCourier {
 public:
  // Replaces the offered package. Only a package that parseScanFirmwarePackage accepted may be set.
  void setPackage(ScanFirmwarePackage package);
  void clear();
  // The offered version, or empty when no package is held.
  std::string packageVersion() const;
  // Whether a sync response should say that an update waits for a scanner running `scannerVersion`.
  bool offersTo(const std::string& scannerVersion) const;

  // Device API answers. Each returns the HTTP status and fills `body`.
  int offer(const ScanFirmwareOfferRequest& request, std::string& body) const;
  int chunk(const ScanFirmwareChunkRequest& request, std::string& body) const;
  void recordStatus(const ScanFirmwareStatusReport& report);
  ScanFirmwareStatusReport lastStatus() const;
  ScanFirmwareTransfer transfer() const;

 private:
  bool offeredTo(const std::string& scannerVersion, std::uint64_t scannerSecurityCounter) const;
  // Clears the progress and the scanner's report for the held package. Expects mutex_ to be held.
  void resetTransfer();

  mutable std::mutex mutex_;
  std::optional<ScanFirmwarePackage> package_;
  ScanFirmwareStatusReport lastStatus_;
  // Moved by chunk answers, so they are mutable like mutex_.
  mutable std::uint64_t sentBytes_ = 0;
  mutable std::optional<std::chrono::steady_clock::time_point> lastChunkAt_;
};

}  // namespace inventatory
