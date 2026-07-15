// Inventatory - Skippable first-run setup for fresh Windows installs.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <chrono>
#include <cctype>
#include <ctime>

namespace inventatory {

using namespace std;

namespace {

string onboardingStepName(int step) {
  switch (step) {
    case 0: return "Welcome";
    case 1: return "Data folder";
    case 2: return "Background service";
    case 3: return "Printer";
    case 4: return "Scan R1";
    case 5: return "DigiKey";
    case 6: return "Complete";
  }
  return {};
}

}  // namespace

void App::advanceOnboarding() {
  onboardingStep_ = static_cast<OnboardingStep>(static_cast<int>(onboardingStep_) + 1);
  dirty_ = true;
}

void App::finishOnboarding() {
  settings_.completedOnboardingVersion = 1;
  settingsDraft_ = settings_;
  if (!saveAppSettings(settingsPath_, settings_)) {
    setMessage("Unable to save setup completion; the wizard will reopen next time", 5);
    return;
  }
  onboardingActive_ = false;
  changePage(Page::Home);
  setMessage("Setup complete. Every choice is available later in Settings.", 5);
}

ftxui::Element App::renderOnboardingUi() const {
  ftxui::Elements rows;
  rows.push_back(styledText("$ inventatory first-run setup", uiSuccessColor()) | ftxui::bold);
  rows.push_back(styledText("Step: " + onboardingStepName(static_cast<int>(onboardingStep_)) + "  |  Optional choices can be skipped and changed later.",
                            uiMutedText()));
  rows.push_back(uiDivider());

  switch (onboardingStep_) {
    case OnboardingStep::Welcome:
      rows.push_back(styledText("Welcome to Inventatory.", uiTitleColor()) | ftxui::bold);
      rows.push_back(ftxui::text("Inventatory stores your inventory locally and can keep Scan R1 available in the background."));
      rows.push_back(ftxui::text("Press Enter to begin."));
      break;
    case OnboardingStep::DataFolder:
      rows.push_back(styledText("Inventory data folder", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text("Default: " + dataPath_.string()));
      rows.push_back(ftxui::text("Enter keeps this location. Press B to choose another folder."));
      break;
    case OnboardingStep::BackgroundService:
      rows.push_back(styledText("Keep Inventatory ready for Scan R1?", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text("When enabled, Inventatory starts for this Windows user and stays in the notification area after the terminal closes."));
      rows.push_back(styledText("Y enables it  |  N keeps it off (default)", uiLinkColor()));
      break;
    case OnboardingStep::Printer:
      rows.push_back(styledText("Optional printer", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text(printerQueues_.empty() ? "No printers were detected." : "Press P to use the first detected printer: " + printerQueues_.front().name));
      rows.push_back(styledText("Enter skips this step. You can select and test a printer later in Settings.", uiMutedText()));
      break;
    case OnboardingStep::ScanR1:
      rows.push_back(styledText("Optional Scan R1 pairing", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text("Press S to start encrypted Bluetooth and Wi-Fi provisioning now."));
      rows.push_back(styledText("Enter skips it. Home > Set up Scan R1 remains available later.", uiMutedText()));
      break;
    case OnboardingStep::DigiKey:
      rows.push_back(styledText("Optional DigiKey credentials", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text("DigiKey enrichment is optional. Credentials are kept in Windows Credential Manager."));
      rows.push_back(styledText("Press D to configure them in Settings now, or Enter to skip.", uiMutedText()));
      break;
    case OnboardingStep::Complete:
      rows.push_back(styledText("Your Inventatory workspace is ready.", uiSuccessColor()) | ftxui::bold);
      rows.push_back(ftxui::text("You can revisit printer, Scan R1, DigiKey, startup, data location, and update settings at any time."));
      rows.push_back(styledText("Press Enter to open the dashboard.", uiLinkColor()));
      break;
  }

  rows.push_back(ftxui::filler());
  return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg());
}

void App::handleOnboardingKey(const KeyEvent& key) {
  const char ch = key.type == KeyType::Character ? static_cast<char>(tolower(static_cast<unsigned char>(key.ch))) : '\0';
  if (key.type == KeyType::Escape) {
    finishOnboarding();
    return;
  }
  switch (onboardingStep_) {
    case OnboardingStep::Welcome:
      if (key.type == KeyType::Enter) advanceOnboarding();
      return;
    case OnboardingStep::DataFolder:
      if (ch == 'b') {
        settingsDraft_ = settings_;
        if (stageInventatoryFolder() && saveSettingsDraft()) {
          settingsDraft_ = settings_;
          setMessage("Data folder saved", 3);
        }
      } else if (key.type == KeyType::Enter) {
        advanceOnboarding();
      }
      return;
    case OnboardingStep::BackgroundService:
      if (ch == 'y' || ch == 'n') {
        settingsDraft_ = settings_;
        settingsDraft_.backgroundServiceEnabled = ch == 'y';
        settingsDraft_.backgroundConsentAsked = true;
        if (saveSettingsDraft()) advanceOnboarding();
      }
      return;
    case OnboardingStep::Printer:
      if (ch == 'p' && !printerQueues_.empty()) {
        settingsDraft_ = settings_;
        settingsDraft_.printerQueue = printerQueues_.front().name;
        if (saveSettingsDraft()) advanceOnboarding();
      } else if (key.type == KeyType::Enter) {
        advanceOnboarding();
      }
      return;
    case OnboardingStep::ScanR1:
      if (ch == 's') {
        returnToOnboardingAfterScan_ = true;
        openInventatoryScanSetup();
      } else if (key.type == KeyType::Enter) {
        advanceOnboarding();
      }
      return;
    case OnboardingStep::DigiKey:
      if (ch == 'd') {
        finishOnboarding();
        openSettings(SettingsCategory::DigiKey);
      } else if (key.type == KeyType::Enter) {
        advanceOnboarding();
      }
      return;
    case OnboardingStep::Complete:
      if (key.type == KeyType::Enter) finishOnboarding();
      return;
  }
}

void App::beginUpdateCheckIfDue() {
  const auto now = static_cast<int64_t>(time(nullptr));
  if (!isUpdateCheckDue(settings_.updateChecksEnabled, settings_.lastUpdateCheckUnixSeconds, now)) return;
  updateCheckFuture_ = async(launch::async, [version = softwareVersion()] { return checkLatestPrivateBetaRelease(version); });
}

void App::processUpdateCheck() {
  if (!updateCheckFuture_.valid() || updateCheckFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
  const auto result = updateCheckFuture_.get();
  if (!result.completed) return;
  settings_.lastUpdateCheckUnixSeconds = static_cast<int64_t>(time(nullptr));
  settings_.latestAvailableVersion = result.updateAvailable ? result.latestVersion : string();
  settings_.latestReleaseUrl = result.updateAvailable ? result.releaseUrl : string();
  settingsDraft_ = settings_;
  saveAppSettings(settingsPath_, settings_);
  if (result.updateAvailable) setMessage("Inventatory " + result.latestVersion + " is available in Settings", 6);
}

}  // namespace inventatory
