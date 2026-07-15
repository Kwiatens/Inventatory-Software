// Inventatory - Hardware Inventory Management System
// Simple pairing and status page for one Inventatory Scan R1 device.

#include "App.h"

#include "platform/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
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
  inventatoryScanConfig_.token = generateInventatoryScanToken();
  if (!CredentialStore::write(kInventatoryScanTokenCredential, inventatoryScanConfig_.token)) {
    setMessage("Unable to save the new pairing token securely", 4);
    return false;
  }
  inventatoryScanConfig_.deviceId.clear();
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
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token);
  if (!saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_)) {
    setMessage("Generated a new token, but Inventatory could not save it", 4);
    return false;
  }
  setMessage("Generated a new pairing token", 3);
  dirty_ = true;
  return true;
}

bool App::clearInventatoryScanPairing() {
  if (trim(inventatoryScanConfig_.deviceId).empty()) {
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

  inventatoryScanConfig_.deviceId.clear();
  deviceLastSeen_ = 0;
  deviceFirmwareVersion_.clear();
  deviceRssi_ = 0;
  deviceDebug_.clear();
  deviceLastResult_.clear();
  deviceProtocolVersion_ = 0;
  deviceMode_.clear();
  devicePendingEventCount_ = 0;
  deviceLastSync_ = 0;
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token);
  if (!saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_)) {
    setMessage("Cleared pairing in memory, but Inventatory could not save it", 4);
    return false;
  }
  setMessage("Cleared paired device identity", 3);
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

void App::openInventatoryScanSetup() {
  bleProvisioning_.stopDiscovery();
  bleWifiSsid_.clear();
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  blePairingCode_.clear();
  bleSetupSelection_ = 0;
  bleSetupMessage_.clear();
  scanSetupStep_ = ScanSetupStep::Introduction;
  inputBuffer_.clear();
  changePage(Page::ScanSetup);
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
  if (!bleProvisioning_.provision(request, error)) {
    setMessage(error.empty() ? "Bluetooth setup failed" : error, 5);
    return false;
  }
  if (!CredentialStore::write(kInventatoryScanTokenCredential, candidateToken)) {
    setMessage("Scanner accepted setup, but Inventatory could not save its token securely", 6);
    return false;
  }
  inventatoryScanConfig_.token = candidateToken;
  inventatoryScanConfig_.deviceId.clear();
  server_.setDeviceCredentials({}, inventatoryScanConfig_.token);
  if (!saveInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_)) {
    setMessage("Scanner setup was sent, but Inventatory could not save pairing metadata", 6);
    return false;
  }
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  blePairingCode_.clear();
  bleSetupMessage_ = "Wi-Fi setup sent securely; waiting for the R1 to join the PC service";
  setMessage(bleSetupMessage_, 6);
  dirty_ = true;
  return true;
}

ftxui::Element App::renderInventatoryScanSetupUi() const {
  const bool onboardingTerminal = returnToOnboardingAfterScan_;
  const auto setupBackground = onboardingTerminal ? uiCanvasBg() : uiPanelLeftBg();
  const auto stepNumber = [&] {
    switch (scanSetupStep_) {
      case ScanSetupStep::Introduction: return string("0 / 5");
      case ScanSetupStep::WifiName: return string("1 / 5");
      case ScanSetupStep::WifiPassword: return string("2 / 5");
      case ScanSetupStep::PairingCode: return string("3 / 5");
      case ScanSetupStep::FindScanner: return string("4 / 5");
      case ScanSetupStep::Confirm: return string("5 / 5");
      case ScanSetupStep::Complete: return string("complete");
    }
    return string();
  };
  const auto progressMark = [this](ScanSetupStep step) {
    const auto active = static_cast<int>(scanSetupStep_);
    const auto candidate = static_cast<int>(step);
    return active > candidate ? string("[x]") : active == candidate ? string("[>]") : string("[ ]");
  };

  ftxui::Elements rows;
  rows.push_back(styledText("$ inventatory setup scan-r1", uiSuccessColor()) | ftxui::bold);
  rows.push_back(styledText("Bluetooth first-use provisioning  |  Step " + stepNumber(), uiMutedText()));
  rows.push_back(uiDivider());
  rows.push_back(fullLine(progressMark(ScanSetupStep::WifiName) + " Wi-Fi network   " +
                          progressMark(ScanSetupStep::WifiPassword) + " Wi-Fi password   " +
                          progressMark(ScanSetupStep::PairingCode) + " R1 verification code   " +
                          progressMark(ScanSetupStep::FindScanner) + " Find scanner   " +
                          progressMark(ScanSetupStep::Confirm) + " Secure transfer",
                          uiMutedColor(), setupBackground));
  rows.push_back(uiDivider());

  switch (scanSetupStep_) {
    case ScanSetupStep::Introduction:
      rows.push_back(styledText("Welcome. This assistant connects an unconfigured Scan R1 without editing files.", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Before continuing:", uiSecondaryText()));
      rows.push_back(styledText("  1. Power on the R1; it must show a six-digit BLE code.", uiTitleColor()));
      rows.push_back(styledText("  2. Keep Bluetooth enabled on this PC.", uiTitleColor()));
      rows.push_back(styledText("  3. Have the Wi-Fi name and password ready.", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("> Press Enter to begin  |  Esc to cancel", uiLinkColor()));
      break;
    case ScanSetupStep::WifiName:
      rows.push_back(styledText("[1/5] Wi-Fi network", uiAccentColor()));
      rows.push_back(styledText("Enter the Wi-Fi SSID the Scan R1 should join.", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("network> " + inputBuffer_ + "_", uiSuccessColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Enter continues  |  Esc cancels", uiMutedText()));
      break;
    case ScanSetupStep::WifiPassword:
      rows.push_back(styledText("[2/5] Wi-Fi password", uiAccentColor()));
      rows.push_back(styledText("Enter the password. It is masked, transmitted only over encrypted BLE, and cleared after setup.",
                                uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("password> " + string(inputBuffer_.size(), '*') + "_", uiSuccessColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Enter continues  |  Esc cancels", uiMutedText()));
      break;
    case ScanSetupStep::PairingCode:
      rows.push_back(styledText("[3/5] Verify the physical scanner", uiAccentColor()));
      rows.push_back(styledText("Type the six-digit code currently shown on the R1 display.", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("r1-code> " + inputBuffer_ + "_", uiSuccessColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("This proves you are pairing with the scanner in front of you.", uiMutedText()));
      break;
    case ScanSetupStep::FindScanner: {
      const auto devices = bleProvisioning_.devices();
      rows.push_back(styledText("[4/5] Find Scan R1", uiAccentColor()));
      rows.push_back(styledText("Searching for nearby R1 devices advertising the setup service...", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      if (devices.empty()) {
        rows.push_back(styledText("bluetooth> waiting for an unconfigured Scan R1", uiWarnColor()));
      } else {
        for (size_t index = 0; index < devices.size(); ++index) {
          const auto& device = devices[index];
          rows.push_back(styledText(string(index == bleSetupSelection_ ? "> " : "  ") + device.name + "  " +
                                    to_string(device.rssi) + " dBm",
                                    index == bleSetupSelection_ ? uiFocusColor() : uiTitleColor()));
        }
      }
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Up/Down selects  |  R refreshes search  |  Enter continues", uiMutedText()));
      break;
    }
    case ScanSetupStep::Confirm: {
      const auto devices = bleProvisioning_.devices();
      const auto scanner = bleSetupSelection_ < devices.size() ? devices[bleSetupSelection_].name : string("No scanner selected");
      rows.push_back(styledText("[5/5] Review and provision", uiAccentColor()));
      rows.push_back(styledText("Scanner: " + scanner, uiTitleColor()));
      rows.push_back(styledText("Wi-Fi network: " + bleWifiSsid_, uiTitleColor()));
      rows.push_back(styledText("Wi-Fi password: " + string(bleWifiPassword_.empty() ? 0 : 12, '*'), uiTitleColor()));
      rows.push_back(styledText("Verification code: " + blePairingCode_, uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("> Press Enter to securely transfer this configuration", uiSuccessColor()));
      rows.push_back(styledText("  Esc cancels without changing the scanner.", uiMutedText()));
      break;
    }
    case ScanSetupStep::Complete:
      rows.push_back(styledText("Setup request accepted.", uiSuccessColor()) | ftxui::bold);
      rows.push_back(styledText("The Scan R1 is joining Wi-Fi and will connect to this Inventatory PC automatically.", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("The device token was stored in Windows Credential Manager; the Wi-Fi password was cleared from Inventatory.",
                                uiMutedText()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("> Press Enter or Esc to return to Home", uiLinkColor()));
      break;
  }

  auto body = ftxui::vbox(move(rows)) | ftxui::bgcolor(setupBackground) | ftxui::flex;
  if (onboardingTerminal) return body | ftxui::bgcolor(uiCanvasBg());
  return ftxui::window(styledText(" Scan R1 Setup Wizard ", uiAccentColor()), body) | ftxui::bgcolor(uiCanvasBg());
}

ftxui::Element App::renderDeviceDebugConsoleUi() const {
  ftxui::Elements lines;
  const auto total = deviceDebugLog_.size();
  const auto visible = min(kDebugWindowLines, total == 0 ? size_t(1) : total);
  const auto maxScroll = total > visible ? total - visible : 0;
  const auto start = min(deviceDebugScroll_, maxScroll);
  const auto end = min(start + visible, total);

  lines.push_back(fullLine("Wi-Fi debug console", uiAccentColor(), uiPanelLeftBg()));
  lines.push_back(fullLine("Up/Down scroll  PageUp/PageDown faster  Home/End jump  focus stays on pairing page",
                           uiMutedColor(), uiPanelLeftBg()));
  if (total == 0) {
    lines.push_back(fullLine("[waiting for device log messages]", uiMutedColor(), uiPanelLeftBg()));
  } else {
    for (size_t index = start; index < end; ++index) {
      const auto& line = deviceDebugLog_[index];
      lines.push_back(fullLine(line, uiTitleColor(), index % 2 == 0 ? uiRowDarkBg() : uiRowLightBg()));
    }
  }

  return ftxui::window(styledText(" Wi-Fi terminal ", uiAccentColor()),
                       ftxui::vbox(move(lines)) | ftxui::yframe | ftxui::vscroll_indicator) |
         ftxui::bgcolor(uiPanelLeftBg());
}

void App::handleInventatoryScanSetupKey(const KeyEvent& key) {
  const auto cancel = [this] {
    bleProvisioning_.stopDiscovery();
    bleWifiSsid_.clear();
    bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
    bleWifiPassword_.clear();
    blePairingCode_.clear();
    inputBuffer_.clear();
    if (returnToOnboardingAfterScan_) {
      returnToOnboardingAfterScan_ = false;
      onboardingStep_ = OnboardingStep::Complete;
      changePage(Page::Onboarding);
    } else {
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
  switch (scanSetupStep_) {
    case ScanSetupStep::Introduction:
      scanSetupStep_ = ScanSetupStep::WifiName;
      inputBuffer_.clear();
      break;
    case ScanSetupStep::WifiName:
      bleWifiSsid_ = trim(inputBuffer_);
      if (bleWifiSsid_.empty()) {
        setMessage("Enter a Wi-Fi network name", 3);
        return;
      }
      inputBuffer_.clear();
      scanSetupStep_ = ScanSetupStep::WifiPassword;
      break;
    case ScanSetupStep::WifiPassword:
      bleWifiPassword_ = inputBuffer_;
      inputBuffer_.clear();
      scanSetupStep_ = ScanSetupStep::PairingCode;
      break;
    case ScanSetupStep::PairingCode:
      blePairingCode_ = trim(inputBuffer_);
      if (blePairingCode_.size() != 6 ||
          !all_of(blePairingCode_.begin(), blePairingCode_.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
        setMessage("The R1 verification code must contain exactly six digits", 4);
        return;
      }
      inputBuffer_.clear();
      scanSetupStep_ = ScanSetupStep::FindScanner;
      refreshBleSetupDiscovery();
      break;
    case ScanSetupStep::FindScanner:
      if (bleProvisioning_.devices().empty()) {
        setMessage("Wait for a nearby unconfigured Scan R1, then try again", 4);
        return;
      }
      scanSetupStep_ = ScanSetupStep::Confirm;
      break;
    case ScanSetupStep::Confirm:
      bleProvisioning_.stopDiscovery();
      if (provisionSelectedBleSetupDevice()) scanSetupStep_ = ScanSetupStep::Complete;
      break;
    case ScanSetupStep::Complete:
      cancel();
      return;
  }
  dirty_ = true;
}

}  // namespace inventatory
