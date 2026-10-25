// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Authenticated local device service for the Inventatory Scanner R1.

#pragma once

#include <atomic>
#include <chrono>
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
#include <unordered_map>
#include <unordered_set>

#include "platform/scanner/HttpServerProtocolInternal.h"
#include "platform/scanner/SocketPlatform.h"

#include "core/inventory/Inventory.h"
#include "core/scanner/InventatoryScanProtocol.h"

namespace inventatory {

class ScanFirmwareCourier;

namespace filesystem = std::filesystem;
using std::atomic;
using std::condition_variable;
using std::filesystem::path;
using std::function;
using std::mutex;
using std::string;
using std::thread;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::vector;
using std::deque;
using std::unordered_map;
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
  // Must be set while the server is stopped. The courier must outlive the server.
  void setFirmwareCourier(ScanFirmwareCourier* courier);

  bool running() const;
  uint16_t port() const;
  vector<string> addresses() const;

 private:
  struct ReadyClient {
    NativeSocket socket = kInvalidSocket;
    string request;
  };
  struct QueuedClient {
    NativeSocket socket = kInvalidSocket;
    uint32_t address = 0;
  };

  void workerLoop();
  void readerLoop();
  void acceptLoop();
  bool serveConnection(NativeSocket clientSocket, string requestText);
  bool serveFirmwareRequest(NativeSocket clientSocket, const string& target,
                            const http_server_detail::HttpHeaderMap& headerValues, const string& body);
  string responseText(const string& status, const string& contentType, const string& body) const;
  string authenticatedResponseText(int status, std::uint64_t counter, const string& token, const string& body,
                                   const string& contentType = "application/json; charset=utf-8") const;
  bool reserveReplayCounter(std::uint64_t counter, std::uint64_t credentialEpoch);
  void releaseReplayCounter(std::uint64_t counter, std::uint64_t credentialEpoch);
  bool advanceReplayCounter(std::uint64_t counter, std::uint64_t credentialEpoch);
  bool bindSocket(uint16_t port, const string& ipv4Address);
  bool sendAll(NativeSocket clientSocket, const string& response) const;

  atomic<bool> running_{false};
  bool networkStarted_ = false;
  thread acceptor_;
  thread reader_;
  vector<thread> workers_;
  mutable mutex socketMutex_;
  mutable mutex pendingClientMutex_;
  condition_variable pendingClientChanged_;
  deque<QueuedClient> pendingClientQueue_;
  size_t pendingClientCount_ = 0;
  unordered_map<uint32_t, size_t> pendingClientsPerAddress_;
  mutable mutex clientQueueMutex_;
  condition_variable clientQueueChanged_;
  deque<ReadyClient> clientQueue_;
  SyncCallback onSync_;
  ScanFirmwareCourier* firmwareCourier_ = nullptr;
  mutable mutex callbackMutex_;
  mutable mutex stateMutex_;
  // The callback runs synchronously, since the response carries its result. Calls are serialized
  // outside the state mutexes, so the callback may re-enter the server or block on its queues.
  mutex callbackSerialMutex_;
  // Rotation is serialized with authentication and replay state. The epoch invalidates a callback's
  // replay commit if it finishes after rotation, without waiting for that callback.
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
  NativeSocket listenSocket_ = kInvalidSocket;
};

}  // namespace inventatory
