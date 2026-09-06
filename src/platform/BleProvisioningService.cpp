// Inventatory Scan R1 BLE discovery and encrypted first-use provisioning.

#include "platform/BleProvisioningService.h"

#include <algorithm>

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

#include <array>
#include <chrono>
#include <cctype>
#include <optional>
#include <thread>
#include <vector>
#endif

namespace inventatory {

using namespace std;

namespace {

constexpr wchar_t kServiceUuidText[] = L"d4d4f5b0-4c21-4a7e-a1a1-4b0db2d00001";
constexpr wchar_t kStatusUuidText[] = L"d4d4f5b0-4c21-4a7e-a1a1-4b0db2d00002";
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

#ifdef _WIN32
class WinrtApartment final {
 public:
  WinrtApartment() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    active_ = true;
  }
  ~WinrtApartment() {
    if (active_) winrt::uninit_apartment();
  }
  WinrtApartment(const WinrtApartment&) = delete;
  WinrtApartment& operator=(const WinrtApartment&) = delete;
 private:
  bool active_ = false;
};
#endif

}  // namespace

BleProvisioningService::BleProvisioningService() = default;
BleProvisioningService::~BleProvisioningService() { stopDiscovery(); }

void BleProvisioningService::startDiscovery() {
  // A watcher can finish on its own after a Windows BLE error. Reap that
  // finished worker before starting a new one; assigning over a joinable
  // std::thread terminates the process.
  stopDiscovery();
  lock_guard<mutex> lock(mutex_);
  devices_.clear();
  discovering_ = true;
  worker_ = thread(&BleProvisioningService::discoveryLoop, this);
}

void BleProvisioningService::stopDiscovery() {
  {
    lock_guard<mutex> lock(mutex_);
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
    WinrtApartment apartment;
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
  } catch (...) {
    lock_guard<mutex> lock(mutex_);
    discovering_ = false;
  }
#endif
}

BleProvisioningOutcome BleProvisioningService::provision(const BleProvisioningRequest& request,
                                                          string& publicError) const {
  publicError.clear();
  if (!validAsciiWifiText(request.wifiSsid, 32) || request.wifiSsid.empty() ||
      !validAsciiWifiText(request.wifiPassword, 63) || !validToken(request.deviceToken) ||
      !validPairingCode(request.pairingCode)) {
    publicError = "Enter a Wi-Fi name, valid pairing code, and generated device token";
    return BleProvisioningOutcome::Failed;
  }
#ifdef _WIN32
  try {
    WinrtApartment apartment;
    using namespace winrt::Windows::Devices::Bluetooth;
    using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
    using namespace winrt::Windows::Devices::Enumeration;
    using namespace winrt::Windows::Storage::Streams;

    auto device = BluetoothLEDevice::FromBluetoothAddressAsync(request.address).get();
    if (!device) {
      publicError = "Scanner is no longer nearby";
      return BleProvisioningOutcome::Failed;
    }
    auto info = DeviceInformation::CreateFromIdAsync(device.DeviceId()).get();
    if (!info) {
      publicError = "Windows could not open the scanner";
      return BleProvisioningOutcome::Failed;
    }
    const auto pairDevice = [&](DeviceInformation& pairingInfo) {
      if (pairingInfo.Pairing().IsPaired()) {
        return pairingInfo.Pairing().ProtectionLevel() == DevicePairingProtectionLevel::EncryptionAndAuthentication;
      }
      const auto pairing = pairingInfo.Pairing().Custom();
      const auto handler = pairing.PairingRequested([pin = request.pairingCode](const auto&, const DevicePairingRequestedEventArgs& args) {
        if (args.PairingKind() == DevicePairingKinds::ProvidePin) {
          args.Accept(winrt::to_hstring(pin));
        }
        // ConfirmOnly is deliberately not auto-accepted.  Windows must show
        // its physical/user confirmation prompt, or pairing fails closed if
        // that prompt cannot be presented.  Accepting here would allow a
        // nearby device to be paired without the user confirming its identity.
      });
      const auto kinds = static_cast<DevicePairingKinds>(static_cast<unsigned>(DevicePairingKinds::ProvidePin) |
                                                         static_cast<unsigned>(DevicePairingKinds::ConfirmOnly));
      const auto result = pairing.PairAsync(kinds, DevicePairingProtectionLevel::EncryptionAndAuthentication).get();
      pairing.PairingRequested(handler);
      return result.Status() == DevicePairingResultStatus::Paired ||
             result.Status() == DevicePairingResultStatus::AlreadyPaired;
    };
    if (!pairDevice(info)) {
      publicError = "Secure Bluetooth pairing was not completed";
      return BleProvisioningOutcome::Failed;
    }

    // Pairing can replace the cached GATT object, and Windows may retain a
    // stale object even when the device was already paired. Always reopen it
    // before service discovery, then retry briefly while the authenticated
    // connection settles.
    device = BluetoothLEDevice::FromBluetoothAddressAsync(request.address).get();
    if (!device) {
      publicError = "Windows could not reopen the paired scanner";
      return BleProvisioningOutcome::Failed;
    }

    const auto serviceUuid = winrt::guid{kServiceUuidText};
    const auto statusUuid = winrt::guid{kStatusUuidText};
    const auto requestUuid = winrt::guid{kRequestUuidText};
    using GattServicesResult = decltype(device.GetGattServicesForUuidAsync(serviceUuid, BluetoothCacheMode::Uncached).get());
    const auto discoverServices = [&]() -> optional<GattServicesResult> {
      optional<GattServicesResult> discovered;
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
          this_thread::sleep_for(chrono::milliseconds(500));
          device = BluetoothLEDevice::FromBluetoothAddressAsync(request.address).get();
          if (!device) break;
        }
        auto candidate = device.GetGattServicesForUuidAsync(serviceUuid, BluetoothCacheMode::Uncached).get();
        if (candidate.Status() == GattCommunicationStatus::Success && candidate.Services().Size() > 0) {
          discovered = move(candidate);
          break;
        }
      }
      return discovered;
    };
    auto services = discoverServices();
    if (!services && info.Pairing().IsPaired()) {
      // A full firmware erase removes the ESP32 bond, but it does not remove
      // the old Windows bond. Re-pair once when the authenticated GATT
      // service cannot be discovered; otherwise every reflashed device stays
      // permanently unavailable to the setup wizard.
      try {
        info.Pairing().UnpairAsync().get();
        this_thread::sleep_for(chrono::milliseconds(500));
        device = BluetoothLEDevice::FromBluetoothAddressAsync(request.address).get();
        DeviceInformation refreshedInfo{nullptr};
        if (device) refreshedInfo = DeviceInformation::CreateFromIdAsync(device.DeviceId()).get();
        if (refreshedInfo && pairDevice(refreshedInfo)) {
          device = BluetoothLEDevice::FromBluetoothAddressAsync(request.address).get();
          if (device) services = discoverServices();
        }
      } catch (...) {
        // Keep the original discovery error if Windows refuses to refresh its
        // pairing record; the caller can still remove the entry manually.
      }
    }
    if (!services) {
      publicError = "Scanner setup service is unavailable";
      return BleProvisioningOutcome::Failed;
    }
    const auto service = services->Services().GetAt(0);
    const auto statuses = service.GetCharacteristicsForUuidAsync(statusUuid, BluetoothCacheMode::Uncached).get();
    if (statuses.Status() != GattCommunicationStatus::Success || statuses.Characteristics().Size() == 0) {
      publicError = "Scanner setup confirmation channel is unavailable";
      return BleProvisioningOutcome::Failed;
    }
    const auto characteristics = service.GetCharacteristicsForUuidAsync(requestUuid, BluetoothCacheMode::Uncached).get();
    if (characteristics.Status() != GattCommunicationStatus::Success || characteristics.Characteristics().Size() == 0) {
      publicError = "Scanner secure setup channel is unavailable";
      return BleProvisioningOutcome::Failed;
    }

    const auto statusCharacteristic = statuses.Characteristics().GetAt(0);
    const auto requestCharacteristic = characteristics.Characteristics().GetAt(0);
    const auto notificationStatus = statusCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
        GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
    if (notificationStatus != GattCommunicationStatus::Success) {
      publicError = "Scanner setup confirmations could not be enabled";
      return BleProvisioningOutcome::Failed;
    }

    mutex statusMutex;
    condition_variable statusChanged;
    string latestStatus;
    bool sawConnecting = false;
    bool terminal = false;
    bool terminalSuccess = false;
    bool handlerAttached = false;
    const auto statusToken = statusCharacteristic.ValueChanged(
        [&](const auto&, const GattValueChangedEventArgs& args) {
          try {
            const auto buffer = args.CharacteristicValue();
            DataReader reader = DataReader::FromBuffer(buffer);
            vector<uint8_t> bytes(reader.UnconsumedBufferLength());
            reader.ReadBytes(bytes);
            const string status(bytes.begin(), bytes.end());
            {
              lock_guard<mutex> lock(statusMutex);
              latestStatus = status;
              if (status == "CONNECTING") sawConnecting = true;
              if (status.rfind("SUCCESS ", 0) == 0) {
                terminal = true;
                terminalSuccess = true;
              } else if (status.rfind("FAILED ", 0) == 0) {
                terminal = true;
                terminalSuccess = false;
              }
            }
            statusChanged.notify_one();
          } catch (...) {
            // A malformed notification is not proof that provisioning failed.
            // The caller will classify the result as indeterminate if the
            // authenticated write reached the device.
          }
        });
    handlerAttached = true;

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
    auto writeStatus = GattCommunicationStatus::Unreachable;
    bool writeThrew = false;
    try {
      writeStatus = requestCharacteristic.WriteValueAsync(writer.DetachBuffer(),
                                                          GattWriteOption::WriteWithResponse).get();
    } catch (...) {
      writeThrew = true;
    }
    fill(payload.begin(), payload.end(), 0);

    bool resultWasTerminal = false;
    bool resultWasSuccess = false;
    bool resultSawConnecting = false;
    {
      unique_lock<mutex> lock(statusMutex);
      statusChanged.wait_for(lock, chrono::seconds(25), [&] { return terminal; });
      resultWasTerminal = terminal;
      resultWasSuccess = terminalSuccess;
      resultSawConnecting = sawConnecting;
      (void)latestStatus;
    }

    if (handlerAttached) {
      try {
        statusCharacteristic.ValueChanged(statusToken);
      } catch (...) {
        // The handler is best-effort cleanup; the terminal status was already
        // copied under the mutex above.
      }
      try {
        statusCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue::None).get();
      } catch (...) {
        // The result has already been captured; cleanup failure must not turn
        // a confirmed provisioning result into a false failure.
      }
    }

    if (resultWasTerminal) {
      if (resultWasSuccess) return BleProvisioningOutcome::Succeeded;
      publicError = "The scanner rejected the Wi-Fi setup request";
      return BleProvisioningOutcome::Failed;
    }
    if (!writeThrew && writeStatus == GattCommunicationStatus::Success) {
      publicError = "Scanner setup was sent, but the scanner did not confirm it before Bluetooth closed";
      return BleProvisioningOutcome::Indeterminate;
    }
    if (resultSawConnecting) {
      publicError = "Scanner setup may have been accepted, but Bluetooth did not confirm the final result";
      return BleProvisioningOutcome::Indeterminate;
    }
    publicError = "Encrypted setup request was rejected";
    return BleProvisioningOutcome::Failed;
  } catch (...) {
    publicError = "Bluetooth setup failed; keep the scanner nearby and retry";
    return BleProvisioningOutcome::Failed;
  }
#else
  publicError = "Bluetooth setup is supported on Windows only";
  return BleProvisioningOutcome::Failed;
#endif
}

}  // namespace inventatory
