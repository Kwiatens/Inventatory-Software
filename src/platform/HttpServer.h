// Inventatory - Hardware Inventory Management System
// Authenticated local device service used by the Inventatory Scan R1 hardware.

#pragma once

#include <atomic>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
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
  using SyncCallback = function<bool(const DeviceSyncRequest&, DeviceSyncResponse&, string&)>;

  LocalHttpServer() = default;
  ~LocalHttpServer();

  LocalHttpServer(const LocalHttpServer&) = delete;
  LocalHttpServer& operator=(const LocalHttpServer&) = delete;

  bool start(uint16_t preferredPort, SyncCallback onSync);
  void stop();
  void setDeviceCredentials(string deviceId, string token, path replayStatePath = {});

  bool running() const;
  uint16_t port() const;
  string baseUrl() const;
  vector<string> addresses() const;

 private:
  void workerLoop();
  bool serveConnection(SOCKET clientSocket, string requestText);
  string responseText(const string& status, const string& contentType, const string& body) const;
  string authenticatedResponseText(int status, std::uint64_t counter, const string& token, const string& body) const;
  bool reserveReplayCounter(std::uint64_t counter);
  void releaseReplayCounter(std::uint64_t counter);
  bool advanceReplayCounter(std::uint64_t counter);
  bool bindSocket(uint16_t port);

  atomic<bool> running_{false};
  bool winsockStarted_ = false;
  vector<thread> workers_;
  SyncCallback onSync_;
  mutable mutex stateMutex_;
  mutex applicationMutex_;
  mutable mutex replayMutex_;
  uint16_t port_ = 0;
  string lastError_;
  vector<string> addresses_;
  string pairedDeviceId_;
  string deviceToken_;
  path replayStatePath_;
  string replayStateFingerprint_;
  bool replayStateValid_ = true;
  std::optional<std::uint64_t> replayCounterInFlight_;
  std::uint64_t lastAcceptedCounter_ = 0;
  SOCKET listenSocket_ = INVALID_SOCKET;
};

}  // namespace inventatory
