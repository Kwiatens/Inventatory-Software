// Inventatory - Hardware Inventory Management System
// Inventatory Scan R1 protocol types, validation, persistence, and stock mutation rules.

#include "core/InventatoryScanProtocol.h"
#include "core/InventatoryScanProtocolPrivate.h"
#include "core/InventoryInternals.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>

namespace inventatory {

using namespace std;
using namespace scan_protocol_detail;

namespace {

bool looksLikeSupportedInventatoryScanCode(const string& code) {
  const auto trimmed = trim(code);
  return !trimmed.empty() &&
         all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) { return isdigit(ch) != 0; });
}

// Mirrors isStandardManufacturerPartNumber() in the R1 firmware. A component
// Data Matrix carries the manufacturer part number, which is what the Scan R1
// puts in `code`, so the desktop must accept the same shape the device does.
bool looksLikeManufacturerPartNumber(const string& code) {
  const auto trimmed = trim(code);
  if (trimmed.size() < 2 || trimmed.size() > 64) return false;
  bool hasAlphanumeric = false;
  for (const unsigned char ch : trimmed) {
    if (isalnum(ch) != 0) {
      hasAlphanumeric = true;
      continue;
    }
    if (strchr("-._/+#&,() ", ch) == nullptr) return false;
  }
  return hasAlphanumeric;
}

bool looksLikeSupportedLookupCode(const string& code) {
  const auto trimmed = trim(code);
  if (looksLikeSupportedInventatoryScanCode(trimmed)) return true;
  return looksLikeManufacturerPartNumber(trimmed);
}

}  // namespace

bool parseQuantityRequestJson(const string& body, DeviceQuantityRequest& request, string& error) {
  if (!jsonObjectIsComplete(body)) {
    error = "Invalid JSON request";
    return false;
  }
  const auto deviceId = jsonString(body, "deviceId");
  const auto requestId = jsonString(body, "requestId");
  const auto code = jsonString(body, "code");
  const auto delta = jsonInt(body, "delta");
  if (!deviceId || trim(*deviceId).empty() || !requestId || trim(*requestId).empty() || !code || !delta) {
    error = "Missing or invalid deviceId, requestId, code, or delta";
    return false;
  }
  if (requestId->size() > 96 || deviceId->size() > 96 || code->size() > 128) {
    error = "Request field is too long";
    return false;
  }
  if (*delta == 0 || *delta < -999999 || *delta > 999999) {
    error = "Delta must be between -999999 and 999999 and cannot be zero";
    return false;
  }
  request = {*deviceId, *requestId, *code, *delta};
  return true;
}

bool parseScanRequestJson(const string& body, DeviceScanRequest& request, string& error) {
  if (!jsonObjectIsComplete(body)) {
    error = "Invalid JSON request";
    return false;
  }
  const auto deviceId = jsonString(body, "deviceId");
  const auto requestId = jsonString(body, "requestId");
  const auto code = jsonString(body, "code");
  const auto quantity = jsonInt(body, "quantity");
  if (!deviceId || trim(*deviceId).empty() || !requestId || trim(*requestId).empty() || !code ||
      trim(*code).empty()) {
    error = "Missing or invalid deviceId, requestId, or code";
    return false;
  }
  if (requestId->size() > 96 || deviceId->size() > 96 || code->size() > 128) {
    error = "Request field is too long";
    return false;
  }
  if (quantity && *quantity <= 0) {
    error = "Quantity must be positive";
    return false;
  }
  request = {*deviceId, *requestId, *code, quantity ? *quantity : 1};
  return true;
}

bool parseDebugReportJson(const string& body, DeviceDebugReport& report, string& error) {
  if (!jsonObjectIsComplete(body)) {
    error = "Invalid JSON request";
    return false;
  }
  const auto deviceId = jsonString(body, "deviceId");
  const auto requestId = jsonString(body, "requestId");
  const auto level = jsonString(body, "level");
  const auto message = jsonString(body, "message");
  if (!deviceId || trim(*deviceId).empty() || !requestId || trim(*requestId).empty() || !message ||
      trim(*message).empty()) {
    error = "Missing or invalid deviceId, requestId, or message";
    return false;
  }
  if (requestId->size() > 96 || deviceId->size() > 96 || message->size() > 512 || (level && level->size() > 24)) {
    error = "Debug field is too long";
    return false;
  }
  report = {*deviceId, *requestId, level ? *level : string("info"), *message};
  return true;
}

bool parseStatusReportJson(const string& body, DeviceStatusReport& report, string& error) {
  if (!jsonObjectIsComplete(body)) {
    error = "Invalid JSON request";
    return false;
  }
  const auto deviceId = jsonString(body, "deviceId");
  const auto version = jsonString(body, "firmwareVersion");
  const auto rssi = jsonInt(body, "rssi");
  const auto debug = jsonString(body, "debug");
  if (!deviceId || trim(*deviceId).empty() || !version || !rssi) {
    error = "Missing or invalid deviceId, firmwareVersion, or rssi";
    return false;
  }
  report = {*deviceId, *version, *rssi, debug ? *debug : string{}, 0, {}, 0};
  return true;
}

bool parseDeviceSyncRequestJson(const string& body, DeviceSyncRequest& request, string& error) {
  if (!jsonObjectIsComplete(body)) {
    error = "Invalid JSON request";
    return false;
  }
  const auto protocolVersion = jsonInt(body, "protocolVersion");
  const auto requestId = jsonString(body, "requestId");
  const auto deviceId = jsonString(body, "deviceId");
  const auto firmwareVersion = jsonString(body, "firmwareVersion");
  const auto mode = jsonString(body, "mode");
  const auto rssi = jsonInt(body, "rssi");
  const auto queueDepth = jsonInt(body, "queueDepth");
  if (!protocolVersion || !requestId || trim(*requestId).empty() || !deviceId || trim(*deviceId).empty() ||
      !firmwareVersion || !mode || !rssi || !queueDepth) {
    error = "Missing or invalid sync envelope field";
    return false;
  }
  if (*protocolVersion != kInventatoryScanTransportProtocolVersion) {
    error = "Unsupported protocol version";
    return false;
  }
  if (requestId->size() > 96 || deviceId->size() > 96 || firmwareVersion->size() > 32 || mode->size() > 32 ||
      *queueDepth < 0 || *queueDepth > 16) {
    error = "Sync envelope field is out of range";
    return false;
  }

  DeviceSyncRequest parsed;
  parsed.protocolVersion = *protocolVersion;
  parsed.requestId = *requestId;
  parsed.deviceId = *deviceId;
  parsed.firmwareVersion = *firmwareVersion;
  parsed.mode = *mode;
  parsed.rssi = *rssi;
  parsed.queueDepth = *queueDepth;
  const auto resultAcks = jsonStringArray(body, "resultAcks");
  if (!resultAcks) {
    error = "Invalid result acknowledgements";
    return false;
  }
  parsed.resultAcks = *resultAcks;
  if (parsed.resultAcks.size() > 4 ||
      any_of(parsed.resultAcks.begin(), parsed.resultAcks.end(), [](const string& value) {
        return value.empty() || value.size() > 96;
      })) {
    error = parsed.resultAcks.size() > 4 ? "Too many result acknowledgements"
                                        : "Invalid result acknowledgement";
    return false;
  }

  const auto eventObjects = jsonObjectArray(body, "events");
  if (!eventObjects) {
    error = "Invalid sync events";
    return false;
  }
  for (const auto& object : *eventObjects) {
    const auto eventId = jsonString(object, "eventId");
    const auto type = jsonString(object, "type");
    const auto code = jsonString(object, "code");
    const auto value = jsonInt(object, "value");
    if (!eventId || trim(*eventId).empty() || !type || trim(*type).empty() || !code || trim(*code).empty() ||
        !value || eventId->size() > 96 || type->size() > 48 || code->size() > 128) {
      error = "Invalid sync event";
      return false;
    }
    if (any_of(parsed.events.begin(), parsed.events.end(), [&](const DeviceSyncEvent& event) {
          return event.eventId == *eventId;
        })) {
      error = "Duplicate sync event id";
      return false;
    }
    parsed.events.push_back({*eventId, *type, *code, *value});
  }
  if (parsed.events.size() > 4) {
    error = "Too many sync events";
    return false;
  }
  if (const auto lookup = jsonObjectBody(body, "lookup")) {
    const auto lookupId = jsonString(*lookup, "lookupId");
    const auto code = jsonString(*lookup, "code");
    if (!lookupId || trim(*lookupId).empty() || !code || lookupId->size() > 96 || code->size() > 128) {
      error = "Invalid sync lookup";
      return false;
    }
    // A lookup is informational and never changes inventory, so a code this
    // build cannot resolve is dropped instead of failing the envelope. The
    // queued inventory events travelling with it must still be delivered.
    if (looksLikeSupportedLookupCode(*code)) {
      parsed.hasLookup = true;
      parsed.lookup = {*lookupId, *code};
    }
  }
  if (const auto print = jsonObjectBody(body, "quickLabelPrint")) {
    const auto printId = jsonString(*print, "requestId");
    const auto presetIndex = jsonInt(*print, "presetIndex");
    const auto revision = jsonInt(*print, "revision");
    if (!printId || trim(*printId).empty() || !presetIndex || !revision || printId->size() > 96 ||
        *presetIndex < 1 || *presetIndex > static_cast<int>(kQuickLabelPresetLimit) || *revision < 1) {
      error = "Invalid quick-label print request";
      return false;
    }
    parsed.hasQuickLabelPrint = true;
    parsed.quickLabelPrint = {*printId, *presetIndex, static_cast<uint32_t>(*revision)};
  }
  request = move(parsed);
  return true;
}

string deviceSyncResponseJson(const DeviceSyncResponse& response) {
  ostringstream out;
  out << "{\"protocolVersion\":" << kInventatoryScanTransportProtocolVersion << ",\"requestId\":\""
      << jsonEscape(response.requestId)
      << "\",\"acceptedEventIds\":[";
  for (size_t index = 0; index < response.acceptedEventIds.size(); ++index) {
    if (index != 0) out << ',';
    out << '"' << jsonEscape(response.acceptedEventIds[index]) << '"';
  }
  out << "],\"results\":[";
  for (size_t index = 0; index < response.results.size(); ++index) {
    if (index != 0) out << ',';
    const auto& result = response.results[index];
    out << "{\"resultId\":\"" << jsonEscape(result.resultId)
        << "\",\"eventId\":\"" << jsonEscape(result.eventId)
        << "\",\"status\":\"" << jsonEscape(result.status)
        << "\",\"existing\":" << (result.existing ? "true" : "false")
        << ",\"itemName\":\"" << jsonEscape(result.itemName)
        << "\",\"requestedDelta\":" << result.requestedDelta
        << ",\"appliedDelta\":" << result.appliedDelta
        << ",\"quantity\":" << result.quantity
        << ",\"location\":\"" << jsonEscape(result.location)
        << "\",\"code\":\"" << jsonEscape(result.code)
        << "\",\"message\":\"" << jsonEscape(result.message) << "\"}";
  }
  out << ']';
  if (response.hasLookupResult) {
    out << ",\"lookupResult\":{\"lookupId\":\"" << jsonEscape(response.lookupResult.lookupId)
        << "\",\"status\":\"" << jsonEscape(response.lookupResult.status)
        << "\",\"itemName\":\"" << jsonEscape(response.lookupResult.itemName) << "\"}";
  }
  if (response.hasQuickLabels) {
    out << ",\"quickLabels\":{\"revision\":" << response.quickLabelRevision << ",\"presets\":[";
    for (size_t index = 0; index < response.quickLabelPresets.size(); ++index) {
      if (index != 0) out << ',';
      out << '"' << jsonEscape(response.quickLabelPresets[index]) << '"';
    }
    out << "]}";
  }
  if (response.hasQuickLabelPrintResult) {
    const auto& result = response.quickLabelPrintResult;
    out << ",\"quickLabelPrintResult\":{\"requestId\":\"" << jsonEscape(result.requestId)
        << "\",\"status\":\"" << jsonEscape(result.status) << "\",\"code\":\""
        << jsonEscape(result.code) << "\",\"message\":\"" << jsonEscape(result.message) << "\"}";
  }
  out << '}';
  return out.str();
}

DeviceLookupResult lookupDeviceItem(const InventoryStore& store, const DeviceLookupRequest& request) {
  DeviceLookupResult result;
  result.lookupId = request.lookupId;
  const auto code = trim(request.code);
  if (!looksLikeSupportedLookupCode(code)) {
    result.status = "not_found";
    return result;
  }
  const InventoryItem* item = nullptr;
  if (looksLikeSupportedInventatoryScanCode(code)) {
    item = store.findByMachineCode(code);
  } else {
    const auto foldedCode = [&code] {
      string folded = code;
      transform(folded.begin(), folded.end(), folded.begin(),
                [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
      return folded;
    }();
    const auto match = find_if(store.items().begin(), store.items().end(), [&](const InventoryItem& candidate) {
      auto equalsCode = [&foldedCode](string value) {
        value = trim(value);
        transform(value.begin(), value.end(), value.begin(),
                  [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
        return value == foldedCode;
      };
      return equalsCode(candidate.digikeyPartNumber) || equalsCode(candidate.sku);
    });
    if (match != store.items().end()) item = &(*match);
  }
  if (item == nullptr) {
    result.status = "not_found";
    return result;
  }
  result.status = "found";
  result.itemName = item->partName;
  return result;
}

DeviceQuantityResult applyDeviceQuantity(InventoryStore& store, const DeviceQuantityRequest& request) {
  DeviceQuantityResult result;
  result.requestedDelta = request.delta;
  const auto code = trim(request.code);
  if (!looksLikeSupportedInventatoryScanCode(code)) {
    result.httpStatus = 400;
    result.error = "Only numeric machine codes can change stock";
    return result;
  }

  auto& items = store.items();
  const auto itemIt = find_if(items.begin(), items.end(), [&](const InventoryItem& item) {
    return matchesMachineCode(item.machineCode, code);
  });

  if (itemIt == items.end()) {
    result.httpStatus = 404;
    result.error = "Unknown machine code";
    return result;
  }

  auto* item = &*itemIt;
  const auto oldQuantity = item->quantity;
  const long long candidate = static_cast<long long>(oldQuantity) + request.delta;
  const auto newQuantity = static_cast<int>(clamp<long long>(candidate, 0, numeric_limits<int>::max()));
  item->quantity = newQuantity;
  item->lastUpdated = time(nullptr);
  result.httpStatus = 200;
  result.ok = true;
  result.item = item->partName;
  result.appliedDelta = newQuantity - oldQuantity;
  result.quantity = newQuantity;
  return result;
}

DeviceQuantityResult applyDeviceQuantityCached(InventoryStore& store, const DeviceQuantityRequest& request,
                                               uint64_t workspaceGeneration,
                                               unordered_map<string, DeviceQuantityCacheEntry>& cache,
                                               deque<string>& order, size_t maxEntries) {
  const auto cached = cache.find(request.requestId);
  if (cached != cache.end() && cached->second.workspaceGeneration == workspaceGeneration &&
      cached->second.request.deviceId == request.deviceId &&
      cached->second.request.requestId == request.requestId &&
      cached->second.request.code == request.code &&
      cached->second.request.delta == request.delta) {
    return cached->second.result;
  }

  const auto result = applyDeviceQuantity(store, request);
  if (find(order.begin(), order.end(), request.requestId) == order.end()) {
    order.push_back(request.requestId);
  }
  cache[request.requestId] = {request, workspaceGeneration, result};
  while (order.size() > maxEntries) {
    cache.erase(order.front());
    order.pop_front();
  }
  return result;
}

string scanResultJson(bool ok, const string& error) {
  return ok ? string("{\"ok\":true}") : string("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}";
}

string quantityResultJson(const DeviceQuantityResult& result) {
  ostringstream out;
  out << "{\"ok\":" << (result.ok ? "true" : "false");
  if (result.ok) {
    out << ",\"item\":\"" << jsonEscape(result.item) << "\""
        << ",\"requestedDelta\":" << result.requestedDelta
        << ",\"appliedDelta\":" << result.appliedDelta
        << ",\"quantity\":" << result.quantity;
  } else {
    out << ",\"error\":\"" << jsonEscape(result.error) << "\"";
  }
  out << '}';
  return out.str();
}

string debugResultJson(bool ok, const string& error) {
  return ok ? string("{\"ok\":true}") : string("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}";
}

string statusResultJson(bool ok, const string& error) {
  return ok ? string("{\"ok\":true}")
            : string("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}";
}

}  // namespace inventatory
