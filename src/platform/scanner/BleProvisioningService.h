// Inventatory Scan R1 BLE discovery and encrypted first-use provisioning.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace inventatory {

struct BleSetupDevice {
  std::uint64_t address = 0;
  std::string name;
  int rssi = 0;
};

struct BleProvisioningRequest {
  std::uint64_t address = 0;
  std::string wifiSsid;
  std::string wifiPassword;
  std::string deviceToken;
  std::string pairingCode;
};

enum class BleProvisioningOutcome {
  Failed,
  Succeeded,
  Indeterminate,
};

// Windows BLE transport. Credentials are passed only to an authenticated,
// encrypted GATT characteristic and never written to files or diagnostic logs.
class BleProvisioningService {
 public:
  BleProvisioningService();
  ~BleProvisioningService();
  BleProvisioningService(const BleProvisioningService&) = delete;
  BleProvisioningService& operator=(const BleProvisioningService&) = delete;

  void startDiscovery();
  void stopDiscovery();
  std::vector<BleSetupDevice> devices() const;
  BleProvisioningOutcome provision(const BleProvisioningRequest& request, std::string& publicError) const;

 private:
  void discoveryLoop();
  void rememberDevice(std::uint64_t address, const std::string& name, int rssi);

  mutable std::mutex mutex_;
  std::vector<BleSetupDevice> devices_;
  std::thread worker_;
  bool discovering_ = false;
};

}  // namespace inventatory
