// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Parsing and checks for signed Scan R1 firmware packages and the firmware device API.

#include "core/scanner/InventatoryScanFirmware.h"

#include "core/scanner/InventatoryScanProtocolPrivate.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace inventatory {

using namespace std;
using namespace scan_protocol_detail;

namespace {

constexpr char kScanFirmwareMagic[] = "IVOT";
constexpr size_t kFormatOffset = 4;
constexpr size_t kHeaderSizeOffset = 6;
constexpr size_t kKeyIdOffset = 8;
constexpr size_t kKeyIdSize = 8;
constexpr size_t kProductOffset = 16;
constexpr size_t kHardwareOffset = 48;
constexpr size_t kVersionOffset = 80;
constexpr size_t kTextFieldSize = 32;
constexpr size_t kCounterOffset = 112;
constexpr size_t kImageSizeOffset = 116;
constexpr size_t kImageSha256Offset = 120;
constexpr size_t kReservedOffset = 152;
constexpr size_t kPublicKeySize = 65;
constexpr size_t kPublicKeyHexSize = 130;
constexpr size_t kSha256Size = 32;

// Public keys that may sign Scan R1 packages, 130 hex characters each. Empty until the owner's key
// is added, so no package is offered.
const vector<string>& trustedScanFirmwareKeyHex() {
  static const vector<string> keys{};
  return keys;
}

bool decodeHex(const string& hex, string& bytes) {
  if (hex.size() % 2U != 0) return false;
  bytes.clear();
  for (size_t i = 0; i < hex.size(); i += 2U) {
    const int high = hexDigit(hex[i]);
    const int low = hexDigit(hex[i + 1U]);
    if (high < 0 || low < 0) return false;
    bytes.push_back(static_cast<char>(high * 16 + low));
  }
  return true;
}

std::uint32_t readLittleEndian32(const string& bytes, size_t offset) {
  std::uint32_t value = 0;
  for (size_t i = 0; i < 4U; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8U * i);
  }
  return value;
}

std::uint16_t readLittleEndian16(const string& bytes, size_t offset) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset]) |
                                    (static_cast<unsigned char>(bytes[offset + 1U]) << 8U));
}

// Text fields are printable ASCII, NUL-terminated inside the field and zero-padded after it.
bool readTextField(const string& header, size_t offset, string& text) {
  const auto field = header.substr(offset, kTextFieldSize);
  const auto length = field.find('\0');
  if (length == string::npos || length == 0) return false;
  for (size_t i = 0; i < length; ++i) {
    const auto ch = static_cast<unsigned char>(field[i]);
    if (ch < 0x20U || ch > 0x7eU) return false;
  }
  for (size_t i = length; i < field.size(); ++i) {
    if (field[i] != '\0') return false;
  }
  text = field.substr(0, length);
  return true;
}

}  // namespace

bool scanFirmwareKeyFromHex(const string& publicKeyHex, ScanFirmwareKey& key) {
  if (publicKeyHex.size() != kPublicKeyHexSize) return false;
  string publicKey;
  if (!decodeHex(publicKeyHex, publicKey) || publicKey.size() != kPublicKeySize ||
      publicKey[0] != '\x04') {
    return false;
  }
  const auto digest = scanFirmwareSha256(publicKey);
  if (digest.size() != kSha256Size) return false;
  key.id = scanFirmwareHex(digest.substr(0, kKeyIdSize));
  key.publicKey = publicKey;
  return true;
}

vector<ScanFirmwareKey> trustedScanFirmwareKeys() {
  vector<ScanFirmwareKey> keys;
  for (const auto& publicKeyHex : trustedScanFirmwareKeyHex()) {
    ScanFirmwareKey key;
    if (scanFirmwareKeyFromHex(publicKeyHex, key)) keys.push_back(key);
  }
  return keys;
}

bool parseScanFirmwarePackage(const string& package, const vector<ScanFirmwareKey>& keys,
                              ScanFirmwarePackage& result, string& error) {
  if (package.size() <= kScanFirmwarePackageHeaderSize) {
    error = "Firmware package is too short";
    return false;
  }
  const auto header = package.substr(0, kScanFirmwarePackageHeaderSize);
  if (header.compare(0, sizeof(kScanFirmwareMagic) - 1U, kScanFirmwareMagic) != 0 ||
      readLittleEndian16(header, kFormatOffset) != 1U ||
      readLittleEndian16(header, kHeaderSizeOffset) != kScanFirmwareSignedSize) {
    error = "Not a Scan R1 firmware package";
    return false;
  }
  const auto imageSize = readLittleEndian32(header, kImageSizeOffset);
  if (imageSize == 0U || imageSize > kScanFirmwareImageLimit ||
      package.size() != kScanFirmwarePackageHeaderSize + imageSize) {
    error = "Firmware package size does not match its header";
    return false;
  }

  const auto keyId = scanFirmwareHex(header.substr(kKeyIdOffset, kKeyIdSize));
  const ScanFirmwareKey* trusted = nullptr;
  for (const auto& key : keys) {
    if (key.id == keyId) trusted = &key;
  }
  if (trusted == nullptr) {
    error = "Firmware package is not signed by a trusted key";
    return false;
  }
  for (size_t i = kReservedOffset; i < kScanFirmwareSignedSize; ++i) {
    if (header[i] != '\0') {
      error = "Firmware package has reserved bytes set";
      return false;
    }
  }
  if (!scanFirmwareSignatureValid(trusted->publicKey, header.substr(0, kScanFirmwareSignedSize),
                                  header.substr(kScanFirmwareSignedSize, kScanFirmwareSignatureSize))) {
    error = "Firmware package signature does not verify";
    return false;
  }

  string product;
  string hardware;
  string version;
  if (!readTextField(header, kProductOffset, product) || product != kScanFirmwareProduct ||
      !readTextField(header, kHardwareOffset, hardware) || hardware != kScanFirmwareHardware ||
      !readTextField(header, kVersionOffset, version)) {
    error = "Firmware package is for another product or hardware";
    return false;
  }

  const auto image = package.substr(kScanFirmwarePackageHeaderSize);
  const auto imageHash = scanFirmwareSha256(image);
  if (imageHash.size() != kSha256Size || imageHash != header.substr(kImageSha256Offset, kSha256Size)) {
    error = "Firmware image does not match the signed hash";
    return false;
  }

  result.version = version;
  result.securityCounter = readLittleEndian32(header, kCounterOffset);
  result.keyId = keyId;
  result.imageSha256 = scanFirmwareHex(imageHash);
  result.header = header;
  result.image = image;
  return true;
}

bool loadScanFirmwarePackage(const filesystem::path& path, const vector<ScanFirmwareKey>& keys,
                             ScanFirmwarePackage& result, string& error) {
  error_code fileError;
  const auto size = filesystem::file_size(path, fileError);
  if (fileError || size > kScanFirmwarePackageHeaderSize + kScanFirmwareImageLimit) {
    error = "Firmware package file is missing or too large";
    return false;
  }
  ifstream file(path, ios::binary);
  if (!file) {
    error = "Firmware package file cannot be read";
    return false;
  }
  const string package((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
  return parseScanFirmwarePackage(package, keys, result, error);
}

bool parseScanFirmwareOfferJson(const string& body, ScanFirmwareOfferRequest& request, string& error) {
  const auto product = jsonString(body, "product");
  const auto hardware = jsonString(body, "hardware");
  const auto version = jsonString(body, "version");
  const auto counter = jsonInt(body, "securityCounter");
  if (!product || !hardware || !version || !counter || *counter < 0) {
    error = "Invalid firmware offer request";
    return false;
  }
  request.product = *product;
  request.hardware = *hardware;
  request.version = *version;
  request.securityCounter = static_cast<std::uint64_t>(*counter);
  return true;
}

bool parseScanFirmwareChunkJson(const string& body, ScanFirmwareChunkRequest& request, string& error) {
  const auto imageSha256 = jsonString(body, "imageSha256");
  const auto offset = jsonInt(body, "offset");
  const auto length = jsonInt(body, "length");
  if (!imageSha256 || !offset || !length || *offset < 0 || *length < 0) {
    error = "Invalid firmware chunk request";
    return false;
  }
  request.imageSha256 = *imageSha256;
  request.offset = static_cast<std::uint64_t>(*offset);
  request.length = static_cast<std::uint64_t>(*length);
  return true;
}

bool parseScanFirmwareStatusJson(const string& body, ScanFirmwareStatusReport& report, string& error) {
  const auto imageSha256 = jsonString(body, "imageSha256");
  const auto state = jsonString(body, "state");
  const auto failure = jsonString(body, "error");
  const auto reason = jsonString(body, "reason");
  if (!imageSha256 || !state || !failure || !reason) {
    error = "Invalid firmware status report";
    return false;
  }
  report.imageSha256 = *imageSha256;
  report.state = *state;
  report.error = *failure;
  report.reason = *reason;
  return true;
}

string scanFirmwareOfferJson(const string& packageHex) {
  if (packageHex.empty()) return R"({"available":false})";
  return R"({"available":true,"package":")" + packageHex + R"("})";
}

string scanFirmwareHex(const string& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  string hex;
  hex.reserve(bytes.size() * 2U);
  for (const char ch : bytes) {
    const auto value = static_cast<unsigned char>(ch);
    hex.push_back(kDigits[value >> 4U]);
    hex.push_back(kDigits[value & 0x0fU]);
  }
  return hex;
}

string scanFirmwareReasonText(const string& reason) {
  if (reason == "battery_low") return "battery too low";
  string text = reason;
  replace(text.begin(), text.end(), '_', ' ');
  return text;
}

string scanFirmwareOutcomeText(const string& state, const string& version, const string& reason) {
  if (state == "installed") return "Scanner installed firmware " + version;
  if (state == "declined") return "Scanner declined firmware " + version;
  if (state == "failed") {
    const auto text = scanFirmwareReasonText(reason);
    return "Scanner could not install firmware " + version + (text.empty() ? string() : ": " + text);
  }
  return "";
}

}  // namespace inventatory
