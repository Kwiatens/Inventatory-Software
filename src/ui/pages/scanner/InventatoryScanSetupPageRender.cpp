// Inventatory - Scan R1 setup page rendering.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <string>

namespace inventatory {

using namespace std;

ftxui::Element App::renderInventatoryScanSetupContent() const {
  ftxui::Elements rows;
  switch (scanSetupStep_) {
    case ScanSetupStep::Introduction:
      rows.push_back(styledText("Connect your Scan R1", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Power on the scanner, keep Bluetooth enabled, and have the Wi-Fi details ready.", uiSecondaryText()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Enter begin   Esc cancel", uiInteractiveColor()));
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
      rows.push_back(styledText("Looking for nearby Scan R1 devices...",
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
      rows.push_back(styledText("Enter transfer configuration   Esc cancel", uiInteractiveColor()));
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
      rows.push_back(styledText("Enter continue", uiLinkColor()));
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

}  // namespace inventatory
