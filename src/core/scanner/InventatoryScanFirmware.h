// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Signed Scan R1 firmware packages, as docs/ota-protocol.md in the scanner firmware repository
// defines them. The desktop only couriers a package: it checks each package before offering it, and
// the scanner checks it again before it installs anything.

#pragma once

#include "core/scanner/InventatoryScanProtocol.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

constexpr std::size_t kScanFirmwarePackageHeaderSize = 320U;  // Header and signature, before the image.
constexpr std::size_t kScanFirmwareSignedSize = 256U;         // Bytes 0 to 255 are covered by the signature.
constexpr std::size_t kScanFirmwareSignatureSize = 64U;       // Raw ECDSA r || s, 32 bytes each.
// The scanner's update partition holds at most this much image.
constexpr std::size_t kScanFirmwareImageLimit = 1638400U;
// One chunk must fit the device API response, so it is bounded by the same limit.
constexpr std::size_t kScanFirmwareChunkLimit = kInventatoryScanResponseBodyLimit;
constexpr char kScanFirmwareProduct[] = "Inventatory-Scanner-R1";
constexpr char kScanFirmwareHardware[] = "waveshare-esp32s3-nano";

struct ScanFirmwareKey {
  std::string id;         // 16 lowercase hex characters: the first 8 bytes of SHA-256 of publicKey.
  std::string publicKey;  // 65 bytes: an uncompressed P-256 point, 0x04 followed by X and Y.
};

// Parses 130 hex characters, as `tools/sign_firmware.py pubkey` prints a key.
bool scanFirmwareKeyFromHex(const std::string& publicKeyHex, ScanFirmwareKey& key);
// Keys that may sign packages. Empty until the owner's key is added, so nothing is offered.
std::vector<ScanFirmwareKey> trustedScanFirmwareKeys();

struct ScanFirmwarePackage {
  std::string version;  // Shown on the scanner. The scanner does not compare it.
  std::uint32_t securityCounter = 0;
  std::string keyId;
  std::string imageSha256;  // 64 lowercase hex characters.
  std::string header;       // The 320 bytes before the image: header and signature.
  std::string image;        // The app image, firmware.bin.
};

// Checks a whole .ota package the way the scanner does. Fills `result` only when every check passes.
bool parseScanFirmwarePackage(const std::string& package, const std::vector<ScanFirmwareKey>& keys,
                              ScanFirmwarePackage& result, std::string& error);
bool loadScanFirmwarePackage(const std::filesystem::path& path, const std::vector<ScanFirmwareKey>& keys,
                             ScanFirmwarePackage& result, std::string& error);

struct ScanFirmwareOfferRequest {
  std::string product;
  std::string hardware;
  std::string version;  // The version the scanner runs now.
  std::uint64_t securityCounter = 0;
};

struct ScanFirmwareChunkRequest {
  std::string imageSha256;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

struct ScanFirmwareStatusReport {
  std::string imageSha256;
  std::string state;
  std::string error;
  std::string reason;
};

bool parseScanFirmwareOfferJson(const std::string& body, ScanFirmwareOfferRequest& request, std::string& error);
bool parseScanFirmwareChunkJson(const std::string& body, ScanFirmwareChunkRequest& request, std::string& error);
bool parseScanFirmwareStatusJson(const std::string& body, ScanFirmwareStatusReport& report, std::string& error);
// The offer answer. An empty `packageHex` means that nothing is offered.
std::string scanFirmwareOfferJson(const std::string& packageHex);

std::string scanFirmwareHex(const std::string& bytes);
// Plain words for a scanner's failure reason code: "battery too low" for "battery_low". Empty stays empty.
std::string scanFirmwareReasonText(const std::string& reason);
// The message for a scanner's install state. Empty for states that need no message, such as "installing".
std::string scanFirmwareOutcomeText(const std::string& state, const std::string& version, const std::string& reason);
// The 32 raw bytes of SHA-256 of `data`. Empty when the system hash fails.
std::string scanFirmwareSha256(const std::string& data);
// Whether `signature` (64 raw bytes) is an ECDSA P-256 signature of SHA-256(`signedData`) by `publicKey`.
bool scanFirmwareSignatureValid(const std::string& publicKey, const std::string& signedData,
                                const std::string& signature);

}  // namespace inventatory
