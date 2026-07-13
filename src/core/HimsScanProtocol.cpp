// HIMS - Hardware Inventory Management System
// HIMS Scan R1 protocol types, validation, persistence, and stock mutation rules.

#include "core/HimsScanProtocol.h"
#include "core/InventoryInternals.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "Bcrypt.lib")
#endif

namespace hims {

using namespace std;

namespace {

int hexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

string jsonEscape(const string& value) {
  ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20U) {
          static constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4U) & 0x0fU] << kHex[ch & 0x0fU];
        } else {
          out << static_cast<char>(ch);
        }
        break;
    }
  }
  return out.str();
}

bool jsonObjectIsComplete(const string& body) {
  const auto begin = body.find_first_not_of(" \t\r\n");
  const auto end = body.find_last_not_of(" \t\r\n");
  if (begin == string::npos || body[begin] != '{' || body[end] != '}') {
    return false;
  }

  int objectDepth = 0;
  int arrayDepth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t index = begin; index <= end; ++index) {
    const unsigned char ch = static_cast<unsigned char>(body[index]);
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      } else if (ch < 0x20U) {
        return false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
    } else if (ch == '{') {
      ++objectDepth;
    } else if (ch == '}') {
      if (--objectDepth < 0) return false;
    } else if (ch == '[') {
      ++arrayDepth;
    } else if (ch == ']') {
      if (--arrayDepth < 0) return false;
    }
  }
  return !inString && !escaped && objectDepth == 0 && arrayDepth == 0;
}

optional<size_t> jsonMemberValuePosition(const string& body, const string& key) {
  if (!jsonObjectIsComplete(body)) {
    return nullopt;
  }

  int objectDepth = 0;
  int arrayDepth = 0;
  optional<size_t> valuePosition;
  for (size_t index = 0; index < body.size(); ++index) {
    const char ch = body[index];
    if (ch == '{') {
      ++objectDepth;
      continue;
    }
    if (ch == '}') {
      --objectDepth;
      continue;
    }
    if (ch == '[') {
      ++arrayDepth;
      continue;
    }
    if (ch == ']') {
      --arrayDepth;
      continue;
    }
    if (ch != '"') {
      continue;
    }

    const auto stringStart = index + 1;
    bool escaped = false;
    for (++index; index < body.size(); ++index) {
      if (escaped) {
        escaped = false;
      } else if (body[index] == '\\') {
        escaped = true;
      } else if (body[index] == '"') {
        break;
      }
    }
    if (index >= body.size()) return nullopt;

    size_t afterName = index + 1;
    while (afterName < body.size() && isspace(static_cast<unsigned char>(body[afterName]))) ++afterName;
    if (objectDepth != 1 || arrayDepth != 0 || afterName >= body.size() || body[afterName] != ':' ||
        body.substr(stringStart, index - stringStart) != key) {
      continue;
    }
    ++afterName;
    while (afterName < body.size() && isspace(static_cast<unsigned char>(body[afterName]))) ++afterName;
    if (valuePosition.has_value()) {
      return nullopt;
    }
    valuePosition = afterName;
  }
  return valuePosition;
}

optional<string> jsonString(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition || *valuePosition >= body.size() || body[*valuePosition] != '"') return nullopt;
  auto position = *valuePosition;
  string value;
  bool escaped = false;
  for (++position; position < body.size(); ++position) {
    const char ch = body[position];
    if (escaped) {
      switch (ch) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u':
          if (position + 4 < body.size()) {
            const auto hi = hexDigit(body[position + 1]);
            const auto h2 = hexDigit(body[position + 2]);
            const auto h3 = hexDigit(body[position + 3]);
            const auto lo = hexDigit(body[position + 4]);
            if (hi >= 0 && h2 >= 0 && h3 >= 0 && lo >= 0) {
              const auto codepoint = static_cast<unsigned>(hi << 12 | h2 << 8 | h3 << 4 | lo);
              if (codepoint <= 0xFFU) {
                value.push_back(static_cast<char>(codepoint));
                position += 4;
                escaped = false;
                continue;
              }
            }
          }
          value.push_back(ch);
          break;
        default: value.push_back(ch); break;
      }
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return value;
    } else {
      value.push_back(ch);
    }
  }
  return nullopt;
}

optional<int> jsonInt(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition) return nullopt;
  auto position = *valuePosition;
  while (position < body.size() && isspace(static_cast<unsigned char>(body[position]))) ++position;
  const auto begin = position;
  if (position < body.size() && (body[position] == '-' || body[position] == '+')) ++position;
  while (position < body.size() && isdigit(static_cast<unsigned char>(body[position]))) ++position;
  if (position == begin || (position == begin + 1 && (body[begin] == '-' || body[begin] == '+'))) return nullopt;
  try {
    const auto parsed = stoll(body.substr(begin, position - begin));
    if (parsed < numeric_limits<int>::min() || parsed > numeric_limits<int>::max()) return nullopt;
    return static_cast<int>(parsed);
  } catch (...) {
    return nullopt;
  }
}

optional<bool> jsonBool(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition) return nullopt;
  auto position = *valuePosition;
  while (position < body.size() && isspace(static_cast<unsigned char>(body[position]))) ++position;
  if (body.compare(position, 4, "true") == 0) return true;
  if (body.compare(position, 5, "false") == 0) return false;
  return nullopt;
}

optional<string> jsonArrayBody(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition || *valuePosition >= body.size() || body[*valuePosition] != '[') return nullopt;
  const auto begin = *valuePosition;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  for (auto index = begin; index < body.size(); ++index) {
    const char ch = body[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == '[') ++depth;
    else if (ch == ']' && --depth == 0) return body.substr(begin + 1, index - begin - 1);
  }
  return nullopt;
}

optional<string> jsonObjectBody(const string& body, const string& key) {
  const auto valuePosition = jsonMemberValuePosition(body, key);
  if (!valuePosition || *valuePosition >= body.size() || body[*valuePosition] != '{') return nullopt;
  const auto begin = *valuePosition;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  for (auto index = begin; index < body.size(); ++index) {
    const char ch = body[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == '{') ++depth;
    else if (ch == '}' && --depth == 0) return body.substr(begin, index - begin + 1);
  }
  return nullopt;
}

vector<string> jsonObjectArray(const string& body, const string& key) {
  vector<string> objects;
  const auto array = jsonArrayBody(body, key);
  if (!array) return objects;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  size_t begin = string::npos;
  for (size_t index = 0; index < array->size(); ++index) {
    const char ch = (*array)[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') inString = true;
    else if (ch == '{') {
      if (depth++ == 0) begin = index;
    } else if (ch == '}' && depth > 0 && --depth == 0 && begin != string::npos) {
      objects.push_back(array->substr(begin, index - begin + 1));
      begin = string::npos;
    }
  }
  return objects;
}

vector<string> jsonStringArray(const string& body, const string& key) {
  vector<string> values;
  const auto array = jsonArrayBody(body, key);
  if (!array) return values;
  size_t position = 0;
  while (position < array->size()) {
    const auto quote = array->find('"', position);
    if (quote == string::npos) break;
    string value;
    bool escaped = false;
    size_t index = quote + 1;
    for (; index < array->size(); ++index) {
      const char ch = (*array)[index];
      if (escaped) {
        value.push_back(ch);
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        values.push_back(value);
        ++index;
        break;
      } else {
        value.push_back(ch);
      }
    }
    position = index;
  }
  return values;
}

string hexToken(const array<unsigned char, 32>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

bool looksLikeSupportedHimsScanCode(const string& code) {
  const auto trimmed = trim(code);
  return !trimmed.empty() &&
         all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) { return isdigit(ch) != 0; });
}

}  // namespace

bool HimsScanConfig::paired() const {
  return !trim(token).empty() && token.size() >= 32;
}

bool loadHimsScanConfig(const filesystem::path& path, HimsScanConfig& config) {
  ifstream input(path);
  if (!input) return false;
  HimsScanConfig loaded;
  string line;
  while (getline(input, line)) {
    const auto separator = line.find('=');
    if (separator == string::npos) continue;
    const auto key = trim(line.substr(0, separator));
    const auto value = trim(line.substr(separator + 1));
    if (key == "device_id") loaded.deviceId = value;
    else if (key == "token") loaded.token = value;
    else if (key == "fallback_host" || key == "server_host") loaded.fallbackHost = value;
    else if (key == "fallback_port" || key == "server_port") {
      try {
        const auto port = stoul(value);
        if (port <= 65535) loaded.fallbackPort = static_cast<uint16_t>(port);
      } catch (...) {
      }
    }
  }
  config = move(loaded);
  return true;
}

bool saveHimsScanConfig(const filesystem::path& path, const HimsScanConfig& config) {
  error_code error;
  filesystem::create_directories(path.parent_path(), error);
  ofstream output(path, ios::trunc);
  if (!output) return false;
  output << "device_id=" << config.deviceId << '\n'
         << "fallback_host=" << config.fallbackHost << '\n'
         << "fallback_port=" << config.fallbackPort << '\n';
  return static_cast<bool>(output);
}

string generateHimsScanToken() {
  array<unsigned char, 32> bytes{};
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
    return hexToken(bytes);
  }
#endif
  random_device source;
  for (auto& byte : bytes) byte = static_cast<unsigned char>(source());
  return hexToken(bytes);
}

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
  if (*protocolVersion != 1) {
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
  parsed.resultAcks = jsonStringArray(body, "resultAcks");
  if (parsed.resultAcks.size() > 4) {
    error = "Too many result acknowledgements";
    return false;
  }

  for (const auto& object : jsonObjectArray(body, "events")) {
    const auto eventId = jsonString(object, "eventId");
    const auto type = jsonString(object, "type");
    const auto code = jsonString(object, "code");
    const auto value = jsonInt(object, "value");
    if (!eventId || trim(*eventId).empty() || !type || !code || trim(*code).empty() || !value ||
        eventId->size() > 96 || code->size() > 128) {
      error = "Invalid sync event";
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
    if (!lookupId || trim(*lookupId).empty() || !code || !looksLikeSupportedHimsScanCode(*code) ||
        lookupId->size() > 96 || code->size() > 128) {
      error = "Invalid sync lookup";
      return false;
    }
    parsed.hasLookup = true;
    parsed.lookup = {*lookupId, *code};
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
  out << "{\"protocolVersion\":1,\"requestId\":\"" << jsonEscape(response.requestId)
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
  if (!looksLikeSupportedHimsScanCode(code)) {
    result.status = "not_found";
    return result;
  }
  const auto* item = store.findByMachineCode(code);
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
  if (!looksLikeSupportedHimsScanCode(code)) {
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
                                               unordered_map<string, DeviceQuantityResult>& cache,
                                               deque<string>& order, size_t maxEntries) {
  const auto cached = cache.find(request.requestId);
  if (cached != cache.end()) {
    return cached->second;
  }

  const auto result = applyDeviceQuantity(store, request);
  cache[request.requestId] = result;
  order.push_back(request.requestId);
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

}  // namespace hims
