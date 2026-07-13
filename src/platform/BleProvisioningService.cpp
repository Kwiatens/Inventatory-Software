// Inventatory Scan R1 BLE discovery and encrypted first-use provisioning.

#include "platform/BleProvisioningService.h"

#ifdef _WIN32
#define WINRT_LEAN_AND_MEAN
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <thread>
#include <vector>
#endif

namespace hims {

using namespace std;

namespace {

constexpr wchar_t kServiceUuidText[] = L"d4d4f5b0-4c21-4a7e-a1a1-4b0db2d00001";
constexpr wchar_t kRequestUuidText[] = L"d4d4f5b0-4c21-4a7e-a1a1-4b0db2d00003";

bool validAsciiWifiText(const std::string& value, std::size_t maximum) {
  return value.size() <= maximum &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 0x20 && ch != 0x7f; });
}

bool validToken(const std::string& value) {
  return value.size() >= 32 && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool validPairingCode(const std::string& value) {
  return value.size() == 6 && std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

}  // namespace

BleProvisioningService::BleProvisioningService() = default;
BleProvisioningService::~BleProvisioningService() { stopDiscovery(); }

void BleProvisioningService::startDiscovery() {
  lock_guard<mutex> lock(mutex_);
  if (discovering_) return;
  devices_.clear();
  discovering_ = true;
  worker_ = thread(&BleProvisioningService::discoveryLoop, this);
}

void BleProvisioningService::stopDiscovery() {
  {
    lock_guard<mutex> lock(mutex_);
    if (!discovering_) return;
    discovering_ = false;
  }
  if (worker_.joinable()) worker_.join();
}

vector<BleSetupDevice> BleProvisioningService::devices() const {
  lock_guard<mutex> lock(mutex_);
  return devices_;
}

void BleProvisioningService::rememberDevice(uint64_t address, const string& name, int rssi) {
  lock_guard<mutex> lock(mutex_);
  const auto found = find_if(devices_.begin(), devices_.end(), [address](const BleSetupDevice& device) {
    return device.address == address;
  });
  if (found != devices_.end()) {
    found->name = name;
    found->rssi = rssi;
    return;
  }
  devices_.push_back({address, name, rssi});
}

void BleProvisioningService::discoveryLoop() {
#ifdef _WIN32
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
    const auto serviceUuid = winrt::guid{kServiceUuidText};
    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    const auto token = watcher.Received([this, serviceUuid](const auto&, const BluetoothLEAdvertisementReceivedEventArgs& args) {
      const auto& advertised = args.Advertisement().ServiceUuids();
      const bool matches = any_of(advertised.begin(), advertised.end(), [serviceUuid](const auto& uuid) {
        return uuid == serviceUuid;
      });
      if (!matches) return;
      const auto localName = winrt::to_string(args.Advertisement().LocalName());
      rememberDevice(args.BluetoothAddress(), localName.empty() ? "Inventatory Scan R1" : localName,
                     args.RawSignalStrengthInDBm());
    });
    watcher.Start();
    for (;;) {
      {
        lock_guard<mutex> lock(mutex_);
        if (!discovering_) break;
      }
      this_thread::sleep_for(chrono::milliseconds(200));
    }
    watcher.Stop();
    watcher.Received(token);
    winrt::uninit_apartment();
  } catch (...) {
    lock_guard<mutex> lock(mutex_);
    discovering_ = false;
  }
#endif
}

bool BleProvisioningService::provision(const BleProvisioningRequest& request, string& publicError) const {
  publicError.clear();
  if (!validAsciiWifiText(request.wifiSsid, 32) || request.wifiSsid.empty() ||
      !validAsciiWifiText(request.wifiPassword, 63) || !validToken(request.deviceToken) ||
      !validPairingCode(request.pairingCode)) {
    publicError = "Enter a Wi-Fi name, valid pairing code, and generated device token";
    return false;
  }
#ifdef _WIN32
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    using namespace winrt::Windows::Devices::Bluetooth;
    using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
    using namespace winrt::Windows::Devices::Enumeration;
    using namespace winrt::Windows::Storage::Streams;

    const auto device = BluetoothLEDevice::FromBluetoothAddressAsync(request.address).get();
    if (!device) {
      publicError = "Scanner is no longer nearby";
      return false;
    }
    const auto info = DeviceInformation::CreateFromIdAsync(device.DeviceId()).get();
    if (!info) {
      publicError = "Windows could not open the scanner";
      return false;
    }
    if (!info.Pairing().IsPaired()) {
      const auto pairing = info.Pairing().Custom();
      const auto handler = pairing.PairingRequested([pin = request.pairingCode](const auto&, const DevicePairingRequestedEventArgs& args) {
        if (args.PairingKind() == DevicePairingKinds::ProvidePin) args.Accept(winrt::to_hstring(pin));
        else args.Accept();
      });
      const auto kinds = static_cast<DevicePairingKinds>(static_cast<unsigned>(DevicePairingKinds::ProvidePin) |
                                                         static_cast<unsigned>(DevicePairingKinds::ConfirmOnly));
      const auto result = pairing.PairAsync(kinds, DevicePairingProtectionLevel::EncryptionAndAuthentication).get();
      pairing.PairingRequested(handler);
      if (result.Status() != DevicePairingResultStatus::Paired && result.Status() != DevicePairingResultStatus::AlreadyPaired) {
        publicError = "Secure Bluetooth pairing was not completed";
        return false;
      }
    }

    const auto serviceUuid = winrt::guid{kServiceUuidText};
    const auto requestUuid = winrt::guid{kRequestUuidText};
    const auto services = device.GetGattServicesForUuidAsync(serviceUuid, BluetoothCacheMode::Uncached).get();
    if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0) {
      publicError = "Scanner setup service is unavailable";
      return false;
    }
    const auto service = services.Services().GetAt(0);
    const auto characteristics = service.GetCharacteristicsForUuidAsync(requestUuid, BluetoothCacheMode::Uncached).get();
    if (characteristics.Status() != GattCommunicationStatus::Success || characteristics.Characteristics().Size() == 0) {
      publicError = "Scanner secure setup channel is unavailable";
      return false;
    }

    vector<uint8_t> payload;
    payload.reserve(4 + request.wifiSsid.size() + request.wifiPassword.size() + request.deviceToken.size());
    payload.push_back(1);
    payload.push_back(static_cast<uint8_t>(request.wifiSsid.size()));
    payload.push_back(static_cast<uint8_t>(request.wifiPassword.size()));
    payload.push_back(static_cast<uint8_t>(request.deviceToken.size()));
    payload.insert(payload.end(), request.wifiSsid.begin(), request.wifiSsid.end());
    payload.insert(payload.end(), request.wifiPassword.begin(), request.wifiPassword.end());
    payload.insert(payload.end(), request.deviceToken.begin(), request.deviceToken.end());
    DataWriter writer;
    writer.WriteBytes(payload);
    const auto status = characteristics.Characteristics().GetAt(0).WriteValueAsync(writer.DetachBuffer(),
                                                                                      GattWriteOption::WriteWithResponse).get();
    fill(payload.begin(), payload.end(), 0);
    if (status != GattCommunicationStatus::Success) {
      publicError = "Encrypted setup request was rejected";
      return false;
    }
    return true;
  } catch (...) {
    publicError = "Bluetooth setup failed; keep the scanner nearby and retry";
    return false;
  }
#else
  publicError = "Bluetooth setup is supported on Windows only";
  return false;
#endif
}

}  // namespace hims
