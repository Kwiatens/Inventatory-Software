// HIMS - Hardware Inventory Management System
// Windows DNS-SD registration for automatic HIMS Scan discovery.

#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windns.h>
#endif

namespace hims {

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
  DNS_SERVICE_REGISTER_REQUEST request_{};
  PDNS_SERVICE_INSTANCE instance_ = nullptr;
#endif
  bool running_ = false;
};

}  // namespace hims
