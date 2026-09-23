// Inventatory - authenticated device server lifecycle and socket binding.

#define NOMINMAX

#include "platform/scanner/HttpServer.h"
#include "platform/system/Console.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kWorkerCount = 4;

}  // namespace

bool LocalHttpServer::start(uint16_t preferredPort, SyncCallback onSync) {
  stop();

#ifdef _WIN32
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    lastError_ = "Failed to initialize Winsock";
    return false;
  }
  networkStarted_ = true;
#endif

  {
    lock_guard<mutex> lock(callbackMutex_);
    onSync_ = move(onSync);
  }

  const unsigned int lastCandidate = min(65535U, static_cast<unsigned int>(preferredPort) + 19U);
  for (unsigned int candidate = preferredPort; candidate <= lastCandidate; ++candidate) {
    if (bindSocket(static_cast<uint16_t>(candidate))) {
      running_.store(true);
      acceptor_ = thread(&LocalHttpServer::acceptLoop, this);
      reader_ = thread(&LocalHttpServer::readerLoop, this);
      workers_.clear();
      workers_.reserve(kWorkerCount);
      for (size_t index = 0; index < kWorkerCount; ++index) {
        workers_.emplace_back(&LocalHttpServer::workerLoop, this);
      }
      return true;
    }
  }

  lastError_ = "Unable to bind any scanner port";
#ifdef _WIN32
  if (networkStarted_) {
    WSACleanup();
    networkStarted_ = false;
  }
#endif
  return false;
}

void LocalHttpServer::stop() {
  running_.store(false);

  NativeSocket listeningSocket = kInvalidSocket;
  {
    lock_guard<mutex> lock(socketMutex_);
    listeningSocket = listenSocket_;
    listenSocket_ = kInvalidSocket;
  }
  if (listeningSocket != kInvalidSocket) {
    shutdown(listeningSocket, kSocketShutdownBoth);
    closeSocket(listeningSocket);
  }

  if (acceptor_.joinable()) acceptor_.join();

  pendingClientChanged_.notify_all();
  if (reader_.joinable()) reader_.join();

  // The acceptor has stopped, so no new socket can be added.  Close every
  // queued socket before waking workers; only sockets already popped by a
  // worker are allowed to finish their bounded I/O/callback work during
  // shutdown.
  deque<ReadyClient> queuedClients;
  {
    lock_guard<mutex> lock(clientQueueMutex_);
    queuedClients.swap(clientQueue_);
  }
  for (const auto& client : queuedClients) closeSocket(client.socket);
  clientQueueChanged_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();

#ifdef _WIN32
  if (networkStarted_) {
    WSACleanup();
    networkStarted_ = false;
  }
#endif
}

bool LocalHttpServer::running() const {
  return running_.load();
}

uint16_t LocalHttpServer::port() const {
  lock_guard<mutex> lock(stateMutex_);
  return port_;
}

string LocalHttpServer::baseUrl() const {
  const auto addresses = this->addresses();
  const auto host = addresses.empty() ? string("127.0.0.1") : addresses.front();
  const auto servicePort = port();
  ostringstream out;
  out << "http://" << host << ':' << servicePort;
  return out.str();
}

vector<string> LocalHttpServer::addresses() const {
  lock_guard<mutex> lock(stateMutex_);
  if (!addresses_.empty()) {
    return addresses_;
  }
  return {"127.0.0.1"};
}

bool LocalHttpServer::bindSocket(uint16_t port) {
  NativeSocket socketHandle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socketHandle == kInvalidSocket) {
    return false;
  }

#ifdef _WIN32
  BOOL reuse = TRUE;
  setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
  int reuse = 1;
  setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  if (::bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    closeSocket(socketHandle);
    return false;
  }

  if (listen(socketHandle, SOMAXCONN) < 0) {
    closeSocket(socketHandle);
    return false;
  }

  {
    lock_guard<mutex> lock(socketMutex_);
    listenSocket_ = socketHandle;
  }
  {
    lock_guard<mutex> lock(stateMutex_);
    port_ = port;
    addresses_ = localAddresses();
  }
  return true;
}

}  // namespace inventatory
