// HIMS - Hardware Inventory Management System
// Windows DNS-SD registration for automatic HIMS Scan discovery.

#include "platform/MdnsService.h"

#include <array>
#include <string>

#include <winsock2.h>

namespace hims {

MdnsService::~MdnsService() {
  stop();
}

bool MdnsService::start(std::uint16_t port) {
  stop();
#ifdef _WIN32
  std::array<char, 256> host{};
  if (gethostname(host.data(), static_cast<int>(host.size())) != 0) return false;
  std::wstring wideHost;
  for (const char ch : std::string(host.data())) wideHost.push_back(static_cast<unsigned char>(ch));
  wideHost += L".local";
  const wchar_t* keys[] = {L"protocol"};
  const wchar_t* values[] = {L"1"};
  instance_ = DnsServiceConstructInstance(L"HIMS._hims._tcp.local", wideHost.c_str(), nullptr, nullptr, port, 0, 0,
                                          1, keys, values);
  if (instance_ == nullptr) return false;
  request_ = {};
  request_.Version = DNS_QUERY_REQUEST_VERSION1;
  request_.InterfaceIndex = 0;
  request_.pServiceInstance = instance_;
  request_.unicastEnabled = FALSE;
  const auto status = DnsServiceRegister(&request_, nullptr);
  if (status != ERROR_SUCCESS && status != DNS_REQUEST_PENDING) {
    DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
    return false;
  }
  running_ = true;
  return true;
#else
  (void)port;
  return false;
#endif
}

void MdnsService::stop() {
#ifdef _WIN32
  if (running_ && instance_ != nullptr) DnsServiceDeRegister(&request_, nullptr);
  if (instance_ != nullptr) DnsServiceFreeInstance(instance_);
  instance_ = nullptr;
  request_ = {};
#endif
  running_ = false;
}

bool MdnsService::running() const {
  return running_;
}

}  // namespace hims
