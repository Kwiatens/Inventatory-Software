// Inventatory - Focused first-run terminal setup for fresh Windows installs.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <chrono>
#include <cctype>
#include <ctime>

namespace inventatory {

using namespace std;

namespace {

ftxui::Element onboardingPrompt(const string& text) {
  return styledText(text, uiLinkColor());
}

ftxui::Color onboardingGradientColor(int row) {
  const auto& colors = activeUiAppearance().colors;
  const auto start = colors[static_cast<size_t>(AppearanceColorRole::Interactive)];
  const auto end = colors[static_cast<size_t>(AppearanceColorRole::FocusText)];
  constexpr int kLastRow = 5;
  const auto channel = [start, end, row](int shift) {
    const auto startChannel = static_cast<int>((start >> shift) & 0xFFu);
    const auto endChannel = static_cast<int>((end >> shift) & 0xFFu);
    return startChannel + (endChannel - startChannel) * row / kLastRow;
  };
  return ftxui::Color::RGB(static_cast<uint8_t>(channel(16)), static_cast<uint8_t>(channel(8)),
                           static_cast<uint8_t>(channel(0)));
}

ftxui::Element welcomeWordmark() {
  // The block glyphs carry their own extrusion; the row colors add a quiet
  // theme-aware blue gradient without introducing another visual treatment.
  const vector<string> lines = {
      u8"\u2588\u2588\u2557\u2588\u2588\u2588\u2557   \u2588\u2588\u2557\u2588\u2588\u2557   \u2588\u2588\u2557\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557\u2588\u2588\u2588\u2557   \u2588\u2588\u2557\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557 \u2588\u2588\u2588\u2588\u2588\u2557 \u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557 \u2588\u2588\u2588\u2588\u2588\u2588\u2557 \u2588\u2588\u2588\u2588\u2588\u2588\u2557 \u2588\u2588\u2557   \u2588\u2588\u2557",
      u8"\u2588\u2588\u2551\u2588\u2588\u2588\u2588\u2557  \u2588\u2588\u2551\u2588\u2588\u2551   \u2588\u2588\u2551\u2588\u2588\u2554\u2550\u2550\u2550\u2550\u255d\u2588\u2588\u2588\u2588\u2557  \u2588\u2588\u2551\u255a\u2550\u2550\u2588\u2588\u2554\u2550\u2550\u255d\u2588\u2588\u2554\u2550\u2550\u2588\u2588\u2557\u255a\u2550\u2550\u2588\u2588\u2554\u2550\u2550\u255d\u2588\u2588\u2554\u2550\u2550\u2550\u2588\u2588\u2557\u2588\u2588\u2554\u2550\u2550\u2588\u2588\u2557\u255a\u2588\u2588\u2557 \u2588\u2588\u2554\u255d",
      u8"\u2588\u2588\u2551\u2588\u2588\u2554\u2588\u2588\u2557 \u2588\u2588\u2551\u2588\u2588\u2551   \u2588\u2588\u2551\u2588\u2588\u2588\u2588\u2588\u2557  \u2588\u2588\u2554\u2588\u2588\u2557 \u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2588\u2588\u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2551\u2588\u2588\u2588\u2588\u2588\u2588\u2554\u255d \u255a\u2588\u2588\u2588\u2588\u2554\u255d",
      u8"\u2588\u2588\u2551\u2588\u2588\u2551\u255a\u2588\u2588\u2557\u2588\u2588\u2551\u255a\u2588\u2588\u2557 \u2588\u2588\u2554\u255d\u2588\u2588\u2554\u2550\u2550\u255d  \u2588\u2588\u2551\u255a\u2588\u2588\u2557\u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2551\u2588\u2588\u2554\u2550\u2550\u2588\u2588\u2557  \u255a\u2588\u2588\u2554\u255d",
      u8"\u2588\u2588\u2551\u2588\u2588\u2551 \u255a\u2588\u2588\u2588\u2588\u2551 \u255a\u2588\u2588\u2588\u2588\u2554\u255d \u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557\u2588\u2588\u2551 \u255a\u2588\u2588\u2588\u2588\u2551   \u2588\u2588\u2551   \u2588\u2588\u2551  \u2588\u2588\u2551   \u2588\u2588\u2551   \u255a\u2588\u2588\u2588\u2588\u2588\u2588\u2554\u255d\u2588\u2588\u2551  \u2588\u2588\u2551   \u2588\u2588\u2551",
      u8"\u255a\u2550\u255d\u255a\u2550\u255d  \u255a\u2550\u2550\u2550\u255d  \u255a\u2550\u2550\u2550\u255d  \u255a\u2550\u2550\u2550\u2550\u2550\u2550\u255d\u255a\u2550\u255d  \u255a\u2550\u2550\u2550\u255d   \u255a\u2550\u255d   \u255a\u2550\u255d  \u255a\u2550\u255d   \u255a\u2550\u255d    \u255a\u2550\u2550\u2550\u2550\u2550\u255d \u255a\u2550\u255d  \u255a\u2550\u255d   \u255a\u2550\u255d",
  };
  ftxui::Elements rows;
  for (size_t row = 0; row < lines.size(); ++row) {
    rows.push_back(styledText(lines[row], onboardingGradientColor(static_cast<int>(row))));
  }
  return ftxui::vbox(move(rows));
}

ftxui::Element renderWelcomeScreen() {
  auto content = ftxui::vbox({
      welcomeWordmark(),
      ftxui::text(""),
      uiHeaderText("Welcome to Inventatory", uiTitleColor()),
      styledText("Let's get your workspace ready.", uiSecondaryText()),
      ftxui::text(""),
      onboardingPrompt("[ Enter ] Continue"),
  });

  return ftxui::vbox({
             ftxui::filler(),
             ftxui::hbox({ftxui::filler(), ftxui::center(move(content)), ftxui::filler()}),
             ftxui::filler(),
         }) |
         ftxui::flex | ftxui::bgcolor(uiCanvasBg());
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
    setMessage("Unable to save setup completion; setup will open again next time", 5);
    return;
  }
  onboardingActive_ = false;
  changePage(Page::Home);
  setMessage("Setup complete.", 5);
}

ftxui::Element App::renderOnboardingUi() const {
  if (onboardingStep_ == OnboardingStep::Welcome) {
    return renderWelcomeScreen();
  }

  ftxui::Elements rows;

  switch (onboardingStep_) {
    case OnboardingStep::Welcome:
      break;
    case OnboardingStep::DataFolder:
      rows.push_back(uiHeaderText("Choose where inventory data lives", uiTitleColor()));
      rows.push_back(styledText("Current folder: " + dataPath_.string(), uiSecondaryText()));
      rows.push_back(onboardingPrompt("[ Enter ] Use this folder   [ B ] Choose another"));
      break;
    case OnboardingStep::Complete:
      rows.push_back(uiHeaderText("You're all set.", uiSuccessColor()));
      rows.push_back(onboardingPrompt("[ Enter ] Open Inventatory"));
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
          setMessage("Data folder updated", 3);
        }
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
  if (updateCheckFuture_.valid() ||
      !isUpdateCheckDue(settings_.updateChecksEnabled, settings_.lastUpdateCheckUnixSeconds, now)) {
    return;
  }
  updateCheckChecked_ = false;
  updateCheckFailed_ = false;
  updateCheckFuture_ = async(launch::async, [version = softwareVersion()] { return checkLatestRelease(version); });
}

void App::beginUpdateChecks() {
  bool started = false;
  if (!updateCheckFuture_.valid()) {
    // A manual check is always allowed, even when the daily background check
    // preference is disabled.
    settings_.lastUpdateCheckUnixSeconds = 0;
    updateCheckChecked_ = false;
    updateCheckFailed_ = false;
    updateCheckFuture_ = async(launch::async, [version = softwareVersion()] { return checkLatestRelease(version); });
    started = true;
  }

  const bool scannerPaired = !trim(inventatoryScanConfig_.deviceId).empty() ||
                             !deviceFirmwareVersion_.empty();
  if (scannerPaired && !scanFirmwareFuture_.valid()) {
    beginScanFirmwareCheck();
    started = true;
  }

  setMessage(started ? (scannerPaired ? "Checking for software and scanner updates"
                                      : "Checking for software updates")
                     : "Already checking for updates",
             4);
  dirty_ = true;
}

void App::processUpdateCheck() {
  if (!updateCheckFuture_.valid() || updateCheckFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
  const auto result = updateCheckFuture_.get();
  updateCheckChecked_ = true;
  updateCheckFailed_ = !result.completed;
  if (result.completed) {
    settings_.lastUpdateCheckUnixSeconds = static_cast<int64_t>(time(nullptr));
    settings_.latestAvailableVersion = result.updateAvailable ? result.latestVersion : string();
    settings_.latestReleaseUrl = result.updateAvailable ? result.releaseUrl : string();
    // Keep staged settings edits intact while refreshing the cached release
    // result that is shown on the Updates page.
    settingsDraft_.lastUpdateCheckUnixSeconds = settings_.lastUpdateCheckUnixSeconds;
    settingsDraft_.latestAvailableVersion = settings_.latestAvailableVersion;
    settingsDraft_.latestReleaseUrl = settings_.latestReleaseUrl;
    saveAppSettings(settingsPath_, settings_);
    if (result.updateAvailable) setMessage("Inventatory " + result.latestVersion + " is available in Updates", 6);
  }
  dirty_ = true;
}

}  // namespace inventatory
