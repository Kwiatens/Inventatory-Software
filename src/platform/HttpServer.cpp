// HIMS - Hardware Inventory Management System
// Authenticated local device service used by the HIMS Scan R1 hardware.

#define NOMINMAX

#include "platform/HttpServer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

#include "core/Inventory.h"
#include "platform/Console.h"

namespace hims {

using namespace std;

namespace {

constexpr size_t kMaxHttpHeaderBytes = 8U * 1024U;
constexpr size_t kMaxHttpBodyBytes = 64U * 1024U;
constexpr DWORD kClientIoTimeoutMs = 5000U;

string jsonEscape(const string& value) {
  ostringstream out;
  for (char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
  return out.str();
}

string trimHttp(const string& value) {
  size_t begin = 0;
  while (begin < value.size() && isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  size_t end = value.size();
  while (end > begin && isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return value.substr(begin, end - begin);
}

string headerValue(const string& headers, const string& wantedName) {
  istringstream input(headers);
  string line;
  const auto wanted = toLower(wantedName);
  while (getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto separator = line.find(':');
    if (separator == string::npos) continue;
    if (toLower(trimHttp(line.substr(0, separator))) == wanted) {
      return trimHttp(line.substr(separator + 1));
    }
  }
  return {};
}

optional<size_t> contentLength(const string& headers) {
  const auto value = headerValue(headers, "Content-Length");
  if (value.empty()) {
    return 0U;
  }

  size_t length = 0;
  for (const unsigned char ch : value) {
    if (!isdigit(ch) || length > (numeric_limits<size_t>::max() - (ch - '0')) / 10U) {
      return nullopt;
    }
    length = length * 10U + (ch - '0');
  }
  return length;
}

bool tokensMatch(const string& expected, const string& supplied) {
  if (expected.empty() || expected.size() != supplied.size()) {
    return false;
  }

  unsigned char difference = 0;
  for (size_t index = 0; index < expected.size(); ++index) {
    difference |= static_cast<unsigned char>(expected[index]) ^ static_cast<unsigned char>(supplied[index]);
  }
  return difference == 0;
}

string httpStatusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 408: return "408 Request Timeout";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 426: return "426 Upgrade Required";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

}  // namespace

LocalHttpServer::~LocalHttpServer() {
  stop();
}

bool LocalHttpServer::start(uint16_t preferredPort, ScanCallback onScan, QuantityCallback onQuantity,
                            StatusCallback onStatus, DebugCallback onDebug, SyncCallback onSync) {
  stop();

  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    lastError_ = "Failed to initialize Winsock";
    return false;
  }
  winsockStarted_ = true;

  onScan_ = move(onScan);
  onDebug_ = move(onDebug);
  onQuantity_ = move(onQuantity);
  onStatus_ = move(onStatus);
  onSync_ = move(onSync);

  for (uint16_t candidate = preferredPort; candidate < static_cast<uint16_t>(preferredPort + 20); ++candidate) {
    if (bindSocket(candidate)) {
      running_.store(true);
      worker_ = thread(&LocalHttpServer::workerLoop, this);
      return true;
    }
  }

  lastError_ = "Unable to bind any scanner port";
  if (winsockStarted_) {
    WSACleanup();
    winsockStarted_ = false;
  }
  return false;
}

void LocalHttpServer::stop() {
  running_.store(false);

  if (listenSocket_ != INVALID_SOCKET) {
    shutdown(listenSocket_, SD_BOTH);
    closesocket(listenSocket_);
    listenSocket_ = INVALID_SOCKET;
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  if (winsockStarted_) {
    WSACleanup();
    winsockStarted_ = false;
  }
}

void LocalHttpServer::setRecentActivity(vector<ActivityEntry> activities) {
  lock_guard<mutex> lock(stateMutex_);
  recentActivities_ = move(activities);
}

void LocalHttpServer::setDeviceCredentials(string deviceId, string token) {
  lock_guard<mutex> lock(stateMutex_);
  pairedDeviceId_ = move(deviceId);
  deviceToken_ = move(token);
}

bool LocalHttpServer::running() const {
  return running_.load();
}

uint16_t LocalHttpServer::port() const {
  return port_;
}

string LocalHttpServer::baseUrl() const {
  const auto addresses = this->addresses();
  const auto host = addresses.empty() ? string("127.0.0.1") : addresses.front();
  ostringstream out;
  out << "http://" << host << ':' << port_;
  return out.str();
}

vector<string> LocalHttpServer::addresses() const {
  lock_guard<mutex> lock(stateMutex_);
  if (!addresses_.empty()) {
    return addresses_;
  }
  return {"127.0.0.1"};
}

string LocalHttpServer::lastScan() const {
  lock_guard<mutex> lock(stateMutex_);
  return lastScan_;
}

bool LocalHttpServer::bindSocket(uint16_t port) {
  SOCKET socketHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socketHandle == INVALID_SOCKET) {
    return false;
  }

  BOOL reuse = TRUE;
  setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  if (::bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
    closesocket(socketHandle);
    return false;
  }

  if (listen(socketHandle, SOMAXCONN) == SOCKET_ERROR) {
    closesocket(socketHandle);
    return false;
  }

  listenSocket_ = socketHandle;
  port_ = port;
  addresses_ = localAddresses();
  return true;
}

void LocalHttpServer::workerLoop() {
  while (running_.load()) {
    sockaddr_in clientAddress{};
    int clientSize = sizeof(clientAddress);
    SOCKET client = accept(listenSocket_, reinterpret_cast<sockaddr*>(&clientAddress), &clientSize);
    if (client == INVALID_SOCKET) {
      if (running_.load()) {
        continue;
      }
      break;
    }

    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kClientIoTimeoutMs),
               sizeof(kClientIoTimeoutMs));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&kClientIoTimeoutMs),
               sizeof(kClientIoTimeoutMs));

    string request;
    array<char, 4096> buffer{};
    size_t expectedSize = string::npos;
    const auto deadline = chrono::steady_clock::now() + chrono::milliseconds(kClientIoTimeoutMs);
    while (true) {
      if (chrono::steady_clock::now() >= deadline) break;
      const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
      if (received <= 0) break;
      request.append(buffer.data(), buffer.data() + received);
      const auto headerEnd = request.find("\r\n\r\n");
      if (headerEnd != string::npos && headerEnd > kMaxHttpHeaderBytes) break;
      if (headerEnd != string::npos && expectedSize == string::npos) {
        const auto bodySize = contentLength(request.substr(0, headerEnd));
        if (!bodySize || *bodySize > kMaxHttpBodyBytes) break;
        expectedSize = headerEnd + 4 + *bodySize;
      }
      if (expectedSize != string::npos && request.size() >= expectedSize) break;
      if (request.size() > kMaxHttpHeaderBytes + kMaxHttpBodyBytes) break;
    }

    serveConnection(client, move(request));
    closesocket(client);
  }
}

string LocalHttpServer::responseText(const string& status, const string& contentType, const string& body) const {
  ostringstream out;
  out << "HTTP/1.1 " << status << "\r\n";
  out << "Content-Type: " << contentType << "\r\n";
  out << "Content-Length: " << body.size() << "\r\n";
  out << "Connection: close\r\n";
  out << "Cache-Control: no-store\r\n\r\n";
  out << body;
  return out.str();
}

bool LocalHttpServer::serveConnection(SOCKET clientSocket, string requestText) {
  // Parse the first request line and route only the tiny local API surface.
  const auto headerEnd = requestText.find("\r\n\r\n");
  if (headerEnd == string::npos) {
    return false;
  }
  if (headerEnd > kMaxHttpHeaderBytes) {
    const auto response = responseText("413 Payload Too Large", "text/plain; charset=utf-8", "Request too large");
    send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
    return false;
  }

  const auto headers = requestText.substr(0, headerEnd);
  const auto body = requestText.substr(headerEnd + 4);
  const auto declaredBodySize = contentLength(headers);
  if (!declaredBodySize || *declaredBodySize > kMaxHttpBodyBytes || body.size() != *declaredBodySize) {
    const auto response = responseText("400 Bad Request", "text/plain; charset=utf-8", "Invalid request body");
    send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
    return false;
  }
  istringstream input(headers);
  string method;
  string target;
  string version;
  input >> method >> target >> version;
  (void)version;

  if (method == "POST" && target == "/api/v1/device/sync") {
    string expectedDevice;
    string expectedToken;
    {
      lock_guard<mutex> lock(stateMutex_);
      expectedDevice = pairedDeviceId_;
      expectedToken = deviceToken_;
    }
    const auto suppliedToken = headerValue(headers, "X-HIMS-Token");
    if (!tokensMatch(expectedToken, suppliedToken)) {
      const auto response = responseText("401 Unauthorized", "application/json; charset=utf-8",
                                         statusResultJson(false, "Unauthorized device"));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }

    DeviceSyncRequest request;
    string error;
    if (!parseDeviceSyncRequestJson(body, request, error)) {
      const auto status = error == "Unsupported protocol version" ? "426 Upgrade Required" : "400 Bad Request";
      const auto response = responseText(status, "application/json; charset=utf-8", statusResultJson(false, error));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }
    if (!expectedDevice.empty() && request.deviceId != expectedDevice) {
      const auto response = responseText("400 Bad Request", "application/json; charset=utf-8",
                                         statusResultJson(false, "Device identity does not match the pairing"));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }

    DeviceSyncResponse syncResponse;
    if (!onSync_ || !onSync_(request, syncResponse, error)) {
      if (error.empty()) error = "Device sync service unavailable";
      const auto response = responseText("503 Service Unavailable", "application/json; charset=utf-8",
                                         statusResultJson(false, error));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }
    const auto response = responseText("200 OK", "application/json; charset=utf-8",
                                       deviceSyncResponseJson(syncResponse));
    send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
    return true;
  }
  if (method == "POST" && target == "/api/device/scan") {
    string expectedDevice;
    string expectedToken;
    {
      lock_guard<mutex> lock(stateMutex_);
      expectedDevice = pairedDeviceId_;
      expectedToken = deviceToken_;
    }
    const auto suppliedToken = headerValue(headers, "X-HIMS-Token");
    if (!tokensMatch(expectedToken, suppliedToken)) {
      const auto response = responseText("401 Unauthorized", "application/json; charset=utf-8",
                                         scanResultJson(false, "Unauthorized device"));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }

    DeviceScanRequest request;
    string error;
    if (!parseScanRequestJson(body, request, error) || (!expectedDevice.empty() && request.deviceId != expectedDevice)) {
      if (error.empty()) error = "Device identity does not match the pairing";
      const auto response = responseText("400 Bad Request", "application/json; charset=utf-8",
                                         scanResultJson(false, error));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }

    bool seen = false;
    {
      lock_guard<mutex> lock(stateMutex_);
      seen = deviceScanRequestCache_.find(request.requestId) != deviceScanRequestCache_.end();
      if (!seen) {
        deviceScanRequestCache_.insert(request.requestId);
        deviceScanRequestOrder_.push_back(request.requestId);
        while (deviceScanRequestOrder_.size() > 64U) {
          deviceScanRequestCache_.erase(deviceScanRequestOrder_.front());
          deviceScanRequestOrder_.pop_front();
        }
        lastScan_ = request.code;
      }
    }

    if (!seen && onScan_) {
      onScan_(request);
    }

    const auto response = responseText("200 OK", "application/json; charset=utf-8", scanResultJson(true));
    send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
    return true;
  }

  if (method == "POST" && target == "/api/device/debug") {
    string expectedDevice;
    string expectedToken;
    {
      lock_guard<mutex> lock(stateMutex_);
      expectedDevice = pairedDeviceId_;
      expectedToken = deviceToken_;
    }
    const auto suppliedToken = headerValue(headers, "X-HIMS-Token");
    if (!tokensMatch(expectedToken, suppliedToken)) {
      const auto response = responseText("401 Unauthorized", "application/json; charset=utf-8",
                                         debugResultJson(false, "Unauthorized device"));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }

    DeviceDebugReport report;
    string error;
    if (!parseDebugReportJson(body, report, error) || (!expectedDevice.empty() && report.deviceId != expectedDevice)) {
      if (error.empty()) error = "Device identity does not match the pairing";
      const auto response = responseText("400 Bad Request", "application/json; charset=utf-8",
                                         debugResultJson(false, error));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }

    const auto response = responseText("200 OK", "application/json; charset=utf-8", debugResultJson(true));
    send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
    if (onDebug_) {
      onDebug_(report);
    }
    return true;
  }

  if (method == "POST" && (target == "/api/device/quantity" || target == "/api/device/status")) {
    string expectedDevice;
    string expectedToken;
    {
      lock_guard<mutex> lock(stateMutex_);
      expectedDevice = pairedDeviceId_;
      expectedToken = deviceToken_;
    }
    const auto suppliedToken = headerValue(headers, "X-HIMS-Token");
    if (!tokensMatch(expectedToken, suppliedToken)) {
      const auto response = responseText("401 Unauthorized", "application/json; charset=utf-8",
                                         statusResultJson(false, "Unauthorized device"));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }

    if (target == "/api/device/quantity") {
      DeviceQuantityRequest request;
      string error;
      if (!parseQuantityRequestJson(body, request, error) ||
          (!expectedDevice.empty() && request.deviceId != expectedDevice)) {
        if (error.empty()) error = "Device identity does not match the pairing";
        const auto response = responseText("400 Bad Request", "application/json; charset=utf-8",
                                           statusResultJson(false, error));
        send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
        return false;
      }
      DeviceQuantityResult result;
      if (onQuantity_) {
        result = onQuantity_(request);
      } else {
        result.httpStatus = 503;
        result.error = "Quantity service unavailable";
      }
      const auto response = responseText(httpStatusText(result.httpStatus), "application/json; charset=utf-8",
                                         quantityResultJson(result));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return result.ok;
    }

    DeviceStatusReport report;
    string error;
    if (!parseStatusReportJson(body, report, error) || (!expectedDevice.empty() && report.deviceId != expectedDevice)) {
      if (error.empty()) error = "Device identity does not match the pairing";
      const auto response = responseText("400 Bad Request", "application/json; charset=utf-8",
                                         statusResultJson(false, error));
      send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
      return false;
    }
    if (onStatus_) onStatus_(report);
    const auto response = responseText("200 OK", "application/json; charset=utf-8", statusResultJson(true));
    send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
    return true;
  }

  const auto response = responseText("404 Not Found", "text/plain; charset=utf-8", "Not found");
  send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
  return false;
}

}  // namespace hims
