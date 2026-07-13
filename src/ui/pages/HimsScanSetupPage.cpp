// HIMS - Hardware Inventory Management System
// Simple pairing and status page for one HIMS Scan R1 device.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <string>

namespace hims {

using namespace std;

namespace {

constexpr size_t kDebugWindowLines = 14;

}  // namespace

bool App::regenerateHimsScanToken() {
  const auto now = time(nullptr);
  if (settingsConfirmAction_ != "regenerate-token" || now > settingsConfirmUntil_) {
    settingsConfirmAction_ = "regenerate-token";
    settingsConfirmUntil_ = now + 5;
    setMessage("Regenerating invalidates the current token; activate again within 5 seconds to confirm", 5);
    return false;
  }
  settingsConfirmAction_.clear();
  settingsConfirmUntil_ = 0;
  himsScanConfig_.token = generateHimsScanToken();
  himsScanConfig_.deviceId.clear();
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
  server_.setDeviceCredentials(himsScanConfig_.deviceId, himsScanConfig_.token);
  if (!saveHimsScanConfig(himsScanConfigPath_, himsScanConfig_)) {
    setMessage("Generated a new token, but HIMS could not save it", 4);
    return false;
  }
  setMessage("Generated a new pairing token", 3);
  dirty_ = true;
  return true;
}

bool App::clearHimsScanPairing() {
  if (trim(himsScanConfig_.deviceId).empty()) {
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

  himsScanConfig_.deviceId.clear();
  deviceLastSeen_ = 0;
  deviceFirmwareVersion_.clear();
  deviceRssi_ = 0;
  deviceDebug_.clear();
  deviceLastResult_.clear();
  deviceProtocolVersion_ = 0;
  deviceMode_.clear();
  devicePendingEventCount_ = 0;
  deviceLastSync_ = 0;
  server_.setDeviceCredentials(himsScanConfig_.deviceId, himsScanConfig_.token);
  if (!saveHimsScanConfig(himsScanConfigPath_, himsScanConfig_)) {
    setMessage("Cleared pairing in memory, but HIMS could not save it", 4);
    return false;
  }
  setMessage("Cleared paired device identity", 3);
  dirty_ = true;
  return true;
}

bool App::copyHimsScanToken() {
  const auto token = trim(himsScanConfig_.token);
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

void App::openHimsScanSetup() {
  openSettings(SettingsCategory::HimsScan);
  setMessage("Pair the device with the token shown here", 4);
}

ftxui::Element App::renderHimsScanSetupUi() const {
  const auto serviceUrl = server_.running() ? server_.baseUrl() : string("R1 service unavailable");
  const auto deviceState = trim(himsScanConfig_.deviceId).empty() ? string("Waiting for first valid device report")
                                                                  : string("Paired device: ") + himsScanConfig_.deviceId;
  const auto lastSeen = deviceLastSeen_ == 0 ? string("No heartbeat yet") : string("Last heartbeat: ") +
                                                                         nowTimestampString(deviceLastSeen_);
  const auto statusLine = himsScanDeviceSummary();

  ftxui::Elements leftRows;
  leftRows.push_back(fullLine("Pairing flow", uiAccentColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine("1. Open the device portal on the ESP32.", uiTitleColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine("2. Join Wi-Fi; the scanner discovers the HIMS PC service.", uiTitleColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine("3. Paste the pairing token shown on this page.", uiTitleColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine("4. Scan a numeric HIMS code for quantity changes or a Data Matrix code for DigiKey",
                              uiTitleColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine("   intake; both are sent straight to HIMS software.", uiTitleColor(),
                              uiPanelLeftBg()));
  leftRows.push_back(uiDivider());
  leftRows.push_back(fullLine("R1 device service", uiAccentColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine(serviceUrl, server_.running() ? uiLinkColor() : uiWarnColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine("Pairing token", uiAccentColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine(himsScanConfig_.token.empty() ? string("Not configured")
                                                             : string("Configured - use Copy token"),
                              uiTitleColor(), uiPanelLeftBg()));
  leftRows.push_back(fullLine("[t] Copy token to clipboard", uiLinkColor(), uiPanelLeftBg()));

  ftxui::Elements rightRows;
  rightRows.push_back(fullLine("Device status", uiAccentColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine(statusLine, uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine(deviceState, uiMutedColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine(lastSeen, uiMutedColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("Protocol: " + (deviceProtocolVersion_ > 0 ? string("v") + to_string(deviceProtocolVersion_)
                                                                    : string("legacy")),
                               deviceProtocolVersion_ == 1 ? uiSuccessColor() : uiWarnColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("Mode: " + (deviceMode_.empty() ? string("n/a") : deviceMode_) +
                                   "  Pending: " + to_string(devicePendingEventCount_),
                               uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine(deviceLastSync_ == 0 ? string("Last sync: n/a")
                                                    : string("Last sync: ") + nowTimestampString(deviceLastSync_),
                               uiMutedColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("Wi-Fi debug", uiAccentColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine(deviceDebug_.empty() ? string("n/a") : ellipsize(deviceDebug_, 64),
                              uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(uiDivider());
  rightRows.push_back(fullLine("What the device sends", uiAccentColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("A scan code from the GM65 UART module", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("Numeric codes enter quantity mode", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("Data Matrix quantities wait for A confirmation", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("A or B add or subtract HIMS quantities", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("Protocol v1 retries safely without double applying stock", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(uiDivider());
  rightRows.push_back(fullLine("Actions", uiAccentColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("'t' copy token to clipboard", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("'r' regenerate token", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("'c' clear paired device", uiTitleColor(), uiPanelRightBg()));
  rightRows.push_back(fullLine("'Esc' return to dashboard", uiTitleColor(), uiPanelRightBg()));

  auto left = ftxui::vbox(move(leftRows)) | ftxui::bgcolor(uiPanelLeftBg()) | ftxui::flex;
  auto right = ftxui::vbox(move(rightRows)) | ftxui::bgcolor(uiPanelRightBg()) | ftxui::flex;
  return ftxui::vbox({
      ftxui::hbox({move(left), uiDivider(), move(right)}),
      uiDivider(),
      renderDeviceDebugConsoleUi(),
  });
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

void App::handleHimsScanSetupKey(const KeyEvent& key) {
  if (key.type == KeyType::Escape) {
    changePage(Page::Home);
    return;
  }

  if (key.type == KeyType::Character) {
    const auto ch = static_cast<char>(tolower(static_cast<unsigned char>(key.ch)));
    if (ch == 'r') {
      regenerateHimsScanToken();
      return;
    }
    if (ch == 't') {
      copyHimsScanToken();
      return;
    }
    if (ch == 'c') {
      clearHimsScanPairing();
      return;
    }
    if (ch == 'q') {
      running_ = false;
      return;
    }
  }

  if (key.type == KeyType::Up) {
    deviceDebugFollow_ = false;
    adjustDeviceDebugScroll(1);
    return;
  }

  if (key.type == KeyType::Down) {
    adjustDeviceDebugScroll(-1);
    if (deviceDebugScroll_ + 1 >= deviceDebugLog_.size()) {
      deviceDebugFollow_ = true;
    }
    return;
  }

  if (key.type == KeyType::PageUp) {
    deviceDebugFollow_ = false;
    adjustDeviceDebugScroll(8);
    return;
  }

  if (key.type == KeyType::PageDown) {
    adjustDeviceDebugScroll(-8);
    if (deviceDebugScroll_ + 1 >= deviceDebugLog_.size()) {
      deviceDebugFollow_ = true;
    }
    return;
  }

  if (key.type == KeyType::Home) {
    deviceDebugFollow_ = false;
    deviceDebugScroll_ = 0;
    return;
  }

  if (key.type == KeyType::End) {
    deviceDebugFollow_ = true;
    deviceDebugScroll_ = deviceDebugLog_.empty() ? 0 : deviceDebugLog_.size() - 1;
    return;
  }

}

}  // namespace hims
