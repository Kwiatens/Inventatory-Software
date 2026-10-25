// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Offer and chunk answers for the checked Scan R1 firmware package.

#include "platform/scanner/ScanFirmwareCourier.h"

#include "platform/system/UpdateService.h"

#include <algorithm>
#include <chrono>

using namespace std;

namespace inventatory {

filesystem::path scanFirmwarePackagePath(const filesystem::path& dataPath) {
  return dataPath / "scanner-firmware" / "firmware.ota";
}

void ScanFirmwareCourier::setPackage(ScanFirmwarePackage package) {
  lock_guard<mutex> lock(mutex_);
  package_ = move(package);
  resetTransfer();
}

void ScanFirmwareCourier::clear() {
  lock_guard<mutex> lock(mutex_);
  package_.reset();
  resetTransfer();
}

string ScanFirmwareCourier::packageVersion() const {
  lock_guard<mutex> lock(mutex_);
  return package_ ? package_->version : string();
}

bool ScanFirmwareCourier::offersTo(const string& scannerVersion) const {
  // The sync flag is a hint. The sync request does not carry the scanner's security counter, so it is
  // not checked here; the offer route checks it before any package is sent.
  lock_guard<mutex> lock(mutex_);
  return offeredTo(scannerVersion, 0);
}

// Expects mutex_ to be held.
bool ScanFirmwareCourier::offeredTo(const string& scannerVersion, uint64_t scannerSecurityCounter) const {
  if (!package_ || package_->securityCounter < scannerSecurityCounter) return false;
  return isVersionNewer(package_->version, scannerVersion);
}

int ScanFirmwareCourier::offer(const ScanFirmwareOfferRequest& request, string& body) const {
  lock_guard<mutex> lock(mutex_);
  const bool sameDevice = request.product == kScanFirmwareProduct && request.hardware == kScanFirmwareHardware;
  if (sameDevice && offeredTo(request.version, request.securityCounter)) {
    body = scanFirmwareOfferJson(scanFirmwareHex(package_->header));
  } else {
    body = scanFirmwareOfferJson({});
  }
  return 200;
}

int ScanFirmwareCourier::chunk(const ScanFirmwareChunkRequest& request, string& body) const {
  lock_guard<mutex> lock(mutex_);
  if (!package_ || request.imageSha256 != package_->imageSha256) {
    body = R"({"error":"no_offer"})";
    return 404;
  }
  const auto& image = package_->image;
  if (request.length < 1U || request.length > kScanFirmwareChunkLimit || request.offset > image.size() ||
      request.length > image.size() - request.offset) {
    body = R"({"error":"bad_range"})";
    return 416;
  }
  body = image.substr(request.offset, request.length);
  sentBytes_ = max(sentBytes_, request.offset + request.length);
  lastChunkAt_ = chrono::steady_clock::now();
  return 200;
}

void ScanFirmwareCourier::recordStatus(const ScanFirmwareStatusReport& report) {
  lock_guard<mutex> lock(mutex_);
  lastStatus_ = report;
}

ScanFirmwareStatusReport ScanFirmwareCourier::lastStatus() const {
  lock_guard<mutex> lock(mutex_);
  return lastStatus_;
}

ScanFirmwareTransfer ScanFirmwareCourier::transfer() const {
  lock_guard<mutex> lock(mutex_);
  ScanFirmwareTransfer transfer;
  if (package_) {
    transfer.version = package_->version;
    transfer.imageBytes = package_->image.size();
  }
  transfer.sentBytes = sentBytes_;
  transfer.lastChunkAt = lastChunkAt_;
  transfer.status = lastStatus_;
  return transfer;
}

// Expects mutex_ to be held.
void ScanFirmwareCourier::resetTransfer() {
  sentBytes_ = 0;
  lastChunkAt_.reset();
  lastStatus_ = {};
}

}  // namespace inventatory
