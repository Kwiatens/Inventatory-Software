// Inventatory - Hardware Inventory Management System
// Windows DNS-SD registration for automatic Inventatory Scan discovery.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windns.h>
#endif

namespace inventatory {

class MdnsService {
 public:
  MdnsService() = default;
  ~MdnsService();
  MdnsService(const MdnsService&) = delete;
  MdnsService& operator=(const MdnsService&) = delete;

  bool start(std::uint16_t port);
  void stop();
  bool running() const;

 private:
#ifdef _WIN32
  static void WINAPI registrationComplete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance);
  bool waitForRegistrationCompletion(std::chrono::milliseconds timeout);
  DNS_SERVICE_REGISTER_REQUEST request_{};
  PDNS_SERVICE_INSTANCE instance_ = nullptr;
  mutable std::mutex completionMutex_;
  std::condition_variable completionChanged_;
  bool completionReceived_ = false;
  DWORD completionStatus_ = ERROR_SUCCESS;
#endif
  bool running_ = false;
};

}  // namespace inventatory
