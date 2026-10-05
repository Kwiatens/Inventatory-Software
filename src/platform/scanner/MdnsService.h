// Inventatory - Hardware Inventory Management System
// Platform DNS-SD registration for automatic Inventatory Scan discovery.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

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

  bool start(std::uint16_t port, const std::string& boundAddress);
  void stop();
  bool running() const;

 private:
#ifdef _WIN32
  struct RegistrationState;
  static void WINAPI registrationComplete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance);
  bool waitForRegistrationCompletion(std::chrono::milliseconds timeout);
  std::shared_ptr<RegistrationState> registrationState_;
#endif
  bool running_ = false;
#ifndef _WIN32
  // Avahi registration (private D-Bus connection plus entry group); defined in MdnsService.cpp.
  struct AvahiPublication;
  std::shared_ptr<AvahiPublication> publication_;
#endif
};

}  // namespace inventatory
