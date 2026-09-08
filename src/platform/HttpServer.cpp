// Inventatory - Hardware Inventory Management System
// Authenticated local device service used by the Inventatory Scan R1 hardware.

#define NOMINMAX

#include "platform/HttpServer.h"
#include "platform/HttpServerInternal.h"
#include "platform/HttpServerProtocolInternal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

#include "core/Inventory.h"
#include "platform/Console.h"

namespace inventatory {

using namespace std;
using namespace http_server_detail;

namespace {

constexpr DWORD kClientIoTimeoutMs = 2000U;
constexpr size_t kWorkerCount = 4;
constexpr size_t kMaxQueuedClients = 16;
constexpr DWORD kReaderSelectIntervalMs = 100U;

struct PendingClient {
  SOCKET socket = INVALID_SOCKET;
  string request;
  size_t expectedSize = string::npos;
  chrono::steady_clock::time_point deadline;
};


}  // namespace

LocalHttpServer::~LocalHttpServer() {
  stop();
}

void LocalHttpServer::setDeviceCredentials(string deviceId, string token, path replayStatePath) {
  // Rotation takes the credential operation lock first and only opportunistically
  // takes callback serialization. If a callback is already running, waiting
  // here could deadlock the application thread when that callback is waiting
  // for the UI to process its request. In that case the epoch changes under
  // replayMutex_; the stale callback can finish, but its replay commit and
  // reservation release are rejected by the epoch check.
  lock_guard<mutex> operationLock(credentialOperationMutex_);
  unique_lock<mutex> callbackLock(callbackSerialMutex_, try_to_lock);
  const auto fingerprint = deviceTransportStateFingerprint(token);
  bool deviceChanged = false;
  {
    lock_guard<mutex> lock(stateMutex_);
    deviceChanged = !pairedDeviceId_.empty() && pairedDeviceId_ != deviceId;
    pairedDeviceId_ = move(deviceId);
    deviceToken_ = move(token);
  }
  lock_guard<mutex> lock(replayMutex_);
  ++credentialEpoch_;
  const bool pairingChanged = deviceChanged ||
                              (!replayStateFingerprint_.empty() && replayStateFingerprint_ != fingerprint);
  replayStatePath_ = move(replayStatePath);
  replayStateFingerprint_ = fingerprint;
  replayCounterInFlight_.reset();
  if (pairingChanged) {
    // A deliberate in-process token rotation starts a fresh replay sequence.
    // The next accepted request rewrites the state file with the new fingerprint.
    replayStateValid_ = true;
    lastAcceptedCounter_ = 0;
  } else {
    replayStateValid_ = loadReplayState(replayStatePath_, replayStateFingerprint_, lastAcceptedCounter_);
    if (!replayStateValid_) lastAcceptedCounter_ = 0;
  }
}

void LocalHttpServer::acceptLoop() {
  while (running_.load()) {
    SOCKET listeningSocket = INVALID_SOCKET;
    {
      lock_guard<mutex> lock(socketMutex_);
      listeningSocket = listenSocket_;
    }
    if (listeningSocket == INVALID_SOCKET) break;
    sockaddr_in clientAddress{};
    int clientSize = sizeof(clientAddress);
    SOCKET client = accept(listeningSocket, reinterpret_cast<sockaddr*>(&clientAddress), &clientSize);
    if (client == INVALID_SOCKET) {
      if (running_.load()) continue;
      break;
    }
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kClientIoTimeoutMs),
               sizeof(kClientIoTimeoutMs));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&kClientIoTimeoutMs),
               sizeof(kClientIoTimeoutMs));
    u_long nonBlocking = 1;
    if (ioctlsocket(client, FIONBIO, &nonBlocking) != 0) {
      closesocket(client);
      continue;
    }

    bool queued = false;
    {
      lock_guard<mutex> lock(pendingClientMutex_);
      if (running_.load() && pendingClientCount_ < kMaxQueuedClients) {
        pendingClientQueue_.push_back(client);
        ++pendingClientCount_;
        queued = true;
      }
    }
    if (queued) {
      pendingClientChanged_.notify_one();
    } else {
      closesocket(client);
    }
  }
}

void LocalHttpServer::readerLoop() {
  vector<PendingClient> pending;
  pending.reserve(kMaxQueuedClients);

  const auto releasePendingSlot = [this] {
    lock_guard<mutex> lock(pendingClientMutex_);
    if (pendingClientCount_ > 0) --pendingClientCount_;
  };
  const auto closePendingClient = [&releasePendingSlot](PendingClient& client) {
    if (client.socket != INVALID_SOCKET) {
      closesocket(client.socket);
      client.socket = INVALID_SOCKET;
      releasePendingSlot();
    }
  };

  while (true) {
    {
      unique_lock<mutex> lock(pendingClientMutex_);
      if (pending.empty() && pendingClientQueue_.empty() && running_.load()) {
        pendingClientChanged_.wait(lock, [this] { return !running_.load() || !pendingClientQueue_.empty(); });
      }
      while (!pendingClientQueue_.empty()) {
        pending.push_back({pendingClientQueue_.front(), {}, string::npos,
                           chrono::steady_clock::now() + chrono::milliseconds(kClientIoTimeoutMs)});
        pendingClientQueue_.pop_front();
      }
    }

    if (!running_.load()) {
      for (auto& client : pending) closePendingClient(client);
      while (true) {
        SOCKET client = INVALID_SOCKET;
        {
          lock_guard<mutex> lock(pendingClientMutex_);
          if (pendingClientQueue_.empty()) break;
          client = pendingClientQueue_.front();
          pendingClientQueue_.pop_front();
        }
        PendingClient queued{client};
        closePendingClient(queued);
      }
      return;
    }
    if (pending.empty()) continue;

    fd_set readable;
    FD_ZERO(&readable);
    auto earliestDeadline = chrono::steady_clock::now() + chrono::milliseconds(kReaderSelectIntervalMs);
    for (const auto& client : pending) {
      FD_SET(client.socket, &readable);
      earliestDeadline = min(earliestDeadline, client.deadline);
    }
    const auto now = chrono::steady_clock::now();
    const auto waitDuration = max(chrono::milliseconds(0),
                                  chrono::duration_cast<chrono::milliseconds>(earliestDeadline - now));
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(waitDuration.count() / 1000);
    timeout.tv_usec = static_cast<long>((waitDuration.count() % 1000) * 1000);
    const int selected = select(0, &readable, nullptr, nullptr, &timeout);
    if (selected == SOCKET_ERROR) {
      for (auto& client : pending) closePendingClient(client);
      pending.clear();
      continue;
    }

    for (size_t index = 0; index < pending.size();) {
      auto& client = pending[index];
      bool ready = false;
      bool close = chrono::steady_clock::now() >= client.deadline;
      if (!close && selected > 0 && FD_ISSET(client.socket, &readable)) {
        array<char, 4096> buffer{};
        const int received = recv(client.socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received == 0) {
          close = true;
        } else if (received == SOCKET_ERROR) {
          const auto error = WSAGetLastError();
          close = error != WSAEWOULDBLOCK && error != WSAEINPROGRESS;
        } else {
          client.request.append(buffer.data(), buffer.data() + received);
          if (client.request.size() > kMaxHttpHeaderBytes + kMaxHttpBodyBytes) {
            ready = true;
          } else {
            const auto headerEnd = client.request.find("\r\n\r\n");
            if (headerEnd != string::npos) {
              if (headerEnd > kMaxHttpHeaderBytes) {
                ready = true;
              } else {
                string method;
                string target;
                string version;
                HttpHeaderMap headers;
                size_t bodySize = 0;
                if (!parseHttpHeaders(client.request.substr(0, headerEnd), method, target, version, headers) ||
                    !parseContentLength(headers, bodySize)) {
                  ready = true;
                } else if (bodySize > kMaxHttpBodyBytes ||
                           headers.find("transfer-encoding") != headers.end()) {
                  ready = true;
                } else {
                  client.expectedSize = headerEnd + 4 + bodySize;
                  ready = client.request.size() >= client.expectedSize;
                }
              }
            }
          }
        }
      }

      if (close) {
        closePendingClient(client);
      } else if (ready) {
        u_long blocking = 0;
        if (ioctlsocket(client.socket, FIONBIO, &blocking) != 0) {
          closePendingClient(client);
        } else {
          ReadyClient completed{client.socket, move(client.request)};
          client.socket = INVALID_SOCKET;
          releasePendingSlot();
          bool queued = false;
          {
            lock_guard<mutex> lock(clientQueueMutex_);
            if (running_.load() && clientQueue_.size() < kMaxQueuedClients) {
              clientQueue_.push_back(move(completed));
              queued = true;
            }
          }
          if (queued) {
            clientQueueChanged_.notify_one();
          } else {
            closesocket(completed.socket);
          }
        }
      }

      if (client.socket == INVALID_SOCKET) {
        pending[index] = move(pending.back());
        pending.pop_back();
      } else {
        ++index;
      }
    }
  }
}

void LocalHttpServer::workerLoop() {
  while (true) {
    ReadyClient client;
    {
      unique_lock<mutex> lock(clientQueueMutex_);
      clientQueueChanged_.wait(lock, [this] { return !running_.load() || !clientQueue_.empty(); });
      if (clientQueue_.empty()) {
        if (!running_.load()) break;
        continue;
      }
      client = move(clientQueue_.front());
      clientQueue_.pop_front();
    }
    serveConnection(client.socket, move(client.request));
    closesocket(client.socket);
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

string LocalHttpServer::authenticatedResponseText(int status, uint64_t counter, const string& token,
                                                  const string& body) const {
  ostringstream out;
  out << "HTTP/1.1 " << httpStatusText(status) << "\r\n";
  out << "Content-Type: application/json; charset=utf-8\r\n";
  out << "Content-Length: " << body.size() << "\r\n";
  out << "Connection: close\r\nCache-Control: no-store\r\n";
  out << "X-Inventatory-Protocol: " << kInventatoryScanTransportProtocolVersion << "\r\n";
  out << "X-Inventatory-Counter: " << counter << "\r\n";
  out << "X-Inventatory-Mac: " << deviceResponseMac(token, counter, status, body) << "\r\n\r\n";
  out << body;
  return out.str();
}

bool LocalHttpServer::reserveReplayCounter(uint64_t counter, uint64_t credentialEpoch) {
  lock_guard<mutex> lock(replayMutex_);
  if (credentialEpoch_ != credentialEpoch || !replayStateValid_ || replayStateFingerprint_.empty() ||
      replayCounterInFlight_.has_value() || counter <= lastAcceptedCounter_) {
    return false;
  }
  replayCounterInFlight_ = ReplayReservation{counter, credentialEpoch};
  return true;
}

void LocalHttpServer::releaseReplayCounter(uint64_t counter, uint64_t credentialEpoch) {
  lock_guard<mutex> lock(replayMutex_);
  if (replayCounterInFlight_.has_value() && replayCounterInFlight_->counter == counter &&
      replayCounterInFlight_->credentialEpoch == credentialEpoch) {
    replayCounterInFlight_.reset();
  }
}

bool LocalHttpServer::advanceReplayCounter(uint64_t counter, uint64_t credentialEpoch) {
  lock_guard<mutex> lock(replayMutex_);
  if (credentialEpoch_ != credentialEpoch || !replayStateValid_ || replayStateFingerprint_.empty() ||
      !replayCounterInFlight_ || replayCounterInFlight_->counter != counter ||
      replayCounterInFlight_->credentialEpoch != credentialEpoch || counter <= lastAcceptedCounter_) {
    return false;
  }
  if (!saveReplayState(replayStatePath_, replayStateFingerprint_, counter)) {
    replayStateValid_ = false;
    replayCounterInFlight_.reset();
    return false;
  }
  lastAcceptedCounter_ = counter;
  replayCounterInFlight_.reset();
  return true;
}

bool LocalHttpServer::sendAll(SOCKET clientSocket, const string& response) const {
  size_t sent = 0;
  while (sent < response.size()) {
    const auto remaining = response.size() - sent;
    const int chunk = static_cast<int>(min(remaining, static_cast<size_t>(numeric_limits<int>::max())));
    const int written = send(clientSocket, response.data() + sent, chunk, 0);
    if (written <= 0) return false;
    sent += static_cast<size_t>(written);
  }
  return true;
}


}  // namespace inventatory
