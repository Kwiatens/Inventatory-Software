// Inventatory Scan R1 BLE discovery and authenticated provisioning through BlueZ.

#include "platform/scanner/BleProvisioningService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gio/gio.h>

namespace inventatory {
namespace {

constexpr const char* kSetupServiceUuid = "d4d4f5b0-4c21-4a7e-a1a1-4b0db2d00001";
constexpr const char* kStatusCharacteristicUuid = "d4d4f5b0-4c21-4a7e-a1a1-4b0db2d00002";
constexpr const char* kRequestCharacteristicUuid = "d4d4f5b0-4c21-4a7e-a1a1-4b0db2d00003";
constexpr const char* kBluezName = "org.bluez";
constexpr const char* kAgentPath = "/org/kwiatens/inventatory/agent";
constexpr size_t kMaximumDiscoveredDevices = 256U;

const char kAgentXml[] =
    "<node><interface name='org.bluez.Agent1'>"
    "<method name='Release'/>"
    "<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
    "<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
    "<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
    "<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
    "<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
    "<method name='Cancel'/>"
    "</interface></node>";

bool validAsciiWifiText(const std::string& value, size_t maximum) {
  return value.size() <= maximum &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 0x20U && ch != 0x7fU; });
}

bool validToken(const std::string& value) {
  return value.size() >= 32U && value.size() <= 128U &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool validPairingCode(const std::string& value) {
  return value.size() == 6U &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool parseBluetoothAddress(const std::string& value, uint64_t& address) {
  address = 0;
  unsigned digits = 0;
  for (const unsigned char ch : value) {
    if (ch == ':') continue;
    if (!std::isxdigit(ch)) return false;
    const unsigned nibble = ch >= '0' && ch <= '9' ? ch - '0' :
                            static_cast<unsigned>(std::tolower(ch) - 'a' + 10);
    if (++digits > 12U) return false;
    address = (address << 4U) | nibble;
  }
  return digits == 12U;
}

GVariant* callBluez(GDBusConnection* connection, const std::string& objectPath, const char* interfaceName,
                    const char* methodName, GVariant* parameters, const GVariantType* expectedReply,
                    int timeoutMs, std::string* errorText = nullptr) {
  if (connection == nullptr) return nullptr;
  GError* error = nullptr;
  auto* result = g_dbus_connection_call_sync(connection, kBluezName, objectPath.c_str(), interfaceName, methodName,
                                              parameters, expectedReply, G_DBUS_CALL_FLAGS_NONE, timeoutMs, nullptr,
                                              &error);
  if (result == nullptr && errorText != nullptr) {
    *errorText = error == nullptr ? "Bluetooth service request failed" : error->message;
  }
  if (error != nullptr) g_error_free(error);
  return result;
}

GVariant* managedObjects(GDBusConnection* connection, int timeoutMs = 5000) {
  auto* reply = callBluez(connection, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
                          G_VARIANT_TYPE("(a{oa{sa{sv}}})"), timeoutMs);
  if (reply == nullptr) return nullptr;
  GVariant* objects = nullptr;
  g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
  g_variant_unref(reply);
  return objects;
}

std::string stringProperty(GVariant* properties, const char* name) {
  const gchar* value = nullptr;
  return g_variant_lookup(properties, name, "&s", &value) && value != nullptr ? value : std::string();
}

std::string objectPathProperty(GVariant* properties, const char* name) {
  const gchar* value = nullptr;
  return g_variant_lookup(properties, name, "&o", &value) && value != nullptr ? value : std::string();
}

bool hasServiceUuid(GVariant* properties) {
  auto* uuids = g_variant_lookup_value(properties, "UUIDs", G_VARIANT_TYPE_STRING_ARRAY);
  if (uuids == nullptr) return false;
  bool found = false;
  GVariantIter iterator;
  g_variant_iter_init(&iterator, uuids);
  const gchar* uuid = nullptr;
  while (g_variant_iter_next(&iterator, "&s", &uuid)) {
    if (uuid != nullptr && lowercase(uuid) == kSetupServiceUuid) {
      found = true;
      break;
    }
  }
  g_variant_unref(uuids);
  return found;
}

std::vector<std::string> adapterPaths(GVariant* objects) {
  std::vector<std::string> result;
  GVariantIter objectIterator;
  g_variant_iter_init(&objectIterator, objects);
  const gchar* objectPath = nullptr;
  GVariant* interfaces = nullptr;
  while (g_variant_iter_next(&objectIterator, "{&o@a{sa{sv}}}", &objectPath, &interfaces)) {
    auto* adapter = g_variant_lookup_value(interfaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
    if (adapter != nullptr) {
      result.emplace_back(objectPath);
      g_variant_unref(adapter);
    }
    g_variant_unref(interfaces);
  }
  return result;
}

std::string findDevicePath(GVariant* objects, uint64_t wantedAddress) {
  GVariantIter objectIterator;
  g_variant_iter_init(&objectIterator, objects);
  const gchar* objectPath = nullptr;
  GVariant* interfaces = nullptr;
  while (g_variant_iter_next(&objectIterator, "{&o@a{sa{sv}}}", &objectPath, &interfaces)) {
    auto* device = g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
    if (device != nullptr) {
      uint64_t foundAddress = 0;
      const auto address = stringProperty(device, "Address");
      const bool matches = parseBluetoothAddress(address, foundAddress) && foundAddress == wantedAddress;
      g_variant_unref(device);
      if (matches) {
        const std::string result(objectPath);
        g_variant_unref(interfaces);
        return result;
      }
    }
    g_variant_unref(interfaces);
  }
  return {};
}

bool findGattCharacteristics(GVariant* objects, const std::string& devicePath,
                              std::string& statusPath, std::string& requestPath) {
  statusPath.clear();
  requestPath.clear();
  GVariantIter objectIterator;
  g_variant_iter_init(&objectIterator, objects);
  const gchar* objectPath = nullptr;
  GVariant* interfaces = nullptr;
  while (g_variant_iter_next(&objectIterator, "{&o@a{sa{sv}}}", &objectPath, &interfaces)) {
    auto* service = g_variant_lookup_value(interfaces, "org.bluez.GattService1", G_VARIANT_TYPE("a{sv}"));
    if (service != nullptr) {
      const auto uuid = lowercase(stringProperty(service, "UUID"));
      const auto device = objectPathProperty(service, "Device");
      if (uuid == kSetupServiceUuid && device == devicePath) {
        const std::string servicePath(objectPath);
        g_variant_unref(service);
        auto* characteristic = g_variant_lookup_value(interfaces, "org.bluez.GattCharacteristic1",
                                                      G_VARIANT_TYPE("a{sv}"));
        if (characteristic != nullptr) {
          const auto characteristicUuid = lowercase(stringProperty(characteristic, "UUID"));
          const auto parentService = objectPathProperty(characteristic, "Service");
          if (parentService == servicePath) {
            if (characteristicUuid == kStatusCharacteristicUuid) statusPath = objectPath;
            else if (characteristicUuid == kRequestCharacteristicUuid) requestPath = objectPath;
          }
          g_variant_unref(characteristic);
        }
      } else {
        g_variant_unref(service);
      }
    } else {
      auto* characteristic = g_variant_lookup_value(interfaces, "org.bluez.GattCharacteristic1",
                                                    G_VARIANT_TYPE("a{sv}"));
      if (characteristic != nullptr) {
        const auto characteristicUuid = lowercase(stringProperty(characteristic, "UUID"));
        const auto parentService = objectPathProperty(characteristic, "Service");
        if (characteristicUuid == kStatusCharacteristicUuid) {
          // The parent service is checked again below after its object appears.
          statusPath = objectPath;
          (void)parentService;
        } else if (characteristicUuid == kRequestCharacteristicUuid) {
          requestPath = objectPath;
        }
        g_variant_unref(characteristic);
      }
    }
    g_variant_unref(interfaces);
  }
  if (statusPath.empty() || requestPath.empty()) return false;
  const auto parentServicePath = [](const std::string& path) {
    const auto separator = path.rfind("/char");
    return separator == std::string::npos ? std::string() : path.substr(0, separator);
  };
  if (parentServicePath(statusPath) != parentServicePath(requestPath)) return false;
  const auto servicePath = parentServicePath(statusPath);
  GVariant* serviceProperties = nullptr;
  GVariantIter verifyIterator;
  g_variant_iter_init(&verifyIterator, objects);
  const gchar* verifyPath = nullptr;
  GVariant* verifyInterfaces = nullptr;
  while (g_variant_iter_next(&verifyIterator, "{&o@a{sa{sv}}}", &verifyPath, &verifyInterfaces)) {
    if (servicePath == verifyPath) {
      serviceProperties = g_variant_lookup_value(verifyInterfaces, "org.bluez.GattService1", G_VARIANT_TYPE("a{sv}"));
      g_variant_unref(verifyInterfaces);
      break;
    }
    g_variant_unref(verifyInterfaces);
  }
  if (serviceProperties == nullptr) return false;
  const bool correctService = lowercase(stringProperty(serviceProperties, "UUID")) == kSetupServiceUuid &&
                              objectPathProperty(serviceProperties, "Device") == devicePath;
  g_variant_unref(serviceProperties);
  return correctService;
}

struct NotificationState {
  std::mutex mutex;
  std::condition_variable changed;
  std::string latestStatus;
  bool sawConnecting = false;
  bool terminal = false;
  bool succeeded = false;
};

void propertiesChanged(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters,
                       gpointer userData) {
  auto* state = static_cast<NotificationState*>(userData);
  const gchar* interfaceName = nullptr;
  GVariant* changed = nullptr;
  GVariant* invalidated = nullptr;
  g_variant_get(parameters, "(&s@a{sv}@as)", &interfaceName, &changed, &invalidated);
  (void)invalidated;
  if (interfaceName != nullptr && std::strcmp(interfaceName, "org.bluez.GattCharacteristic1") == 0) {
    auto* value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE_BYTESTRING);
    if (value == nullptr) value = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
    if (value != nullptr) {
      gsize size = 0;
      const auto* bytes = static_cast<const char*>(g_variant_get_fixed_array(value, &size, sizeof(guint8)));
      const std::string status(bytes == nullptr ? "" : std::string(bytes, size));
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->latestStatus = status;
        if (status == "CONNECTING") state->sawConnecting = true;
        if (status.rfind("SUCCESS ", 0) == 0) {
          state->terminal = true;
          state->succeeded = true;
        } else if (status.rfind("FAILED ", 0) == 0) {
          state->terminal = true;
          state->succeeded = false;
        }
      }
      state->changed.notify_one();
      g_variant_unref(value);
    }
  }
  g_variant_unref(changed);
  g_variant_unref(invalidated);
}

class BluezAgent final {
 public:
  BluezAgent(std::string pairingCode, std::string expectedDevicePath)
      : pairingCode_(std::move(pairingCode)), expectedDevicePath_(std::move(expectedDevicePath)) {}
  ~BluezAgent() { stop(); }

  BluezAgent(const BluezAgent&) = delete;
  BluezAgent& operator=(const BluezAgent&) = delete;

  bool start(std::string& error) {
    error.clear();
    loopThread_ = std::thread([this] { runLoop(); });
    {
      std::unique_lock<std::mutex> lock(mutex_);
      readyChanged_.wait_for(lock, std::chrono::seconds(5), [this] { return ready_; });
      if (!ready_ || connection_ == nullptr || registrationId_ == 0) {
        error = startupError_.empty() ? "Linux Bluetooth agent is unavailable" : startupError_;
        lock.unlock();
        stop();
        return false;
      }
    }
    auto* reply = callBluez(connection_, "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
                            g_variant_new("(os)", kAgentPath, "KeyboardDisplay"), G_VARIANT_TYPE("()"), 5000, &error);
    if (reply == nullptr) {
      stop();
      return false;
    }
    g_variant_unref(reply);
    registered_ = true;
    reply = callBluez(connection_, "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent",
                      g_variant_new("(o)", kAgentPath), G_VARIANT_TYPE("()"), 5000, &error);
    if (reply == nullptr) {
      stop();
      return false;
    }
    g_variant_unref(reply);
    return true;
  }

  bool verified() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return verified_;
  }

  bool pairingCancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
  }

  bool isExpectedDevice(const gchar* devicePath) const {
    if (devicePath == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return expectedDevicePath_ == devicePath;
  }

  void setExpectedDevicePath(std::string devicePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    expectedDevicePath_ = std::move(devicePath);
  }

  guint subscribeToStatus(const std::string& objectPath, NotificationState* notification) {
    struct Request {
      std::mutex mutex;
      std::condition_variable changed;
      GDBusConnection* connection = nullptr;
      std::string objectPath;
      NotificationState* notification = nullptr;
      guint subscription = 0;
      bool complete = false;
    } request;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      request.connection = connection_;
      if (context_ == nullptr || request.connection == nullptr) return 0;
      request.objectPath = objectPath;
      request.notification = notification;
      g_object_ref(request.connection);
      g_main_context_invoke(context_, [](gpointer userData) -> gboolean {
        auto* item = static_cast<Request*>(userData);
        item->subscription = g_dbus_connection_signal_subscribe(
            item->connection, kBluezName, "org.freedesktop.DBus.Properties", "PropertiesChanged",
            item->objectPath.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, propertiesChanged,
            item->notification, nullptr);
        {
          std::lock_guard<std::mutex> lock(item->mutex);
          item->complete = true;
        }
        item->changed.notify_one();
        return G_SOURCE_REMOVE;
      }, &request);
    }
    {
      std::unique_lock<std::mutex> lock(request.mutex);
      request.changed.wait(lock, [&] { return request.complete; });
    }
    g_object_unref(request.connection);
    return request.complete ? request.subscription : 0;
  }

  void unsubscribeStatus(guint subscription) {
    if (subscription == 0) return;
    GMainContext* context = nullptr;
    GDBusConnection* connection = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      context = context_;
      connection = connection_;
      if (connection != nullptr) g_object_ref(connection);
    }
    if (context == nullptr || connection == nullptr) {
      if (connection != nullptr) g_object_unref(connection);
      return;
    }
    struct Request {
      std::mutex mutex;
      std::condition_variable changed;
      GDBusConnection* connection = nullptr;
      guint subscription = 0;
      bool complete = false;
    } request;
    request.connection = connection;
    request.subscription = subscription;
    g_main_context_invoke(context, [](gpointer userData) -> gboolean {
      auto* item = static_cast<Request*>(userData);
      g_dbus_connection_signal_unsubscribe(item->connection, item->subscription);
      {
        std::lock_guard<std::mutex> lock(item->mutex);
        item->complete = true;
      }
      item->changed.notify_one();
      return G_SOURCE_REMOVE;
    }, &request);
    {
      std::unique_lock<std::mutex> lock(request.mutex);
      request.changed.wait(lock, [&] { return request.complete; });
    }
    g_object_unref(connection);
  }

  void stop() {
    if (registered_ && connection_ != nullptr) {
      auto* reply = callBluez(connection_, "/org/bluez", "org.bluez.AgentManager1", "UnregisterAgent",
                              g_variant_new("(o)", kAgentPath), G_VARIANT_TYPE("()"), 2000);
      if (reply != nullptr) g_variant_unref(reply);
      registered_ = false;
    }
    GMainContext* context = nullptr;
    GMainLoop* loop = nullptr;
    bool requestLoopStop = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopRequested_ = true;
      context = context_;
      loop = loop_;
      requestLoopStop = registrationId_ != 0;
    }
    if (requestLoopStop && context != nullptr && loop != nullptr) {
      // Attach a source instead of invoking directly: invoke may run inline
      // when the loop thread has published its context but has not entered
      // g_main_loop_run() yet, losing the quit request in that startup window.
      auto* source = g_idle_source_new();
      g_source_set_callback(source, [](gpointer userData) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(userData));
        return G_SOURCE_REMOVE;
      }, loop, nullptr);
      g_source_attach(source, context);
      g_source_unref(source);
    }
    if (loopThread_.joinable()) loopThread_.join();
  }

 private:
  static void methodCall(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* methodName,
                         GVariant* parameters, GDBusMethodInvocation* invocation, gpointer userData) {
    auto* agent = static_cast<BluezAgent*>(userData);
    if (std::strcmp(methodName, "Release") == 0) {
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (std::strcmp(methodName, "Cancel") == 0) {
      {
        std::lock_guard<std::mutex> lock(agent->mutex_);
        agent->cancelled_ = true;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (std::strcmp(methodName, "RequestPinCode") == 0) {
      const gchar* devicePath = nullptr;
      g_variant_get(parameters, "(&o)", &devicePath);
      if (!agent->isExpectedDevice(devicePath)) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "Only the selected Inventatory scanner can be paired");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(agent->mutex_);
        agent->verified_ = true;
      }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", agent->pairingCode_.c_str()));
      return;
    }
    if (std::strcmp(methodName, "RequestPasskey") == 0) {
      const gchar* devicePath = nullptr;
      g_variant_get(parameters, "(&o)", &devicePath);
      if (!agent->isExpectedDevice(devicePath)) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "Only the selected Inventatory scanner can be paired");
        return;
      }
      const auto passkey = static_cast<guint32>(std::stoul(agent->pairingCode_));
      {
        std::lock_guard<std::mutex> lock(agent->mutex_);
        agent->verified_ = true;
      }
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", passkey));
      return;
    }
    if (std::strcmp(methodName, "DisplayPinCode") == 0) {
      const gchar* devicePath = nullptr;
      const gchar* code = nullptr;
      g_variant_get(parameters, "(&o&s)", &devicePath, &code);
      if (agent->isExpectedDevice(devicePath) && code != nullptr && agent->pairingCode_ == code) {
        std::lock_guard<std::mutex> lock(agent->mutex_);
        agent->verified_ = true;
      } else {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "The displayed scanner code does not match");
        return;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (std::strcmp(methodName, "DisplayPasskey") == 0) {
      const gchar* devicePath = nullptr;
      guint32 passkey = 0;
      guint16 entered = 0;
      g_variant_get(parameters, "(&ouq)", &devicePath, &passkey, &entered);
      (void)entered;
      if (agent->isExpectedDevice(devicePath) &&
          passkey == static_cast<guint32>(std::stoul(agent->pairingCode_))) {
        std::lock_guard<std::mutex> lock(agent->mutex_);
        agent->verified_ = true;
      } else {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "The displayed scanner code does not match");
        return;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (std::strcmp(methodName, "RequestConfirmation") == 0) {
      const gchar* devicePath = nullptr;
      guint32 passkey = 0;
      g_variant_get(parameters, "(&ou)", &devicePath, &passkey);
      if (!agent->isExpectedDevice(devicePath) ||
          passkey != static_cast<guint32>(std::stoul(agent->pairingCode_))) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "The scanner code was not confirmed");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(agent->mutex_);
        agent->verified_ = true;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (std::strcmp(methodName, "RequestAuthorization") == 0) {
      const gchar* devicePath = nullptr;
      g_variant_get(parameters, "(&o)", &devicePath);
      if (!agent->isExpectedDevice(devicePath) || !agent->verified()) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "Physical scanner code was not verified");
        return;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (std::strcmp(methodName, "AuthorizeService") == 0) {
      const gchar* devicePath = nullptr;
      const gchar* uuid = nullptr;
      g_variant_get(parameters, "(&o&s)", &devicePath, &uuid);
      if (!agent->isExpectedDevice(devicePath) || uuid == nullptr || lowercase(uuid) != kSetupServiceUuid ||
          !agent->verified()) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "Only the verified Inventatory setup service is authorized");
        return;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected", "Unsupported pairing request");
  }

  void runLoop() {
    auto* localContext = g_main_context_new();
    g_main_context_push_thread_default(localContext);
    GError* error = nullptr;
    auto* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    GDBusNodeInfo* node = nullptr;
    guint registration = 0;
    if (connection != nullptr) {
      node = g_dbus_node_info_new_for_xml(kAgentXml, &error);
      if (node != nullptr) {
        static const GDBusInterfaceVTable vtable = {methodCall, nullptr, nullptr, {nullptr}};
        registration = g_dbus_connection_register_object(connection, kAgentPath, node->interfaces[0], &vtable,
                                                          this, nullptr, &error);
      }
    }
    auto* loop = g_main_loop_new(localContext, FALSE);
    bool shouldRunLoop = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      context_ = localContext;
      connection_ = connection;
      loop_ = loop;
      registrationId_ = registration;
      if (error != nullptr) startupError_ = error->message;
      ready_ = true;
      shouldRunLoop = registration != 0 && connection != nullptr && !stopRequested_;
    }
    readyChanged_.notify_all();
    if (shouldRunLoop) g_main_loop_run(loop);
    if (registration != 0 && connection != nullptr) g_dbus_connection_unregister_object(connection, registration);
    if (node != nullptr) g_dbus_node_info_unref(node);
    if (error != nullptr) g_error_free(error);
    if (connection != nullptr) g_object_unref(connection);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(localContext);
    g_main_context_unref(localContext);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      context_ = nullptr;
      connection_ = nullptr;
      loop_ = nullptr;
      registrationId_ = 0;
    }
  }

  std::string pairingCode_;
  std::string expectedDevicePath_;
  mutable std::mutex mutex_;
  std::condition_variable readyChanged_;
  std::thread loopThread_;
  GMainContext* context_ = nullptr;
  GDBusConnection* connection_ = nullptr;
  GMainLoop* loop_ = nullptr;
  guint registrationId_ = 0;
  bool stopRequested_ = false;
  bool ready_ = false;
  bool registered_ = false;
  bool verified_ = false;
  bool cancelled_ = false;
  std::string startupError_;
};

GVariant* getProperty(GDBusConnection* connection, const std::string& objectPath, const char* interfaceName,
                      const char* propertyName) {
  auto* reply = callBluez(connection, objectPath, "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new("(ss)", interfaceName, propertyName), G_VARIANT_TYPE("(v)"), 5000);
  if (reply == nullptr) return nullptr;
  GVariant* boxed = nullptr;
  g_variant_get(reply, "(@v)", &boxed);
  auto* value = g_variant_get_variant(boxed);
  g_variant_unref(boxed);
  g_variant_unref(reply);
  return value;
}

std::vector<std::string> uuidsForAdapterFilter() {
  return {kSetupServiceUuid};
}

}  // namespace

BleProvisioningService::BleProvisioningService() = default;
BleProvisioningService::~BleProvisioningService() { stopDiscovery(); }

void BleProvisioningService::startDiscovery() {
  stopDiscovery();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
    discovering_ = true;
  }
  worker_ = std::thread(&BleProvisioningService::discoveryLoop, this);
}

void BleProvisioningService::stopDiscovery() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    discovering_ = false;
  }
  if (worker_.joinable()) worker_.join();
}

std::vector<BleSetupDevice> BleProvisioningService::devices() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return devices_;
}

void BleProvisioningService::rememberDevice(uint64_t address, const std::string& name, int rssi) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = std::find_if(devices_.begin(), devices_.end(), [address](const BleSetupDevice& device) {
    return device.address == address;
  });
  if (found != devices_.end()) {
    found->name = name;
    found->rssi = rssi;
    return;
  }
  if (devices_.size() >= kMaximumDiscoveredDevices) return;
  devices_.push_back({address, name, rssi});
}

void BleProvisioningService::discoveryLoop() {
  GError* error = nullptr;
  auto* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (error != nullptr) g_error_free(error);
  if (connection == nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    discovering_ = false;
    return;
  }
  auto* initialObjects = managedObjects(connection);
  if (initialObjects == nullptr) {
    g_object_unref(connection);
    std::lock_guard<std::mutex> lock(mutex_);
    discovering_ = false;
    return;
  }
  const auto adapters = adapterPaths(initialObjects);
  g_variant_unref(initialObjects);
  std::vector<std::string> startedAdapters;
  for (const auto& adapter : adapters) {
    GVariantBuilder filter;
    g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("le"));
    const auto uuids = uuidsForAdapterFilter();
    const gchar* uuidPointers[] = {uuids.front().c_str(), nullptr};
    g_variant_builder_add(&filter, "{sv}", "UUIDs", g_variant_new_strv(uuidPointers, 1));
    auto* filterReply = callBluez(connection, adapter, "org.bluez.Adapter1", "SetDiscoveryFilter",
                                  g_variant_new("(@a{sv})", g_variant_builder_end(&filter)), G_VARIANT_TYPE("()"), 3000);
    if (filterReply != nullptr) g_variant_unref(filterReply);
    auto* startReply = callBluez(connection, adapter, "org.bluez.Adapter1", "StartDiscovery", nullptr,
                                 G_VARIANT_TYPE("()"), 3000);
    if (startReply != nullptr) {
      startedAdapters.push_back(adapter);
      g_variant_unref(startReply);
    }
  }

  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!discovering_) break;
    }
    auto* objects = managedObjects(connection, 3000);
    if (objects != nullptr) {
      GVariantIter objectIterator;
      g_variant_iter_init(&objectIterator, objects);
      const gchar* objectPath = nullptr;
      GVariant* interfaces = nullptr;
      while (g_variant_iter_next(&objectIterator, "{&o@a{sa{sv}}}", &objectPath, &interfaces)) {
        auto* device = g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
        if (device != nullptr && hasServiceUuid(device)) {
          uint64_t address = 0;
          const auto addressText = stringProperty(device, "Address");
          if (parseBluetoothAddress(addressText, address)) {
            auto name = stringProperty(device, "Alias");
            if (name.empty()) name = stringProperty(device, "Name");
            if (name.empty()) name = "Inventatory Scan R1";
            gint16 rssi = 0;
            g_variant_lookup(device, "RSSI", "n", &rssi);
            rememberDevice(address, name, rssi);
          }
          g_variant_unref(device);
        }
        g_variant_unref(interfaces);
      }
      g_variant_unref(objects);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }
  for (const auto& adapter : startedAdapters) {
    auto* reply = callBluez(connection, adapter, "org.bluez.Adapter1", "StopDiscovery", nullptr,
                            G_VARIANT_TYPE("()"), 3000);
    if (reply != nullptr) g_variant_unref(reply);
  }
  g_object_unref(connection);
}

BleProvisioningOutcome BleProvisioningService::provision(const BleProvisioningRequest& request,
                                                          std::string& publicError) const {
  publicError.clear();
  if (!validAsciiWifiText(request.wifiSsid, 32U) || request.wifiSsid.empty() ||
      !validAsciiWifiText(request.wifiPassword, 63U) || !validToken(request.deviceToken) ||
      !validPairingCode(request.pairingCode)) {
    publicError = "Enter a Wi-Fi name, valid pairing code, and generated device token";
    return BleProvisioningOutcome::Failed;
  }
  GError* connectionError = nullptr;
  auto* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &connectionError);
  if (connection == nullptr) {
    if (connectionError != nullptr) g_error_free(connectionError);
    publicError = "Linux Bluetooth service is unavailable; check that BlueZ is running";
    return BleProvisioningOutcome::Failed;
  }
  if (connectionError != nullptr) g_error_free(connectionError);

  auto* objects = managedObjects(connection);
  if (objects == nullptr) {
    g_object_unref(connection);
    publicError = "Linux Bluetooth service could not discover the scanner";
    return BleProvisioningOutcome::Failed;
  }
  auto devicePath = findDevicePath(objects, request.address);
  g_variant_unref(objects);
  if (devicePath.empty()) {
    g_object_unref(connection);
    publicError = "Scanner is no longer nearby; refresh Bluetooth discovery and retry";
    return BleProvisioningOutcome::Failed;
  }

  std::string agentError;
  BluezAgent agent(request.pairingCode, devicePath);
  if (!agent.start(agentError)) {
    g_object_unref(connection);
    publicError = "Secure Bluetooth pairing could not start; check the desktop Bluetooth service";
    return BleProvisioningOutcome::Failed;
  }

  // BlueZ does not expose the protection level of an existing bond through
  // Device1. Remove an old bond and require the six-digit code exchange every
  // time this setup flow runs, so an unauthenticated cached pairing is never
  // treated as proof that the physical scanner was verified.
  auto* deviceProperties = managedObjects(connection);
  bool alreadyPaired = false;
  if (deviceProperties != nullptr) {
    GVariantIter objectIterator;
    g_variant_iter_init(&objectIterator, deviceProperties);
    const gchar* objectPath = nullptr;
    GVariant* interfaces = nullptr;
    while (g_variant_iter_next(&objectIterator, "{&o@a{sa{sv}}}", &objectPath, &interfaces)) {
      if (devicePath == objectPath) {
        auto* device = g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
        if (device != nullptr) {
          gboolean paired = FALSE;
          g_variant_lookup(device, "Paired", "b", &paired);
          alreadyPaired = paired != FALSE;
          g_variant_unref(device);
        }
      }
      g_variant_unref(interfaces);
    }
    g_variant_unref(deviceProperties);
  }
  if (alreadyPaired) {
    const auto separator = devicePath.rfind("/dev_");
    const auto adapterPath = separator == std::string::npos ? std::string() : devicePath.substr(0, separator);
    auto* removed = adapterPath.empty() ? nullptr
                                       : callBluez(connection, adapterPath, "org.bluez.Adapter1", "RemoveDevice",
                                                   g_variant_new("(o)", devicePath.c_str()), G_VARIANT_TYPE("()"), 5000);
    if (removed != nullptr) g_variant_unref(removed);
    auto* discoveryStarted = adapterPath.empty()
                                 ? nullptr
                                 : callBluez(connection, adapterPath, "org.bluez.Adapter1", "StartDiscovery", nullptr,
                                             G_VARIANT_TYPE("()"), 3000);
    if (discoveryStarted != nullptr) g_variant_unref(discoveryStarted);
    std::string refreshedPath;
    const auto discoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < discoveryDeadline && refreshedPath.empty()) {
      objects = managedObjects(connection, 3000);
      if (objects != nullptr) {
        refreshedPath = findDevicePath(objects, request.address);
        g_variant_unref(objects);
      }
      if (refreshedPath.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    auto* discoveryStopped = adapterPath.empty()
                                 ? nullptr
                                 : callBluez(connection, adapterPath, "org.bluez.Adapter1", "StopDiscovery", nullptr,
                                             G_VARIANT_TYPE("()"), 3000);
    if (discoveryStopped != nullptr) g_variant_unref(discoveryStopped);
    if (refreshedPath.empty()) {
      agent.stop();
      g_object_unref(connection);
      publicError = "The scanner could not be rediscovered after clearing its old Bluetooth pairing";
      return BleProvisioningOutcome::Failed;
    }
    devicePath = refreshedPath;
    agent.setExpectedDevicePath(devicePath);
  }

  std::string pairError;
  auto* paired = callBluez(connection, devicePath, "org.bluez.Device1", "Pair", nullptr, G_VARIANT_TYPE("()"),
                           30000, &pairError);
  if (paired == nullptr || !agent.verified() || agent.pairingCancelled()) {
    if (paired != nullptr) g_variant_unref(paired);
    agent.stop();
    g_object_unref(connection);
    publicError = "Secure Bluetooth pairing was not completed with the code shown on the scanner";
    return BleProvisioningOutcome::Failed;
  }
  g_variant_unref(paired);

  auto* connected = callBluez(connection, devicePath, "org.bluez.Device1", "Connect", nullptr,
                              G_VARIANT_TYPE("()"), 15000);
  if (connected == nullptr) {
    // BlueZ reports AlreadyConnected when its discovery session already owns
    // the connection. Treat that state as usable; subsequent GATT lookup is
    // the definitive connection check.
    const auto state = getProperty(connection, devicePath, "org.bluez.Device1", "Connected");
    gboolean isConnected = FALSE;
    if (state != nullptr) {
      g_variant_get(state, "b", &isConnected);
      g_variant_unref(state);
    }
    if (!isConnected) {
      agent.stop();
      g_object_unref(connection);
      publicError = "The scanner could not open a secure Bluetooth connection";
      return BleProvisioningOutcome::Failed;
    }
  } else {
    g_variant_unref(connected);
  }

  std::string statusPath;
  std::string requestPath;
  bool foundCharacteristics = false;
  for (int attempt = 0; attempt < 12; ++attempt) {
    objects = managedObjects(connection, 5000);
    if (objects != nullptr) {
      foundCharacteristics = findGattCharacteristics(objects, devicePath, statusPath, requestPath);
      g_variant_unref(objects);
      if (foundCharacteristics) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (!foundCharacteristics) {
    agent.stop();
    g_object_unref(connection);
    publicError = "Scanner setup service is unavailable; keep the R1 nearby and retry";
    return BleProvisioningOutcome::Failed;
  }

  NotificationState notification;
  const auto subscription = agent.subscribeToStatus(statusPath, &notification);
  if (subscription == 0) {
    agent.stop();
    g_object_unref(connection);
    publicError = "Scanner setup confirmation channel is unavailable";
    return BleProvisioningOutcome::Failed;
  }
  auto* notifyReply = callBluez(connection, statusPath, "org.bluez.GattCharacteristic1", "StartNotify", nullptr,
                                G_VARIANT_TYPE("()"), 8000);
  if (notifyReply == nullptr) {
    agent.unsubscribeStatus(subscription);
    agent.stop();
    g_object_unref(connection);
    publicError = "Scanner setup confirmation channel is unavailable";
    return BleProvisioningOutcome::Failed;
  }
  g_variant_unref(notifyReply);

  std::vector<guint8> payload;
  payload.reserve(4U + request.wifiSsid.size() + request.wifiPassword.size() + request.deviceToken.size());
  payload.push_back(1U);
  payload.push_back(static_cast<guint8>(request.wifiSsid.size()));
  payload.push_back(static_cast<guint8>(request.wifiPassword.size()));
  payload.push_back(static_cast<guint8>(request.deviceToken.size()));
  payload.insert(payload.end(), request.wifiSsid.begin(), request.wifiSsid.end());
  payload.insert(payload.end(), request.wifiPassword.begin(), request.wifiPassword.end());
  payload.insert(payload.end(), request.deviceToken.begin(), request.deviceToken.end());
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string("request"));
  auto* value = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, payload.data(), payload.size(), sizeof(guint8));
  auto* writeReply = callBluez(connection, requestPath, "org.bluez.GattCharacteristic1", "WriteValue",
                               g_variant_new("(@ay@a{sv})", value, g_variant_builder_end(&options)),
                               G_VARIANT_TYPE("()"), 15000);
  std::fill(payload.begin(), payload.end(), 0U);

  bool resultWasTerminal = false;
  bool resultSucceeded = false;
  bool resultSawConnecting = false;
  {
    std::unique_lock<std::mutex> lock(notification.mutex);
    notification.changed.wait_for(lock, std::chrono::seconds(25), [&] { return notification.terminal; });
    resultWasTerminal = notification.terminal;
    resultSucceeded = notification.succeeded;
    resultSawConnecting = notification.sawConnecting;
  }
  if (writeReply != nullptr) g_variant_unref(writeReply);
  auto* stopReply = callBluez(connection, statusPath, "org.bluez.GattCharacteristic1", "StopNotify", nullptr,
                              G_VARIANT_TYPE("()"), 2000);
  if (stopReply != nullptr) g_variant_unref(stopReply);
  agent.unsubscribeStatus(subscription);
  agent.stop();
  g_object_unref(connection);

  if (resultWasTerminal) {
    if (resultSucceeded) return BleProvisioningOutcome::Succeeded;
    publicError = "The scanner rejected the Wi-Fi setup request";
    return BleProvisioningOutcome::Failed;
  }
  if (writeReply != nullptr) {
    publicError = "Scanner setup was sent, but the scanner did not confirm it before Bluetooth closed";
    return BleProvisioningOutcome::Indeterminate;
  }
  if (resultSawConnecting) {
    publicError = "Scanner setup may have been accepted, but Bluetooth did not confirm the final result";
    return BleProvisioningOutcome::Indeterminate;
  }
  publicError = "Secure setup request was rejected; keep the scanner nearby and retry";
  return BleProvisioningOutcome::Failed;
}

}  // namespace inventatory
