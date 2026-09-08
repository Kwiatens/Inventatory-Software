// Inventatory - Hardware Inventory Management System
// Windows DNS-SD registration for automatic Inventatory Scan discovery.

#include "platform/scanner/MdnsService.h"

#include <array>
#include <chrono>
#include <memory>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace inventatory {

#ifdef _WIN32
struct MdnsService::RegistrationState {
  struct CallbackContext {
    std::shared_ptr<RegistrationState> state;
    // The DNS API owns the callback timing.  Keep this context alive until
    // that callback runs even if MdnsService has already been destroyed.
    std::shared_ptr<CallbackContext> self;
    bool deregistration = false;
  };

  ~RegistrationState() {
    if (instance != nullptr) DnsServiceFreeInstance(instance);
  }

  DNS_SERVICE_REGISTER_REQUEST registerRequest{};
  DNS_SERVICE_REGISTER_REQUEST deregisterRequest{};
  DNS_SERVICE_CANCEL registrationCancel{};
  PDNS_SERVICE_INSTANCE instance = nullptr;
  std::mutex completionMutex;
  std::condition_variable completionChanged;
  bool registrationComplete = false;
  DWORD registrationStatus = ERROR_IO_PENDING;
  bool deregistrationComplete = false;
  DWORD deregistrationStatus = ERROR_IO_PENDING;
};

namespace {

bool isPrivateIpv4(IP4_ADDRESS address) {
  const auto hostOrder = ntohl(address);
  const auto first = (hostOrder >> 24U) & 0xffU;
  const auto second = (hostOrder >> 16U) & 0xffU;
  return first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
         (first == 192U && second == 168U) || (first == 169U && second == 254U);
}

}  // namespace

void WINAPI MdnsService::registrationComplete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
  auto* callbackContext = static_cast<RegistrationState::CallbackContext*>(context);
  if (callbackContext == nullptr) return;
  // Copy the self-retaining reference before breaking the cycle.  The state
  // remains alive for the complete callback even when the owning service has
  // timed out or been destroyed.
  const auto keepAlive = callbackContext->self;
  const auto state = callbackContext->state;
  callbackContext->self.reset();
  if (state == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(state->completionMutex);
    if (callbackContext->deregistration) {
      state->deregistrationStatus = status;
      state->deregistrationComplete = true;
    } else {
      state->registrationStatus = status;
      state->registrationComplete = true;
    }
  }
  // Windows supplies a separately allocated instance to completion callbacks.
  // The state-owned instance is released by RegistrationState after all
  // asynchronous operations have quiesced.
  if (instance != nullptr && instance != state->instance) DnsServiceFreeInstance(instance);
  state->completionChanged.notify_all();
  (void)keepAlive;
}

bool MdnsService::waitForRegistrationCompletion(std::chrono::milliseconds timeout) {
  if (registrationState_ == nullptr) return false;
  std::unique_lock<std::mutex> lock(registrationState_->completionMutex);
  if (!registrationState_->completionChanged.wait_for(
          lock, timeout, [this] { return registrationState_->registrationComplete; })) {
    return false;
  }
  return registrationState_->registrationStatus == ERROR_SUCCESS;
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
  registrationState_ = std::make_shared<RegistrationState>();
  registrationState_->instance = DnsServiceConstructInstance(
      L"Inventatory._inventatory._tcp.local", wideHost.c_str(), haveIpv4Address ? &ipv4Address : nullptr,
      nullptr, port, 0, 0, 1, keys, values);
  // mDNS is a local-network discovery mechanism.  Do not publish a service
  // instance that has no private LAN address (for example on a public/VPN-only
  // host), even though the HTTP listener may still be reachable by an address
  // known to the user.
  if (registrationState_->instance == nullptr || !haveIpv4Address) {
    registrationState_.reset();
    return false;
  }
  auto& request = registrationState_->registerRequest;
  request.Version = DNS_QUERY_REQUEST_VERSION1;
  request.InterfaceIndex = 0;
  request.pServiceInstance = registrationState_->instance;
  request.pRegisterCompletionCallback = &MdnsService::registrationComplete;
  request.unicastEnabled = FALSE;
  auto registrationCallback = std::make_shared<RegistrationState::CallbackContext>();
  registrationCallback->state = registrationState_;
  registrationCallback->self = registrationCallback;
  request.pQueryContext = registrationCallback.get();
  {
    std::lock_guard<std::mutex> lock(registrationState_->completionMutex);
    registrationState_->registrationComplete = false;
    registrationState_->registrationStatus = ERROR_IO_PENDING;
  }
  const auto status = DnsServiceRegister(&request, &registrationState_->registrationCancel);
  if (status != ERROR_SUCCESS && status != DNS_REQUEST_PENDING) {
    registrationCallback->self.reset();
    registrationCallback.reset();
    registrationState_.reset();
    return false;
  }
  if (status == ERROR_SUCCESS) {
    registrationCallback->self.reset();
    registrationCallback.reset();
    {
      std::lock_guard<std::mutex> lock(registrationState_->completionMutex);
      registrationState_->registrationComplete = true;
      registrationState_->registrationStatus = ERROR_SUCCESS;
    }
  }
  if (status == DNS_REQUEST_PENDING && !waitForRegistrationCompletion(std::chrono::seconds(2))) {
    bool registrationComplete = false;
    {
      std::lock_guard<std::mutex> lock(registrationState_->completionMutex);
      registrationComplete = registrationState_->registrationComplete;
    }
    if (!registrationComplete) DnsServiceRegisterCancel(&registrationState_->registrationCancel);

    auto deregistrationCallback = std::make_shared<RegistrationState::CallbackContext>();
    deregistrationCallback->state = registrationState_;
    deregistrationCallback->self = deregistrationCallback;
    registrationState_->deregisterRequest = registrationState_->registerRequest;
    registrationState_->deregisterRequest.pQueryContext = deregistrationCallback.get();
    const auto deregistrationStatus = DnsServiceDeRegister(&registrationState_->deregisterRequest, nullptr);
    if (deregistrationStatus == DNS_REQUEST_PENDING) {
      std::unique_lock<std::mutex> lock(registrationState_->completionMutex);
      registrationState_->completionChanged.wait_for(
          lock, std::chrono::seconds(2), [this] { return registrationState_->deregistrationComplete; });
    } else {
      deregistrationCallback->self.reset();
      deregistrationCallback.reset();
      std::lock_guard<std::mutex> lock(registrationState_->completionMutex);
      registrationState_->deregistrationComplete = true;
      registrationState_->deregistrationStatus = deregistrationStatus;
    }
    registrationState_.reset();
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
  if (registrationState_ != nullptr) {
    bool registrationComplete = false;
    {
      std::lock_guard<std::mutex> lock(registrationState_->completionMutex);
      registrationComplete = registrationState_->registrationComplete;
    }
    if (!registrationComplete) DnsServiceRegisterCancel(&registrationState_->registrationCancel);

    auto deregistrationCallback = std::make_shared<RegistrationState::CallbackContext>();
    deregistrationCallback->state = registrationState_;
    deregistrationCallback->self = deregistrationCallback;
    registrationState_->deregisterRequest = registrationState_->registerRequest;
    registrationState_->deregisterRequest.pQueryContext = deregistrationCallback.get();
    const auto status = DnsServiceDeRegister(&registrationState_->deregisterRequest, nullptr);
    if (status == DNS_REQUEST_PENDING) {
      std::unique_lock<std::mutex> lock(registrationState_->completionMutex);
      registrationState_->completionChanged.wait_for(
          lock, std::chrono::seconds(2), [this] { return registrationState_->deregistrationComplete; });
    } else {
      deregistrationCallback->self.reset();
      deregistrationCallback.reset();
      std::lock_guard<std::mutex> lock(registrationState_->completionMutex);
      registrationState_->deregistrationComplete = true;
      registrationState_->deregistrationStatus = status;
    }
  }
  registrationState_.reset();
#endif
  running_ = false;
}

bool MdnsService::running() const {
  return running_;
}

}  // namespace inventatory
