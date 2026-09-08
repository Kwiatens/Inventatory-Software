// Inventatory - Inventatory Scan R1 configuration persistence.

#include "core/InventatoryScanProtocol.h"
#include "core/AtomicFile.h"
#include "core/InventoryInternals.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kMaxScanConfigFileBytes = 8U * 1024U;
constexpr size_t kMaxScanConfigDeviceIdBytes = 128U;
constexpr size_t kMaxScanConfigHostBytes = 256U;

bool validScanConfigText(const string& value, size_t maximum) {
  return value.size() <= maximum &&
         all_of(value.begin(), value.end(), [](unsigned char ch) {
           return ch != 0 && ch != '\r' && ch != '\n';
         });
}

bool validScanConfig(const InventatoryScanConfig& config) {
  return validScanConfigText(config.deviceId, kMaxScanConfigDeviceIdBytes) &&
         validScanConfigText(config.fallbackHost, kMaxScanConfigHostBytes) && config.fallbackPort <= 65535;
}

}  // namespace

bool InventatoryScanConfig::paired() const {
  return setupComplete;
}

bool loadInventatoryScanConfig(const filesystem::path& path, InventatoryScanConfig& config) {
  error_code sizeError;
  const auto fileSize = filesystem::file_size(path, sizeError);
  if (sizeError || fileSize > kMaxScanConfigFileBytes) return false;
  ifstream input(path);
  if (!input) return false;
  InventatoryScanConfig loaded;
  bool malformedLine = false;
  unordered_set<string> seenKeys;
  bool hasDeviceId = false;
  bool hasFallbackHost = false;
  bool hasFallbackPort = false;
  bool hasSetupComplete = false;
  string line;
  while (getline(input, line)) {
    const auto separator = line.find('=');
    if (line.size() > kMaxScanConfigFileBytes || separator == string::npos || separator == 0) {
      malformedLine = true;
      continue;
    }
    const auto key = trim(line.substr(0, separator));
    const auto value = trim(line.substr(separator + 1));
    if ((key != "device_id" && key != "fallback_host" && key != "fallback_port" && key != "setup_complete") ||
        !seenKeys.insert(key).second) {
      return false;
    }
    if (key == "device_id") {
      if (!validScanConfigText(value, kMaxScanConfigDeviceIdBytes)) return false;
      loaded.deviceId = value;
      hasDeviceId = true;
    } else if (key == "fallback_host") {
      if (!validScanConfigText(value, kMaxScanConfigHostBytes)) return false;
      loaded.fallbackHost = value;
      hasFallbackHost = true;
    } else if (key == "fallback_port") {
      if (value.empty() || any_of(value.begin(), value.end(), [](unsigned char ch) { return !isdigit(ch); })) return false;
      uint64_t port = 0;
      try {
        size_t consumed = 0;
        port = stoull(value, &consumed, 10);
        if (consumed != value.size() || port > 65535) return false;
      } catch (...) {
        return false;
      }
      loaded.fallbackPort = static_cast<uint16_t>(port);
      hasFallbackPort = true;
    } else if (key == "setup_complete") {
      if (value == "true" || value == "1") {
        loaded.setupComplete = true;
      } else if (value == "false" || value == "0") {
        loaded.setupComplete = false;
      } else {
        return false;
      }
      hasSetupComplete = true;
    }
  }
  if (!input.eof() || malformedLine || !(hasDeviceId && hasFallbackHost && hasFallbackPort)) return false;
  // Older config files had no setup marker. A stored device identity means
  // that pairing had already completed before the marker was introduced.
  if (!hasSetupComplete) loaded.setupComplete = !loaded.deviceId.empty();
  config = move(loaded);
  return true;
}

bool saveInventatoryScanConfig(const filesystem::path& path, const InventatoryScanConfig& config) {
  if (!validScanConfig(config)) return false;
  ostringstream output;
  output << "device_id=" << config.deviceId << '\n'
         << "fallback_host=" << config.fallbackHost << '\n'
         << "fallback_port=" << config.fallbackPort << '\n'
         << "setup_complete=" << (config.setupComplete ? "true" : "false") << '\n';
  const auto text = output.str();
  if (text.size() > kMaxScanConfigFileBytes) return false;
  string error;
  return writeFileAtomically(path, text, &error);
}

}  // namespace inventatory
