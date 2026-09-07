// Inventatory - Hardware Inventory Management System
// Authenticated local device service used by the Inventatory Scan R1 hardware.

#pragma once

#include <atomic>
#include <condition_variable>
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
using std::condition_variable;
using std::filesystem::path;
using std::function;
using std::mutex;
using std::string;
using std::thread;
using std::uint16_t;
using std::uint64_t;
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
  void acceptLoop();
  bool serveConnection(SOCKET clientSocket, string requestText);
  string responseText(const string& status, const string& contentType, const string& body) const;
  string authenticatedResponseText(int status, std::uint64_t counter, const string& token, const string& body) const;
  bool reserveReplayCounter(std::uint64_t counter, std::uint64_t credentialEpoch);
  void releaseReplayCounter(std::uint64_t counter, std::uint64_t credentialEpoch);
  bool advanceReplayCounter(std::uint64_t counter, std::uint64_t credentialEpoch);
  bool bindSocket(uint16_t port);
  bool sendAll(SOCKET clientSocket, const string& response) const;

  atomic<bool> running_{false};
  bool winsockStarted_ = false;
  thread acceptor_;
  vector<thread> workers_;
  mutable mutex socketMutex_;
  mutable mutex clientQueueMutex_;
  condition_variable clientQueueChanged_;
  deque<SOCKET> clientQueue_;
  SyncCallback onSync_;
  mutable mutex callbackMutex_;
  mutable mutex stateMutex_;
  // The callback is synchronous because the authenticated response contains
  // its result.  Serialize callback execution without holding a server state
  // mutex, so application work can safely call back into the server or block
  // on its own persistence/printing queues.
  mutex callbackSerialMutex_;
  // Credential rotation is serialized with the complete authenticated request
  // lifecycle. A request that has already captured the old credentials either
  // finishes before rotation, or is rejected before it can reserve, invoke the
  // application callback, or advance replay state.
  mutable mutex credentialOperationMutex_;
  mutable mutex replayMutex_;
  uint16_t port_ = 0;
  string lastError_;
  vector<string> addresses_;
  string pairedDeviceId_;
  string deviceToken_;
  path replayStatePath_;
  string replayStateFingerprint_;
  bool replayStateValid_ = true;
  struct ReplayReservation {
    std::uint64_t counter = 0;
    std::uint64_t credentialEpoch = 0;
  };
  std::optional<ReplayReservation> replayCounterInFlight_;
  std::uint64_t lastAcceptedCounter_ = 0;
  std::uint64_t credentialEpoch_ = 0;
  SOCKET listenSocket_ = INVALID_SOCKET;
};

}  // namespace inventatory
