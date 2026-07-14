// Inventatory - Hardware Inventory Management System
// Authenticated local device service used by the Inventatory Scan R1 hardware.

#pragma once

#include <atomic>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include "core/Inventory.h"
#include "core/InventatoryScanProtocol.h"

namespace inventatory {

namespace filesystem = std::filesystem;
using std::atomic;
using std::filesystem::path;
using std::function;
using std::mutex;
using std::string;
using std::thread;
using std::uint16_t;
using std::vector;
using std::deque;
using std::unordered_set;

class LocalHttpServer {
 public:
  using ScanCallback = function<void(const DeviceScanRequest&)>;
  using DebugCallback = function<void(const DeviceDebugReport&)>;
  using QuantityCallback = function<DeviceQuantityResult(const DeviceQuantityRequest&)>;
  using StatusCallback = function<void(const DeviceStatusReport&)>;
  using SyncCallback = function<bool(const DeviceSyncRequest&, DeviceSyncResponse&, string&)>;

  LocalHttpServer() = default;
  ~LocalHttpServer();

  LocalHttpServer(const LocalHttpServer&) = delete;
  LocalHttpServer& operator=(const LocalHttpServer&) = delete;

  bool start(uint16_t preferredPort, ScanCallback onScan, QuantityCallback onQuantity = {},
             StatusCallback onStatus = {}, DebugCallback onDebug = {}, SyncCallback onSync = {});
  void stop();
  void setRecentActivity(vector<ActivityEntry> activities);
  void setDeviceCredentials(string deviceId, string token);

  bool running() const;
  uint16_t port() const;
  string baseUrl() const;
  vector<string> addresses() const;
  string lastScan() const;

 private:
  void workerLoop();
  bool serveConnection(SOCKET clientSocket, string requestText);
  string responseText(const string& status, const string& contentType, const string& body) const;
  bool bindSocket(uint16_t port);

  atomic<bool> running_{false};
  bool winsockStarted_ = false;
  thread worker_;
  ScanCallback onScan_;
  DebugCallback onDebug_;
  QuantityCallback onQuantity_;
  StatusCallback onStatus_;
  SyncCallback onSync_;
  mutable mutex stateMutex_;
  deque<string> deviceScanRequestOrder_;
  unordered_set<string> deviceScanRequestCache_;
  uint16_t port_ = 0;
  string lastScan_;
  string lastError_;
  vector<string> addresses_;
  vector<ActivityEntry> recentActivities_;
  string pairedDeviceId_;
  string deviceToken_;
  SOCKET listenSocket_ = INVALID_SOCKET;
};

}  // namespace inventatory
