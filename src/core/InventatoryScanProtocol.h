// Inventatory - Hardware Inventory Management System
// Inventatory Scan R1 protocol types, validation, persistence, and stock mutation rules.

#pragma once

#include "core/Inventory.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace inventatory {

struct InventatoryScanConfig {
  std::string deviceId;
  std::string token;
  std::string fallbackHost;
  std::uint16_t fallbackPort = 0;

  bool paired() const;
};

struct DeviceQuantityRequest {
  std::string deviceId;
  std::string requestId;
  std::string code;
  int delta = 0;
};

struct DeviceScanRequest {
  std::string deviceId;
  std::string requestId;
  std::string code;
  int quantity = 1;
};

struct DeviceDebugReport {
  std::string deviceId;
  std::string requestId;
  std::string level;
  std::string message;
};

struct DeviceQuantityResult {
  int httpStatus = 400;
  bool ok = false;
  std::string error;
  std::string item;
  int requestedDelta = 0;
  int appliedDelta = 0;
  int quantity = 0;
};

struct DeviceStatusReport {
  std::string deviceId;
  std::string firmwareVersion;
  int rssi = 0;
  std::string debug;
  int protocolVersion = 0;
  std::string mode;
  int pendingEventCount = 0;
};

struct DeviceSyncEvent {
  std::string eventId;
  std::string type;
  std::string code;
  int value = 0;
};

struct DeviceLookupRequest {
  std::string lookupId;
  std::string code;
};

struct DeviceLookupResult {
  std::string lookupId;
  std::string status;
  std::string itemName;
};

constexpr std::size_t kQuickLabelPresetLimit = 12;
constexpr std::size_t kQuickLabelPresetTextLimit = 24;

struct DeviceQuickLabelPrintRequest {
  std::string requestId;
  int presetIndex = 0;  // One-based index in the published ordered list.
  std::uint32_t revision = 0;
};

struct DeviceQuickLabelPrintResult {
  std::string requestId;
  std::string status;
  std::string code;
  std::string message;
};

struct DeviceSyncRequest {
  int protocolVersion = 0;
  std::string requestId;
  std::string deviceId;
  std::string firmwareVersion;
  std::string mode;
  int rssi = 0;
  int queueDepth = 0;
  std::vector<DeviceSyncEvent> events;
  std::vector<std::string> resultAcks;
  bool hasLookup = false;
  DeviceLookupRequest lookup;
  bool hasQuickLabelPrint = false;
  DeviceQuickLabelPrintRequest quickLabelPrint;
};

struct DeviceSyncResult {
  std::string resultId;
  std::string eventId;
  std::string status;
  bool existing = false;
  std::string itemName;
  int requestedDelta = 0;
  int appliedDelta = 0;
  int quantity = 0;
  std::string location;
  std::string code;
  std::string message;
};

struct DeviceSyncResponse {
  int protocolVersion = 1;
  std::string requestId;
  std::vector<std::string> acceptedEventIds;
  std::vector<DeviceSyncResult> results;
  bool hasLookupResult = false;
  DeviceLookupResult lookupResult;
  bool hasQuickLabels = false;
  std::uint32_t quickLabelRevision = 0;
  std::vector<std::string> quickLabelPresets;
  bool hasQuickLabelPrintResult = false;
  DeviceQuickLabelPrintResult quickLabelPrintResult;
};

bool loadInventatoryScanConfig(const std::filesystem::path& path, InventatoryScanConfig& config);
bool saveInventatoryScanConfig(const std::filesystem::path& path, const InventatoryScanConfig& config);
std::string generateInventatoryScanToken();

bool parseQuantityRequestJson(const std::string& body, DeviceQuantityRequest& request, std::string& error);
bool parseScanRequestJson(const std::string& body, DeviceScanRequest& request, std::string& error);
bool parseDebugReportJson(const std::string& body, DeviceDebugReport& report, std::string& error);
bool parseStatusReportJson(const std::string& body, DeviceStatusReport& report, std::string& error);
bool parseDeviceSyncRequestJson(const std::string& body, DeviceSyncRequest& request, std::string& error);
std::string deviceSyncResponseJson(const DeviceSyncResponse& response);
bool acceptDeviceSyncEvents(const std::filesystem::path& databasePath, const DeviceSyncRequest& request,
                            DeviceSyncResponse& response, std::string& error);
std::vector<DeviceSyncEvent> loadPendingDeviceSyncEvents(const std::filesystem::path& databasePath,
                                                         std::size_t limit = 4);
bool completeDeviceSyncEvent(InventoryStore& store, const std::filesystem::path& databasePath,
                             const DeviceSyncResult& result);
DeviceLookupResult lookupDeviceItem(const InventoryStore& store, const DeviceLookupRequest& request);
DeviceLookupResult lookupDeviceItem(const std::filesystem::path& databasePath,
                                    const DeviceLookupRequest& request);
DeviceQuantityResult applyDeviceQuantity(InventoryStore& store, const DeviceQuantityRequest& request);
DeviceQuantityResult applyDeviceQuantityCached(
    InventoryStore& store, const DeviceQuantityRequest& request,
    std::unordered_map<std::string, DeviceQuantityResult>& cache, std::deque<std::string>& order,
    std::size_t maxEntries = 64);
std::string debugResultJson(bool ok, const std::string& error = {});
std::string scanResultJson(bool ok, const std::string& error = {});
std::string quantityResultJson(const DeviceQuantityResult& result);
std::string statusResultJson(bool ok, const std::string& error = {});

}  // namespace inventatory
