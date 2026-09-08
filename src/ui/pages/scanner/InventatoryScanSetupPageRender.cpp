// Inventatory - Scan R1 setup page rendering.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <string>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kDebugWindowLines = 14;

}  // namespace

ftxui::Element App::renderInventatoryScanSetupContent() const {
  ftxui::Elements rows;
  switch (scanSetupStep_) {
    case ScanSetupStep::Introduction:
      rows.push_back(styledText("Connect your Scan R1", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Power on the scanner, keep Bluetooth enabled, and have the Wi-Fi details ready.", uiSecondaryText()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("[ Enter ] Begin   [ Esc ] Cancel", uiInteractiveColor()));
      break;
    case ScanSetupStep::WifiName:
      rows.push_back(styledText("Wi-Fi network for the Scan R1", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("network> " + inputBuffer_ + "_", uiInteractiveColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Enter continues   Esc cancels", uiMutedText()));
      break;
    case ScanSetupStep::WifiPassword:
      rows.push_back(styledText("Wi-Fi password", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("password> " + string(inputBuffer_.size(), '*') + "_", uiInteractiveColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Enter continues   Esc cancels", uiMutedText()));
      break;
    case ScanSetupStep::PairingCode:
      rows.push_back(styledText("Six-digit code shown on the Scan R1", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("r1-code> " + inputBuffer_ + "_", uiInteractiveColor()));
      break;
    case ScanSetupStep::FindScanner: {
      const auto devices = bleProvisioning_.devices();
      rows.push_back(styledText("Looking for nearby Scan R1 devices " + uiLoadingSpinner(),
                                uiTitleColor()));
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
      rows.push_back(styledText("Up/Down selects   R refreshes   Enter continues", uiMutedText()));
      break;
    }
    case ScanSetupStep::Confirm: {
      const auto devices = bleProvisioning_.devices();
      const auto scanner = bleSetupSelection_ < devices.size() ? devices[bleSetupSelection_].name : string("No scanner selected");
      rows.push_back(styledText("Scanner: " + scanner, uiTitleColor()));
      rows.push_back(styledText("Wi-Fi network: " + bleWifiSsid_, uiTitleColor()));
      rows.push_back(styledText("Wi-Fi password: " + string(bleWifiPassword_.empty() ? 0 : 12, '*'), uiTitleColor()));
      rows.push_back(styledText("Verification code: " + blePairingCode_, uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("[ Enter ] Transfer configuration   [ Esc ] Cancel", uiInteractiveColor()));
      break;
    }
    case ScanSetupStep::Complete:
      if (bleSetupOutcomeUncertain_) {
        rows.push_back(uiHeaderText("Setup result was not confirmed.", uiWarnColor()));
        rows.push_back(styledText("The token was retained because the R1 may already own it. If it does not connect, run setup again.",
                                  uiTitleColor()));
      } else {
        rows.push_back(uiHeaderText("Setup request accepted.", uiSuccessColor()));
        rows.push_back(styledText("The Scan R1 is joining Wi-Fi and will connect automatically.", uiTitleColor()));
      }
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("[ Enter ] Continue", uiLinkColor()));
      break;
  }

  return ftxui::vbox(move(rows));
}

ftxui::Element App::renderInventatoryScanSetupUi() const {
  const bool onboardingTerminal = returnToOnboardingAfterScan_;
  const auto setupBackground = onboardingTerminal ? uiCanvasBg() : uiPanelLeftBg();
  auto body = renderInventatoryScanSetupContent();
  if (onboardingTerminal) return renderOnboardingFrame(body | ftxui::bgcolor(setupBackground));
  body = body | ftxui::bgcolor(setupBackground) | ftxui::flex;
  return ftxui::window(ftxui::text(""), body) | ftxui::bgcolor(uiCanvasBg());
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


}  // namespace inventatory
