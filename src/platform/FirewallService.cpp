// Inventatory - Windows Firewall hardening for the Scan R1 bridge.

#include "platform/FirewallService.h"

#ifdef _WIN32

#include <windows.h>
#include <netfw.h>
#include <oleauto.h>

#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace inventatory {

namespace {

constexpr wchar_t kRuleName[] = L"Inventatory Scan R1 Bridge";
constexpr wchar_t kRuleDescription[] =
    L"Allows the Inventatory Scan R1 bridge on the current private-network LAN port.";

class ComString {
 public:
  explicit ComString(const std::wstring& value) : value_(SysAllocString(value.c_str())) {}
  ~ComString() { SysFreeString(value_); }
  BSTR get() const { return value_; }

 private:
  BSTR value_ = nullptr;
};

bool succeeded(HRESULT value, const char* operation, std::string& warning) {
  if (SUCCEEDED(value)) return true;
  warning = std::string("Windows Firewall could not ") + operation +
            ". The Scan R1 bridge is still running; allow Inventatory on Private networks or retry as administrator.";
  return false;
}

std::wstring widenUtf8(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                          length) != length) {
    return {};
  }
  return result;
}

}  // namespace

bool synchronizeScanFirewallRule(std::uint16_t port, const std::string& executablePath, std::string& warning) {
  warning.clear();
  if (port == 0 || executablePath.empty()) {
    warning = "Windows Firewall rule could not be prepared because the bridge port or executable path is unavailable.";
    return false;
  }

  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return succeeded(initialized, "initialize its firewall connection", warning);
  }
  const bool uninitialize = SUCCEEDED(initialized);

  INetFwPolicy2* policy = nullptr;
  INetFwRules* rules = nullptr;
  INetFwRule* rule = nullptr;
  auto cleanup = [&] {
    if (rule != nullptr) rule->Release();
    if (rules != nullptr) rules->Release();
    if (policy != nullptr) policy->Release();
    if (uninitialize) CoUninitialize();
  };

  HRESULT result = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy));
  if (!succeeded(result, "open its firewall policy", warning)) {
    cleanup();
    return false;
  }
  result = policy->get_Rules(&rules);
  if (!succeeded(result, "read its firewall rules", warning)) {
    cleanup();
    return false;
  }

  // Removing by the stable app-owned name makes port changes replace the old
  // rule instead of leaving a stale listening port open.
  ComString ruleName(kRuleName);
  rules->Remove(ruleName.get());

  result = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                            __uuidof(INetFwRule), reinterpret_cast<void**>(&rule));
  if (!succeeded(result, "create its firewall rule", warning)) {
    cleanup();
    return false;
  }
  const std::wstring portText = std::to_wstring(port);
  const auto applicationPath = widenUtf8(executablePath);
  if (applicationPath.empty()) {
    warning = "Windows Firewall could not scope its rule to the Inventatory executable because its path is invalid. "
              "The Scan R1 bridge is still running; allow Inventatory on Private networks or retry as administrator.";
    cleanup();
    return false;
  }
  ComString description(kRuleDescription);
  ComString application(applicationPath);
  ComString ports(portText);
  ComString remoteAddresses(L"LocalSubnet");

  if (!succeeded(rule->put_Name(ruleName.get()), "name its firewall rule", warning) ||
      !succeeded(rule->put_Description(description.get()), "describe its firewall rule", warning) ||
      !succeeded(rule->put_ApplicationName(application.get()), "scope its firewall rule to Inventatory", warning) ||
      !succeeded(rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP), "set the firewall protocol", warning) ||
      !succeeded(rule->put_LocalPorts(ports.get()), "set the firewall port", warning) ||
      !succeeded(rule->put_RemoteAddresses(remoteAddresses.get()), "set the firewall network scope", warning) ||
      !succeeded(rule->put_Direction(NET_FW_RULE_DIR_IN), "set the firewall direction", warning) ||
      !succeeded(rule->put_Action(NET_FW_ACTION_ALLOW), "set the firewall action", warning) ||
      !succeeded(rule->put_Profiles(NET_FW_PROFILE2_PRIVATE), "set the firewall profile", warning) ||
      !succeeded(rule->put_Enabled(VARIANT_TRUE), "enable the firewall rule", warning) ||
      !succeeded(rules->Add(rule), "install its firewall rule", warning)) {
    cleanup();
    return false;
  }
  cleanup();
  return true;
}

bool removeScanFirewallRule(std::string& warning) {
  warning.clear();
  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return succeeded(initialized, "initialize its firewall connection", warning);
  }
  const bool uninitialize = SUCCEEDED(initialized);
  INetFwPolicy2* policy = nullptr;
  INetFwRules* rules = nullptr;
  auto cleanup = [&] {
    if (rules != nullptr) rules->Release();
    if (policy != nullptr) policy->Release();
    if (uninitialize) CoUninitialize();
  };
  auto result = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                 __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy));
  if (!succeeded(result, "open its firewall policy", warning)) {
    cleanup();
    return false;
  }
  result = policy->get_Rules(&rules);
  if (!succeeded(result, "read its firewall rules", warning)) {
    cleanup();
    return false;
  }
  ComString ruleName(kRuleName);
  // ERROR_FILE_NOT_FOUND is represented as an HRESULT here when no old rule
  // exists; that is a successful no-op for shutdown/port changes.
  rules->Remove(ruleName.get());
  cleanup();
  return true;
}

}  // namespace inventatory

#else

namespace inventatory {
bool synchronizeScanFirewallRule(std::uint16_t, const std::string&, std::string& warning) {
  warning = "Windows Firewall hardening is unavailable on this platform.";
  return false;
}
bool removeScanFirewallRule(std::string& warning) {
  warning.clear();
  return true;
}
}  // namespace inventatory

#endif
