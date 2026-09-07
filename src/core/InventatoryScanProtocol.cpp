// Inventatory - Hardware Inventory Management System
// Inventatory Scan R1 protocol types, validation, persistence, and stock mutation rules.

#include "core/InventatoryScanProtocol.h"
#include "core/AtomicFile.h"
#include "core/InventoryInternals.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_set>

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

namespace inventatory {

using namespace std;

namespace {

// The transport body is already bounded by the HTTP server, but a bounded
// nesting depth keeps malformed JSON from consuming unbounded parser work and
// makes the accepted wire shape explicit.
constexpr size_t kMaxJsonNestingDepth = 32;
constexpr size_t kMaxJsonBodyBytes = 64U * 1024U;
constexpr size_t kMaxScanConfigFileBytes = 8U * 1024U;
constexpr size_t kMaxScanConfigDeviceIdBytes = 128U;
constexpr size_t kMaxScanConfigHostBytes = 256U;

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

string hexBytes(const unsigned char* bytes, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  string out;
  out.reserve(size * 2U);
  for (size_t index = 0; index < size; ++index) {
    out.push_back(kHex[(bytes[index] >> 4U) & 0x0fU]);
    out.push_back(kHex[bytes[index] & 0x0fU]);
  }
  return out;
}

bool decodeToken(const string& token, array<unsigned char, 32>& bytes) {
  if (token.size() != bytes.size() * 2U) return false;
  for (size_t index = 0; index < bytes.size(); ++index) {
    const auto high = hexDigit(token[index * 2U]);
    const auto low = hexDigit(token[index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    bytes[index] = static_cast<unsigned char>((high << 4U) | low);
  }
  return true;
}

#ifdef _WIN32
bool hmacSha256(const unsigned char* key, size_t keySize, const string& input, array<unsigned char, 32>& output) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD objectSize = 0;
  DWORD hashSize = 0;
  ULONG ignored = 0;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                        &ignored, 0) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
                        &ignored, 0) != 0 || hashSize != output.size()) {
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  vector<unsigned char> object(objectSize);
  if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, const_cast<PUCHAR>(key),
                       static_cast<ULONG>(keySize), 0) != 0 ||
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                     static_cast<ULONG>(input.size()), 0) != 0 ||
      BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) != 0) {
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return true;
}
#endif

string transportMac(const string& token, const char* direction, const string& input) {
  array<unsigned char, 32> root{};
  array<unsigned char, 32> key{};
  array<unsigned char, 32> mac{};
  if (!decodeToken(token, root)) return {};
#ifdef _WIN32
  if (!hmacSha256(root.data(), root.size(), string("Inventatory Scan R1 transport v1 ") + direction, key) ||
      !hmacSha256(key.data(), key.size(), input, mac)) {
    return {};
  }
  return hexBytes(mac.data(), mac.size());
#else
  (void)direction;
  (void)input;
  return {};
#endif
}

bool jsonObjectIsComplete(const string& body) {
  if (body.size() > kMaxJsonBodyBytes) return false;
  const auto begin = body.find_first_not_of(" \t\r\n");
  const auto end = body.find_last_not_of(" \t\r\n");
  if (begin == string::npos || body[begin] != '{' || body[end] != '}') {
    return false;
  }

  vector<char> nesting;
  nesting.reserve(8);
  bool inString = false;
  bool escaped = false;
  for (size_t index = begin; index <= end; ++index) {
    const unsigned char ch = static_cast<unsigned char>(body[index]);
    if (inString) {
      if (escaped) {
        if (ch == 'u') {
          if (index + 4U > end || hexDigit(body[index + 1U]) < 0 || hexDigit(body[index + 2U]) < 0 ||
              hexDigit(body[index + 3U]) < 0 || hexDigit(body[index + 4U]) < 0) return false;
          index += 4U;
        } else if (ch != '"' && ch != '\\' && ch != '/' && ch != 'b' && ch != 'f' && ch != 'n' &&
                   ch != 'r' && ch != 't') {
          return false;
        }
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
      if (nesting.size() >= kMaxJsonNestingDepth) return false;
      nesting.push_back('{');
    } else if (ch == '}') {
      if (nesting.empty() || nesting.back() != '{') return false;
      nesting.pop_back();
    } else if (ch == '[') {
      if (nesting.size() >= kMaxJsonNestingDepth) return false;
      nesting.push_back('[');
    } else if (ch == ']') {
      if (nesting.empty() || nesting.back() != '[') return false;
      nesting.pop_back();
    }
  }
  return !inString && !escaped && nesting.empty();
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
  if (position < body.size() && body[position] != ',' && body[position] != '}' && body[position] != ']' &&
      isspace(static_cast<unsigned char>(body[position])) == 0) return nullopt;
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
  const auto validTerminator = [&](size_t end) {
    return end == body.size() || body[end] == ',' || body[end] == '}' || body[end] == ']' ||
           isspace(static_cast<unsigned char>(body[end])) != 0;
  };
  if (body.compare(position, 4, "true") == 0 && validTerminator(position + 4U)) return true;
  if (body.compare(position, 5, "false") == 0 && validTerminator(position + 5U)) return false;
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

optional<vector<string>> jsonObjectArray(const string& body, const string& key) {
  const auto array = jsonArrayBody(body, key);
  if (!array) return nullopt;
  vector<string> objects;
  size_t position = 0;
  const auto skipWhitespace = [&]() {
    while (position < array->size() && isspace(static_cast<unsigned char>((*array)[position])) != 0) ++position;
  };
  skipWhitespace();
  if (position == array->size()) return objects;
  while (position < array->size()) {
    if ((*array)[position] != '{') return nullopt;
    const size_t begin = position;
    vector<char> nesting;
    bool inString = false;
    bool escaped = false;
    for (; position < array->size(); ++position) {
      const char ch = (*array)[position];
      if (inString) {
        if (escaped) {
          escaped = false;
        } else if (ch == '\\') {
          escaped = true;
        } else if (ch == '"') {
          inString = false;
        }
        continue;
      }
      if (ch == '"') {
        inString = true;
      } else if (ch == '{' || ch == '[') {
        if (nesting.size() >= kMaxJsonNestingDepth) return nullopt;
        nesting.push_back(ch);
      } else if (ch == '}' || ch == ']') {
        if (nesting.empty() || (ch == '}' && nesting.back() != '{') ||
            (ch == ']' && nesting.back() != '[')) {
          return nullopt;
        }
        nesting.pop_back();
        if (nesting.empty()) {
          ++position;
          break;
        }
      }
    }
    if (inString || escaped || !nesting.empty() || position <= begin) return nullopt;
    objects.push_back(array->substr(begin, position - begin));
    skipWhitespace();
    if (position == array->size()) return objects;
    if ((*array)[position] != ',') return nullopt;
    ++position;
    skipWhitespace();
    if (position == array->size()) return nullopt;
  }
  return objects;
}

optional<vector<string>> jsonStringArray(const string& body, const string& key) {
  const auto array = jsonArrayBody(body, key);
  if (!array) return nullopt;
  vector<string> values;
  size_t position = 0;
  const auto skipWhitespace = [&]() {
    while (position < array->size() && isspace(static_cast<unsigned char>((*array)[position])) != 0) ++position;
  };
  skipWhitespace();
  if (position == array->size()) return values;
  while (position < array->size()) {
    if ((*array)[position] != '"') return nullopt;
    string value;
    bool escaped = false;
    bool closed = false;
    size_t index = position + 1;
    for (; index < array->size(); ++index) {
      const char ch = (*array)[index];
      if (escaped) {
        if (ch == 'u') {
          if (index + 4 >= array->size() || hexDigit((*array)[index + 1]) < 0 ||
              hexDigit((*array)[index + 2]) < 0 || hexDigit((*array)[index + 3]) < 0 ||
              hexDigit((*array)[index + 4]) < 0) return nullopt;
          value.push_back(ch);
          index += 4;
        } else if (ch != '"' && ch != '\\' && ch != '/' && ch != 'b' && ch != 'f' && ch != 'n' &&
                   ch != 'r' && ch != 't') {
          return nullopt;
        } else {
          value.push_back(ch);
        }
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        closed = true;
        ++index;
        break;
      } else {
        if (static_cast<unsigned char>(ch) < 0x20U) return nullopt;
        value.push_back(ch);
      }
    }
    if (!closed || escaped) return nullopt;
    values.push_back(move(value));
    position = index;
    skipWhitespace();
    if (position == array->size()) return values;
    if ((*array)[position] != ',') return nullopt;
    ++position;
    skipWhitespace();
    if (position == array->size()) return nullopt;
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

string generateInventatoryScanToken() {
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

string deviceRequestMac(const string& token, const string& method, const string& path, const string& deviceId,
                        uint64_t counter, const string& body) {
  return transportMac(token, "client-to-server",
                      "Inventatory Scan R1/v1\nrequest\n" + method + '\n' + path + '\n' + deviceId + '\n' +
                          to_string(counter) + '\n' + body);
}

string deviceResponseMac(const string& token, uint64_t counter, int status, const string& body) {
  return transportMac(token, "server-to-client",
                      "Inventatory Scan R1/v1\nresponse\n" + to_string(counter) + '\n' + to_string(status) + '\n' +
                          body);
}

string deviceTransportStateFingerprint(const string& token) {
  return transportMac(token, "replay-state", "Inventatory Scan R1/v1 replay state");
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

}  // namespace inventatory
