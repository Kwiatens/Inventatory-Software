// Inventatory - Hardware Inventory Management System
// Windows DNS-SD registration for automatic Inventatory Scan discovery.

#include "platform/MdnsService.h"

#include <array>
#include <chrono>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace inventatory {

#ifdef _WIN32
namespace {

bool isPrivateIpv4(IP4_ADDRESS address) {
  const auto hostOrder = ntohl(address);
  const auto first = (hostOrder >> 24U) & 0xffU;
  const auto second = (hostOrder >> 16U) & 0xffU;
  return first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
         (first == 192U && second == 168U) || (first == 169U && second == 254U);
}

}  // namespace

void WINAPI MdnsService::registrationComplete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE) {
  auto* service = static_cast<MdnsService*>(context);
  if (service == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(service->completionMutex_);
    service->completionStatus_ = status;
    service->completionReceived_ = true;
  }
  service->completionChanged_.notify_one();
}

bool MdnsService::waitForRegistrationCompletion(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(completionMutex_);
  if (!completionChanged_.wait_for(lock, timeout, [this] { return completionReceived_; })) return false;
  return completionStatus_ == ERROR_SUCCESS;
}
#endif

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
  IP4_ADDRESS ipv4Address = 0;
  bool haveIpv4Address = false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* resolved = nullptr;
  if (getaddrinfo(host.data(), nullptr, &hints, &resolved) == 0) {
    for (auto* address = resolved; address != nullptr; address = address->ai_next) {
      const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->ai_addr);
      const auto hostOrder = ntohl(ipv4->sin_addr.S_un.S_addr);
      if (hostOrder != 0U && (hostOrder >> 24U) != 127U && isPrivateIpv4(ipv4->sin_addr.S_un.S_addr)) {
        ipv4Address = ipv4->sin_addr.S_un.S_addr;
        haveIpv4Address = true;
        break;
      }
    }
    freeaddrinfo(resolved);
  }
  const wchar_t* keys[] = {L"protocol"};
  const wchar_t* values[] = {L"1"};
  instance_ = DnsServiceConstructInstance(L"Inventatory._inventatory._tcp.local", wideHost.c_str(),
                                          haveIpv4Address ? &ipv4Address : nullptr, nullptr, port, 0, 0,
                                          1, keys, values);
  // mDNS is a local-network discovery mechanism.  Do not publish a service
  // instance that has no private LAN address (for example on a public/VPN-only
  // host), even though the HTTP listener may still be reachable by an address
  // known to the user.
  if (instance_ == nullptr || !haveIpv4Address) {
    if (instance_ != nullptr) DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
    return false;
  }
  request_ = {};
  request_.Version = DNS_QUERY_REQUEST_VERSION1;
  request_.InterfaceIndex = 0;
  request_.pServiceInstance = instance_;
  request_.pRegisterCompletionCallback = &MdnsService::registrationComplete;
  request_.pQueryContext = this;
  request_.unicastEnabled = FALSE;
  {
    std::lock_guard<std::mutex> lock(completionMutex_);
    completionReceived_ = false;
    completionStatus_ = ERROR_SUCCESS;
  }
  const auto status = DnsServiceRegister(&request_, nullptr);
  if (status != ERROR_SUCCESS && status != DNS_REQUEST_PENDING) {
    DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
    return false;
  }
  if (status == DNS_REQUEST_PENDING && !waitForRegistrationCompletion(std::chrono::seconds(2))) {
    DnsServiceDeRegister(&request_, nullptr);
    waitForRegistrationCompletion(std::chrono::seconds(2));
    DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
    request_ = {};
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
  if (instance_ != nullptr) {
    {
      std::lock_guard<std::mutex> lock(completionMutex_);
      completionReceived_ = false;
      completionStatus_ = ERROR_SUCCESS;
    }
    const auto status = DnsServiceDeRegister(&request_, nullptr);
    if (status == DNS_REQUEST_PENDING) waitForRegistrationCompletion(std::chrono::seconds(2));
  }
  if (instance_ != nullptr) DnsServiceFreeInstance(instance_);
  instance_ = nullptr;
  request_ = {};
#endif
  running_ = false;
}

bool MdnsService::running() const {
  return running_;
}

}  // namespace inventatory
