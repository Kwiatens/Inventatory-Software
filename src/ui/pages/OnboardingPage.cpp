// Inventatory - Focused first-run terminal setup for fresh Windows installs.

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
    case 3: return "Scan R1";
    case 4: return "Complete";
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
  rows.push_back(styledText("step> " + onboardingStepName(static_cast<int>(onboardingStep_)), uiMutedText()));
  rows.push_back(uiDivider());

  switch (onboardingStep_) {
    case OnboardingStep::Welcome:
      rows.push_back(styledText("Welcome to Inventatory.", uiTitleColor()) | ftxui::bold);
      rows.push_back(ftxui::text("Your inventory stays on this PC. Optional features can be configured later."));
      rows.push_back(styledText("> Press Enter to begin", uiLinkColor()));
      break;
    case OnboardingStep::DataFolder:
      rows.push_back(styledText("Inventory data folder", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text("Default: " + dataPath_.string()));
      rows.push_back(ftxui::text("Enter keeps this location. Press B to choose another folder."));
      break;
    case OnboardingStep::BackgroundService:
      rows.push_back(styledText("Keep Inventatory running in the background?", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text("When enabled, Inventatory starts when you sign in and remains available in the notification area after this terminal closes."));
      rows.push_back(styledText("Y enables it  |  N keeps it off (default)", uiLinkColor()));
      break;
    case OnboardingStep::ScanR1:
      rows.push_back(styledText("Do you have a hardware Inventatory Scan R1 scanning device?", uiAccentColor()) | ftxui::bold);
      rows.push_back(ftxui::text("The Scan R1 is an optional handheld device that scans parts into this Inventatory PC."));
      rows.push_back(styledText("Y starts Scan R1 setup now  |  N continues without one", uiLinkColor()));
      break;
    case OnboardingStep::Complete:
      rows.push_back(styledText("Your Inventatory workspace is ready.", uiSuccessColor()) | ftxui::bold);
      rows.push_back(ftxui::text("You can configure printers, Scan R1, vendor integrations, startup, and data location later in Settings."));
      rows.push_back(styledText("Press Enter to open the dashboard.", uiLinkColor()));
      break;
  }

  rows.push_back(ftxui::filler());
  return ftxui::vbox(move(rows)) | ftxui::flex | ftxui::bgcolor(uiCanvasBg());
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
    case OnboardingStep::ScanR1:
      if (ch == 'y') {
        returnToOnboardingAfterScan_ = true;
        openInventatoryScanSetup();
      } else if (ch == 'n') {
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
  updateCheckFuture_ = async(launch::async, [version = softwareVersion()] { return checkLatestRelease(version); });
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
