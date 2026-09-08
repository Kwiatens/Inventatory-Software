// Inventatory - Hardware Inventory Management System
// Inventatory Scan R1 protocol types, validation, persistence, and stock mutation rules.

#pragma once

#include "core/inventory/Inventory.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace inventatory {

constexpr int kInventatoryScanTransportProtocolVersion = 1;

struct InventatoryScanConfig {
  std::string deviceId;
  std::string token;
  std::string fallbackHost;
  std::uint16_t fallbackPort = 0;
  bool setupComplete = false;

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

// A request id is only idempotent for the exact operation that created it.
// Keep the request and workspace generation beside the cached result so a
// reused id cannot replay a result for another device, payload, or workspace.
struct DeviceQuantityCacheEntry {
  DeviceQuantityRequest request;
  std::uint64_t workspaceGeneration = 0;
  DeviceQuantityResult result;
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
  // Filled from the durable inbox for pending events.  It is intentionally
  // not part of the on-wire event object: the authenticated sync envelope
  // already carries the sender identity.
  std::string deviceId;
};

struct DeviceSyncEventRecord {
  DeviceSyncEvent event;
  std::string deviceId;
  std::string state;
  std::string resultStatus;
  std::string resultCode;
  std::string resultMessage;
  time_t receivedAt = 0;
  time_t completedAt = 0;
  bool acknowledged = false;
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
  std::string deviceId;
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
  int protocolVersion = kInventatoryScanTransportProtocolVersion;
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
std::filesystem::path inventatoryScanReplayStatePath(const std::filesystem::path& workspaceDirectory);

std::string deviceRequestMac(const std::string& token, const std::string& method, const std::string& path,
                             const std::string& deviceId, std::uint64_t counter, const std::string& body);
std::string deviceResponseMac(const std::string& token, std::uint64_t counter, int status,
                              const std::string& body);
std::string deviceTransportStateFingerprint(const std::string& token);

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
std::vector<DeviceSyncEventRecord> loadDeviceSyncEventRecords(const std::filesystem::path& databasePath,
                                                              std::size_t limit = 64);
bool retryFailedDeviceSyncEvents(const std::filesystem::path& databasePath, std::size_t& retriedCount);
bool discardFailedDeviceSyncEvents(const std::filesystem::path& databasePath, std::size_t& discardedCount);
bool completeDeviceSyncEvent(InventoryStore& store, const std::filesystem::path& databasePath,
                              const DeviceSyncResult& result, const InventoryStore* previousStore = nullptr);
DeviceLookupResult lookupDeviceItem(const InventoryStore& store, const DeviceLookupRequest& request);
DeviceLookupResult lookupDeviceItem(const std::filesystem::path& databasePath,
                                    const DeviceLookupRequest& request);
DeviceQuantityResult applyDeviceQuantity(InventoryStore& store, const DeviceQuantityRequest& request);
DeviceQuantityResult applyDeviceQuantityCached(
    InventoryStore& store, const DeviceQuantityRequest& request,
    std::uint64_t workspaceGeneration,
    std::unordered_map<std::string, DeviceQuantityCacheEntry>& cache, std::deque<std::string>& order,
    std::size_t maxEntries = 64);
std::string debugResultJson(bool ok, const std::string& error = {});
std::string scanResultJson(bool ok, const std::string& error = {});
std::string quantityResultJson(const DeviceQuantityResult& result);
std::string statusResultJson(bool ok, const std::string& error = {});

}  // namespace inventatory
