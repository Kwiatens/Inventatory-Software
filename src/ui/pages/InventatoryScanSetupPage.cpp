// Inventatory - Hardware Inventory Management System
// Simple pairing and status page for one Inventatory Scan R1 device.

#include "App.h"

#include "platform/CredentialStore.h"
#include "platform/UpdateService.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <string>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kDebugWindowLines = 14;
constexpr const char* kInventatoryScanTokenCredential = "inventatory-scan-pairing-token";

}  // namespace

bool App::regenerateInventatoryScanToken() {
  const auto now = time(nullptr);
  if (settingsConfirmAction_ != "regenerate-token" || now > settingsConfirmUntil_) {
    settingsConfirmAction_ = "regenerate-token";
    settingsConfirmUntil_ = now + 5;
    setMessage("Regenerating invalidates the current token; activate again within 5 seconds to confirm", 5);
    return false;
  }
  settingsConfirmAction_.clear();
  settingsConfirmUntil_ = 0;
  const auto previousConfig = inventatoryScanConfig_;
  const bool previousCredentialPending = scannerCredentialSavePending_;
  const bool previousConfigPending = scannerConfigSavePending_;
  const auto previousPersistenceError = persistenceError_;
  const bool serviceWasRunning = server_.running();
  mdnsService_.stop();
  server_.stop();
  inventatoryScanConfig_.token = generateInventatoryScanToken();
  inventatoryScanConfig_.deviceId.clear();
  inventatoryScanConfig_.setupComplete = false;
  if (!saveScannerConfigChecked(true)) {
    inventatoryScanConfig_ = previousConfig;
    scannerCredentialSavePending_ = previousCredentialPending;
    scannerConfigSavePending_ = previousConfigPending;
    persistenceError_ = previousPersistenceError;
    if (serviceWasRunning) restartDeviceService();
    return false;
  }
  if (!saveScannerCredentialChecked(true)) {
    setMessage("Generated a new token, but it could not be stored securely; pair again after saving succeeds", 7);
    return false;
  }
  deviceLastSeen_ = 0;
  deviceFirmwareVersion_.clear();
  deviceRssi_ = 0;
  deviceDebug_.clear();
  deviceLastResult_.clear();
  deviceProtocolVersion_ = 0;
  deviceMode_.clear();
  devicePendingEventCount_ = 0;
  deviceLastSync_ = 0;
  deviceRequestCache_.clear();
  deviceRequestOrder_.clear();
  clearQuickLabelPrintCache();
  error_code replayError;
  filesystem::remove(inventatoryScanReplayStatePath(dataPath_), replayError);
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                               inventatoryScanReplayStatePath(dataPath_));
  setMessage("Generated a new pairing token; pair the scanner again", 5);
  dirty_ = true;
  return true;
}

bool App::clearInventatoryScanPairing() {
  if (trim(inventatoryScanConfig_.deviceId).empty() && !inventatoryScanConfig_.setupComplete) {
    setMessage("No paired device to clear", 2);
    return false;
  }

  const auto now = time(nullptr);
  if (settingsConfirmAction_ != "clear-device" || now > settingsConfirmUntil_) {
    settingsConfirmAction_ = "clear-device";
    settingsConfirmUntil_ = now + 5;
    setMessage("Clearing removes the paired device identity; activate again within 5 seconds to confirm", 5);
    return false;
  }
  settingsConfirmAction_.clear();
  settingsConfirmUntil_ = 0;

  const auto rotatedToken = generateInventatoryScanToken();
  const auto previousConfig = inventatoryScanConfig_;
  const bool previousCredentialPending = scannerCredentialSavePending_;
  const bool previousConfigPending = scannerConfigSavePending_;
  const auto previousPersistenceError = persistenceError_;
  const bool serviceWasRunning = server_.running();
  mdnsService_.stop();
  server_.stop();
  inventatoryScanConfig_.token = rotatedToken;
  inventatoryScanConfig_.deviceId.clear();
  inventatoryScanConfig_.setupComplete = false;
  if (!saveScannerConfigChecked(true)) {
    inventatoryScanConfig_ = previousConfig;
    scannerCredentialSavePending_ = previousCredentialPending;
    scannerConfigSavePending_ = previousConfigPending;
    persistenceError_ = previousPersistenceError;
    if (serviceWasRunning) restartDeviceService();
    return false;
  }
  if (!saveScannerCredentialChecked(true)) {
    setMessage("Cleared pairing, but the replacement token could not be stored securely", 7);
    return false;
  }
  deviceLastSeen_ = 0;
  deviceFirmwareVersion_.clear();
  deviceRssi_ = 0;
  deviceDebug_.clear();
  deviceLastResult_.clear();
  deviceProtocolVersion_ = 0;
  deviceMode_.clear();
  devicePendingEventCount_ = 0;
  deviceLastSync_ = 0;
  deviceRequestCache_.clear();
  deviceRequestOrder_.clear();
  clearQuickLabelPrintCache();
  error_code replayError;
  filesystem::remove(inventatoryScanReplayStatePath(dataPath_), replayError);
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                               inventatoryScanReplayStatePath(dataPath_));
  setMessage("Cleared paired device identity; pair the scanner again", 5);
  dirty_ = true;
  return true;
}

bool App::copyInventatoryScanToken() {
  const auto token = trim(inventatoryScanConfig_.token);
  if (token.empty()) {
    setMessage("No pairing token to copy", 2);
    return false;
  }

  if (!copyToClipboard(token)) {
    setMessage("Unable to copy the pairing token", 3);
    return false;
  }

  setMessage("Copied the pairing token to the clipboard", 3);
  return true;
}

void App::beginScanFirmwareCheck() {
  if (scanFirmwareFuture_.valid()) {
    setMessage("Already checking for Scan R1 firmware updates", 3);
    return;
  }
  scanFirmwareChecked_ = false;
  scanFirmwareCheckFailed_ = false;
  scanFirmwareFuture_ = async(launch::async, [installed = deviceFirmwareVersion_] {
    return checkLatestScanFirmwareRelease(installed);
  });
  setMessage("Checking for Scan R1 firmware updates", 4);
  dirty_ = true;
}

void App::processScanFirmwareCheck() {
  if (!scanFirmwareFuture_.valid() || scanFirmwareFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
  const auto result = scanFirmwareFuture_.get();
  scanFirmwareChecked_ = true;
  scanFirmwareCheckFailed_ = !result.completed;
  scanFirmwareLatestVersion_ = result.completed ? result.latestVersion : string();
  if (!result.completed) {
    setMessage("Could not reach the firmware release channel", 4);
  } else if (result.updateAvailable) {
    setMessage("Scan R1 firmware " + result.latestVersion + " is available", 6);
  } else if (deviceFirmwareVersion_.empty()) {
    setMessage("Firmware channel checked; connect a scanner to compare versions", 5);
  } else {
    setMessage("Scan R1 firmware is up to date", 4);
  }
  dirty_ = true;
}

// One short line for the Scan settings panel: the device's own version first,
// then the result of the last release check, so the row reads as a value.
string App::scanFirmwareStatus() const {
  const bool installedKnown = !deviceFirmwareVersion_.empty();
  const auto installed = installedKnown ? deviceFirmwareVersion_ : string("Not reported");
  if (scanFirmwareFuture_.valid()) return installed + "  \xC2\xB7  checking " + uiLoadingSpinner();
  if (!installedKnown) {
    if (scanFirmwareCheckFailed_) return installed + "  \xC2\xB7  check failed";
    return "Pair a scanner to compare firmware";
  }
  if (!scanFirmwareChecked_ || scanFirmwareLatestVersion_.empty()) {
    return scanFirmwareCheckFailed_ ? installed + "  \xC2\xB7  check failed" : installed;
  }
  if (isVersionNewer(scanFirmwareLatestVersion_, deviceFirmwareVersion_)) {
    return installed + "  \xC2\xB7  " + scanFirmwareLatestVersion_ + " available";
  }
  return installed + "  \xC2\xB7  up to date";
}

void App::openInventatoryScanSetup() {
  bleProvisioning_.stopDiscovery();
  bleWifiSsid_.clear();
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  blePairingCode_.clear();
  bleSetupSelection_ = 0;
  bleSetupMessage_.clear();
  bleSetupOutcomeUncertain_ = false;
  scanSetupStep_ = ScanSetupStep::Introduction;
  inputBuffer_.clear();
  if (returnToOnboardingAfterScan_) {
    beginWizardTransition(Page::ScanSetup, onboardingStep_, scanSetupStep_, true);
  } else {
    changePage(Page::ScanSetup);
  }
  setMessage("Scan R1 setup wizard started", 3);
}

void App::refreshBleSetupDiscovery() {
  bleProvisioning_.startDiscovery();
  bleSetupSelection_ = 0;
  bleSetupMessage_ = "Searching for nearby unconfigured Scan R1 devices";
  setMessage(bleSetupMessage_, 4);
  dirty_ = true;
}

bool App::provisionSelectedBleSetupDevice() {
  const auto devices = bleProvisioning_.devices();
  if (devices.empty() || bleSetupSelection_ >= devices.size()) {
    setMessage("Select a nearby Scan R1 first", 4);
    return false;
  }
  if (trim(bleWifiSsid_).empty() || blePairingCode_.size() != 6) {
    setMessage("Enter the Wi-Fi name and the six-digit code shown on the R1", 5);
    return false;
  }
  const auto candidateToken = generateInventatoryScanToken();
  BleProvisioningRequest request;
  request.address = devices[bleSetupSelection_].address;
  request.wifiSsid = trim(bleWifiSsid_);
  request.wifiPassword = bleWifiPassword_;
  request.deviceToken = candidateToken;
  request.pairingCode = blePairingCode_;
  string error;
  const auto outcome = bleProvisioning_.provision(request, error);
  if (outcome == BleProvisioningOutcome::Failed) {
    setMessage(error.empty() ? "Bluetooth setup failed" : error, 5);
    return false;
  }
  inventatoryScanConfig_.token = candidateToken;
  inventatoryScanConfig_.deviceId.clear();
  // Persist the unpaired state before replacing the workspace credential. If
  // the final paired-state write fails, a restart remains fail-closed even if
  // Credential Manager has already accepted the new token.
  inventatoryScanConfig_.setupComplete = false;
  const bool unpairedConfigSaved = saveScannerConfigChecked(false);
  const bool tokenSaved = unpairedConfigSaved && saveScannerCredentialChecked(false);
  bool configSaved = false;
  if (tokenSaved) {
    inventatoryScanConfig_.setupComplete = true;
    configSaved = saveScannerConfigChecked(false);
  }
  error_code replayError;
  filesystem::remove(inventatoryScanReplayStatePath(dataPath_), replayError);
  if (tokenSaved && !configSaved) {
    CredentialStore::eraseForWorkspace(dataPath_, kInventatoryScanTokenCredential);
    inventatoryScanConfig_.token.clear();
    inventatoryScanConfig_.deviceId.clear();
    inventatoryScanConfig_.setupComplete = false;
    scannerCredentialSavePending_ = false;
    saveScannerConfigChecked(false);
  }
  if (tokenSaved && configSaved) {
    server_.setDeviceCredentials({}, inventatoryScanConfig_.token,
                                 inventatoryScanReplayStatePath(dataPath_));
    restartDeviceService();
  } else {
    mdnsService_.stop();
    server_.stop();
  }
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  blePairingCode_.clear();
  bleSetupOutcomeUncertain_ = outcome == BleProvisioningOutcome::Indeterminate;
  bleSetupMessage_ = bleSetupOutcomeUncertain_
                         ? "Setup result was not confirmed; the token was retained so the R1 can recover if it accepted it"
                         : "Wi-Fi setup confirmed securely; waiting for the R1 to join the PC service";
  if (!tokenSaved || !configSaved) {
    bleSetupMessage_ = "Scanner setup was sent, but pairing data is not fully saved; press R to retry saving";
    setMessage(bleSetupMessage_, 7);
    dirty_ = true;
    return false;
  }
  setMessage(bleSetupMessage_, 6);
  dirty_ = true;
  return true;
}

void App::handleInventatoryScanSetupKey(const KeyEvent& key) {
  const auto cancel = [this] {
    bleProvisioning_.stopDiscovery();
    if (returnToOnboardingAfterScan_) {
      beginWizardTransition(Page::Onboarding, OnboardingStep::Complete, scanSetupStep_, false);
    } else {
      bleWifiSsid_.clear();
      bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
      bleWifiPassword_.clear();
      blePairingCode_.clear();
      inputBuffer_.clear();
      changePage(Page::Home);
    }
  };

  if (key.type == KeyType::Escape) {
    cancel();
    return;
  }

  if (scanSetupStep_ == ScanSetupStep::WifiName || scanSetupStep_ == ScanSetupStep::WifiPassword ||
      scanSetupStep_ == ScanSetupStep::PairingCode) {
    if (key.type == KeyType::Character) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
      return;
    }
    if (key.type == KeyType::Backspace) {
      if (!inputBuffer_.empty()) inputBuffer_.pop_back();
      dirty_ = true;
      return;
    }
  }

  if (scanSetupStep_ == ScanSetupStep::FindScanner) {
    const auto devices = bleProvisioning_.devices();
    if (key.type == KeyType::Up && bleSetupSelection_ > 0) {
      --bleSetupSelection_;
      dirty_ = true;
      return;
    }
    if (key.type == KeyType::Down && bleSetupSelection_ + 1 < devices.size()) {
      ++bleSetupSelection_;
      dirty_ = true;
      return;
    }
    if (key.type == KeyType::Character && tolower(static_cast<unsigned char>(key.ch)) == 'r') {
      bleProvisioning_.stopDiscovery();
      refreshBleSetupDiscovery();
      return;
    }
  }

  if (key.type != KeyType::Enter) return;
  const auto advanceScanStep = [this](ScanSetupStep next) {
    if (returnToOnboardingAfterScan_) {
      beginWizardTransition(Page::ScanSetup, onboardingStep_, next, true);
    } else {
      scanSetupStep_ = next;
      dirty_ = true;
    }
  };
  switch (scanSetupStep_) {
    case ScanSetupStep::Introduction:
      advanceScanStep(ScanSetupStep::WifiName);
      inputBuffer_.clear();
      break;
    case ScanSetupStep::WifiName:
      bleWifiSsid_ = trim(inputBuffer_);
      if (bleWifiSsid_.empty()) {
        setMessage("Enter a Wi-Fi network name", 3);
        return;
      }
      advanceScanStep(ScanSetupStep::WifiPassword);
      break;
    case ScanSetupStep::WifiPassword:
      bleWifiPassword_ = inputBuffer_;
      advanceScanStep(ScanSetupStep::PairingCode);
      break;
    case ScanSetupStep::PairingCode:
      blePairingCode_ = trim(inputBuffer_);
      if (blePairingCode_.size() != 6 ||
          !all_of(blePairingCode_.begin(), blePairingCode_.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
        setMessage("The R1 verification code must contain exactly six digits", 4);
        return;
      }
      advanceScanStep(ScanSetupStep::FindScanner);
      refreshBleSetupDiscovery();
      break;
    case ScanSetupStep::FindScanner:
      if (bleProvisioning_.devices().empty()) {
        setMessage("Wait for a nearby unconfigured Scan R1, then try again", 4);
        return;
      }
      advanceScanStep(ScanSetupStep::Confirm);
      break;
    case ScanSetupStep::Confirm:
      bleProvisioning_.stopDiscovery();
      if (provisionSelectedBleSetupDevice()) advanceScanStep(ScanSetupStep::Complete);
      break;
    case ScanSetupStep::Complete:
      cancel();
      return;
  }
  dirty_ = true;
}

}  // namespace inventatory
