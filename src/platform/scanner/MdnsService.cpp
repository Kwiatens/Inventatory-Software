// Inventatory - Hardware Inventory Management System
// Platform DNS-SD registration for automatic Inventatory Scan discovery.

#include "platform/scanner/MdnsService.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <gio/gio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <thread>
#endif

namespace inventatory {
using std::string;

#ifndef _WIN32
namespace {

bool privateIpv4(uint32_t hostOrder) {
  const auto first = (hostOrder >> 24U) & 0xffU;
  const auto second = (hostOrder >> 16U) & 0xffU;
  return first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
         (first == 192U && second == 168U) || (first == 169U && second == 254U);
}

string privateInterfaceName(const string& requestedAddress) {
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return {};
  string selected;
  for (auto* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_name == nullptr ||
        (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
        entry->ifa_addr->sa_family != AF_INET) continue;
    const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
    char buffer[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) != nullptr &&
        requestedAddress == buffer && privateIpv4(ntohl(address->sin_addr.s_addr))) {
      selected = entry->ifa_name;
      break;
    }
  }
  freeifaddrs(interfaces);
  return selected;
}

// Avahi's D-Bus API (org.freedesktop.Avahi).  avahi-publish-service has no
// option to select an interface, so the registration goes straight to the
// daemon, which lets one entry group be published on a single interface index.
constexpr const char* kAvahiBusName = "org.freedesktop.Avahi";
constexpr const char* kAvahiServerInterface = "org.freedesktop.Avahi.Server";
constexpr const char* kAvahiEntryGroupInterface = "org.freedesktop.Avahi.EntryGroup";
constexpr gint32 kAvahiProtocolInet = 0;  // AVAHI_PROTO_INET: IPv4 only, like the listener
constexpr gint32 kAvahiEntryGroupEstablished = 2;
constexpr gint32 kAvahiEntryGroupCollision = 3;
constexpr gint32 kAvahiEntryGroupFailure = 4;
constexpr int kAvahiCallTimeoutMs = 1000;
constexpr auto kAvahiEstablishTimeout = std::chrono::seconds(2);

// Synchronous call that never auto-starts avahi-daemon.  Returns a full
// reference (or nullptr on any error); `parameters` is consumed either way.
GVariant* callAvahi(GDBusConnection* connection, const char* objectPath, const char* interfaceName,
                    const char* methodName, GVariant* parameters, const char* replyType) {
  GError* error = nullptr;
  auto* reply = g_dbus_connection_call_sync(connection, kAvahiBusName, objectPath, interfaceName, methodName,
                                            parameters, G_VARIANT_TYPE(replyType),
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, kAvahiCallTimeoutMs, nullptr, &error);
  if (error != nullptr) g_error_free(error);
  return reply;
}

// TXT record exactly as before: a single "protocol=1" string and nothing else.
GVariant* serviceTxtRecords() {
  static const char kProtocolText[] = "protocol=1";
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("aay"));
  g_variant_builder_add_value(
      &builder, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, kProtocolText, sizeof(kProtocolText) - 1U, 1U));
  return g_variant_builder_end(&builder);
}

}  // namespace

// Owns a private system-bus connection and the entry group registered on it.
// A private connection is deliberate: avahi-daemon drops every entry group of a
// client that disconnects, so the advertisement disappears when the application
// exits or is killed, and the process is never terminated by a shared
// connection's exit-on-close behaviour if the system bus goes away.  All calls
// are synchronous with short timeouts and run on the caller's (UI) thread.
struct MdnsService::AvahiPublication {
  AvahiPublication() = default;
  AvahiPublication(const AvahiPublication&) = delete;
  AvahiPublication& operator=(const AvahiPublication&) = delete;
  ~AvahiPublication() { release(); }

  bool connect() {
    GError* error = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (address != nullptr) {
      connection = g_dbus_connection_new_for_address_sync(
          address,
          static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
          nullptr, nullptr, &error);
      g_free(address);
    }
    if (error != nullptr) g_error_free(error);
    return connection != nullptr;
  }

  gint32 groupState() const {
    auto* reply = callAvahi(connection, groupPath.c_str(), kAvahiEntryGroupInterface, "GetState", nullptr, "(i)");
    if (reply == nullptr) return kAvahiEntryGroupFailure;
    gint32 state = kAvahiEntryGroupFailure;
    g_variant_get(reply, "(i)", &state);
    g_variant_unref(reply);
    return state;
  }

  bool publish(unsigned interfaceIndex, std::uint16_t port) {
    auto* created = callAvahi(connection, "/", kAvahiServerInterface, "EntryGroupNew", nullptr, "(o)");
    if (created == nullptr) return false;
    const gchar* path = nullptr;
    g_variant_get(created, "(&o)", &path);
    if (path != nullptr) groupPath = path;
    g_variant_unref(created);
    if (groupPath.empty()) return false;

    // AddService(interface, protocol, flags, name, type, domain, host, port, txt):
    // a concrete interface index plus AVAHI_PROTO_INET keeps the service off
    // every other interface; the empty domain/host select the daemon defaults.
    auto* added = callAvahi(
        connection, groupPath.c_str(), kAvahiEntryGroupInterface, "AddService",
        g_variant_new("(iiussssq@aay)", static_cast<gint32>(interfaceIndex), kAvahiProtocolInet, 0U, "Inventatory",
                      "_inventatory._tcp", "", "", static_cast<guint16>(port), serviceTxtRecords()),
        "()");
    if (added == nullptr) return false;
    g_variant_unref(added);

    auto* committed = callAvahi(connection, groupPath.c_str(), kAvahiEntryGroupInterface, "Commit", nullptr, "()");
    if (committed == nullptr) return false;
    g_variant_unref(committed);

    // Wait for probing to finish so a name collision or daemon failure is
    // reported to the caller instead of leaving a silent non-advertisement.
    const auto deadline = std::chrono::steady_clock::now() + kAvahiEstablishTimeout;
    for (;;) {
      const auto state = groupState();
      if (state == kAvahiEntryGroupEstablished) return true;
      if (state == kAvahiEntryGroupCollision || state == kAvahiEntryGroupFailure) return false;
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

  void release() {
    if (connection == nullptr) return;
    if (!groupPath.empty()) {
      auto* freed = callAvahi(connection, groupPath.c_str(), kAvahiEntryGroupInterface, "Free", nullptr, "()");
      if (freed != nullptr) g_variant_unref(freed);
      groupPath.clear();
    }
    GError* error = nullptr;
    g_dbus_connection_close_sync(connection, nullptr, &error);
    if (error != nullptr) g_error_free(error);
    g_object_unref(connection);
    connection = nullptr;
  }

  GDBusConnection* connection = nullptr;
  string groupPath;
};
#endif

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

bool MdnsService::start(std::uint16_t port, const string& boundAddress) {
  stop();
#ifdef _WIN32
  std::array<char, 256> host{};
  if (gethostname(host.data(), static_cast<int>(host.size())) != 0) return false;
  std::wstring wideHost;
  for (const char ch : std::string(host.data())) wideHost.push_back(static_cast<unsigned char>(ch));
  wideHost += L".local";
  IN_ADDR parsedAddress{};
  const bool haveIpv4Address = InetPtonA(AF_INET, boundAddress.c_str(), &parsedAddress) == 1 &&
                               isPrivateIpv4(parsedAddress.S_un.S_addr);
  IP4_ADDRESS ipv4Address = haveIpv4Address ? parsedAddress.S_un.S_addr : 0;
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
  // Only the private-LAN interface that owns the bound address may advertise
  // the service.  No such interface (loopback, public, unknown) means no
  // advertisement at all, and this gate runs before any D-Bus traffic.
  const auto interfaceName = privateInterfaceName(boundAddress);
  if (interfaceName.empty() || interfaceName.size() >= IFNAMSIZ) return false;
  const auto interfaceIndex = if_nametoindex(interfaceName.c_str());
  if (interfaceIndex == 0) return false;
  auto publication = std::make_shared<AvahiPublication>();
  if (!publication->connect() || !publication->publish(interfaceIndex, port)) return false;
  publication_ = std::move(publication);
  running_ = true;
  return true;
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
#ifndef _WIN32
  // Frees the entry group and closes the private connection (idempotent).
  publication_.reset();
#endif
  running_ = false;
}

bool MdnsService::running() const {
  return running_;
}

}  // namespace inventatory
