// Inventatory - Focused first-run terminal setup for fresh installs.

#include "App.h"

#include "platform/system/StartupRegistration.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>

namespace inventatory {

using namespace std;

namespace {

constexpr long long kWizardSelectionHoldDurationMs = 500;
constexpr long long kWizardBlankDurationMs = 200;
constexpr int kWizardReservedContentHeight = 8;

ftxui::Element onboardingPrompt(const string& text) {
  return styledText(text, uiLinkColor());
}

ftxui::Element onboardingChoicePrompt(char selectedOption, const string& first, const string& second) {
  const auto firstColor = selectedOption == 'y' ? uiLinkColor() : uiMutedText();
  const auto secondColor = selectedOption == 'n' ? uiLinkColor() : uiMutedText();
  return ftxui::hbox({styledText("[ Y ] " + first, firstColor), styledText("   ", uiMutedText()),
                      styledText("[ N ] " + second, secondColor)});
}

constexpr int kWordmarkRows = 6;
constexpr long long kWordmarkScanDelayMs = 1350;
constexpr long long kWordmarkScanDurationMs = 1100;

uint32_t blendRgb(uint32_t from, uint32_t to, double amount) {
  const auto channel = [from, to, amount](int shift) {
    const auto a = static_cast<double>((from >> shift) & 0xFFu);
    const auto b = static_cast<double>((to >> shift) & 0xFFu);
    return static_cast<uint32_t>(a + (b - a) * amount + 0.5) << shift;
  };
  return channel(16) | channel(8) | channel(0);
}

ftxui::Color rgbColor(uint32_t rgb) {
  return ftxui::Color::RGB(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
}

uint32_t onboardingGradientRgb(int row) {
  const auto& colors = activeUiAppearance().colors;
  const auto start = colors[static_cast<size_t>(AppearanceColorRole::Interactive)];
  const auto end = colors[static_cast<size_t>(AppearanceColorRole::FocusText)];
  return blendRgb(start, end, static_cast<double>(row) / (kWordmarkRows - 1));
}

// scanPosition < 0 renders the static gradient. Otherwise it is the scan
// line's vertical position in rows: rows it has passed show the gradient,
// rows ahead stay muted, and rows near the line glow toward primary text.
ftxui::Color wordmarkRowColor(int row, double scanPosition) {
  if (scanPosition < 0.0) return rgbColor(onboardingGradientRgb(row));
  const auto& colors = activeUiAppearance().colors;
  const auto center = static_cast<double>(row) + 0.5;
  const auto base = center < scanPosition ? onboardingGradientRgb(row)
                                          : colors[static_cast<size_t>(AppearanceColorRole::MutedText)];
  const auto glow = max(0.0, 1.0 - abs(center - scanPosition) / 1.25);
  return rgbColor(blendRgb(base, colors[static_cast<size_t>(AppearanceColorRole::PrimaryText)], glow));
}

ftxui::Element alignedOnboardingWordmark(double scanPosition) {
  const vector<string> wordmarkRows = {
      u8"██╗███╗   ██╗██╗   ██╗███████╗███╗   ██╗████████╗ █████╗ ████████╗ ██████╗ ██████╗ ██╗   ██╗",
      u8"██║████╗  ██║██║   ██║██╔════╝████╗  ██║╚══██╔══╝██╔══██╗╚══██╔══╝██╔═══██╗██╔══██╗╚██╗ ██╔╝",
      u8"██║██╔██╗ ██║██║   ██║█████╗  ██╔██╗ ██║   ██║   ███████║   ██║   ██║   ██║██████╔╝ ╚████╔╝ ",
      u8"██║██║╚██╗██║╚██╗ ██╔╝██╔══╝  ██║╚██╗██║   ██║   ██╔══██║   ██║   ██║   ██║██╔══██╗  ╚██╔╝  ",
      u8"██║██║ ╚████║ ╚████╔╝ ███████╗██║ ╚████║   ██║   ██║  ██║   ██║   ╚██████╔╝██║  ██║   ██║   ",
      u8"╚═╝╚═╝  ╚═══╝  ╚═══╝  ╚══════╝╚═╝  ╚═══╝   ╚═╝   ╚═╝  ╚═╝   ╚═╝    ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ",
  };

  ftxui::Elements renderedRows;
  renderedRows.reserve(wordmarkRows.size());
  for (size_t row = 0; row < wordmarkRows.size(); ++row) {
    renderedRows.push_back(styledText(wordmarkRows[row], wordmarkRowColor(static_cast<int>(row), scanPosition)));
  }
  return ftxui::vbox(move(renderedRows));
}

}  // namespace

bool App::wordmarkScanRunning() const {
  const auto startedAt = wordmarkScanStartedAt_.load();
  return startedAt >= 0 && uiAnimationTicks() - startedAt < kWordmarkScanDelayMs + kWordmarkScanDurationMs;
}

ftxui::Element App::renderOnboardingWordmark() const {
  const auto startedAt = wordmarkScanStartedAt_.load();
  if (startedAt < 0 || page_ != Page::Update) return alignedOnboardingWordmark(-1.0);
  const auto elapsed = uiAnimationTicks() - startedAt - kWordmarkScanDelayMs;
  if (elapsed >= kWordmarkScanDurationMs) return alignedOnboardingWordmark(-1.0);
  // Travel from above the first row to below the last so the glow fully
  // enters and leaves the wordmark.
  constexpr double kTravel = kWordmarkRows + 3.0;
  const auto progress = max(0.0, static_cast<double>(elapsed) / kWordmarkScanDurationMs);
  return alignedOnboardingWordmark(progress * kTravel - 1.5);
}

ftxui::Element App::renderOnboardingFrame(ftxui::Element content) const {
  auto reservedContent = ftxui::vbox({move(content), ftxui::filler()}) |
                         ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, kWizardReservedContentHeight);
  auto centeredContent = ftxui::vbox({
      ftxui::hbox({ftxui::filler(), renderOnboardingWordmark(), ftxui::filler()}),
      ftxui::text(""),
      ftxui::hbox({ftxui::filler(), move(reservedContent), ftxui::filler()}),
  });
  auto version = ftxui::hbox({ftxui::filler(), styledText("Inventatory v" + softwareVersion(), uiMutedText()),
                              ftxui::filler()});
  return ftxui::vbox({
             ftxui::filler(),
             move(centeredContent),
             ftxui::filler(),
             move(version),
         }) |
         ftxui::flex | ftxui::bgcolor(uiCanvasBg());
}

void App::advanceOnboarding() {
  const auto next = static_cast<OnboardingStep>(static_cast<int>(onboardingStep_) + 1);
  beginWizardTransition(Page::Onboarding, next, scanSetupStep_, returnToOnboardingAfterScan_);
}

void App::finishOnboarding() {
  settings_.completedOnboardingVersion = 1;
  settingsDraft_ = settings_;
  if (!saveAppSettings(settingsPath_, settings_)) {
    setMessage("Unable to save setup completion; setup will open again next time", 5);
    return;
  }
  string shortcutError;
  const bool shortcutCreated = createDesktopShortcut(shortcutError);
  onboardingActive_ = false;
  changePage(Page::Stock);
#ifdef _WIN32
  setMessage(shortcutCreated ? "Setup complete."
                             : "Setup complete; desktop shortcut could not be created: " + shortcutError,
             shortcutCreated ? 5 : 7);
#else
  setMessage(shortcutCreated ? "Setup complete."
                             : "Setup complete; application launcher could not be created: " + shortcutError,
             shortcutCreated ? 5 : 7);
#endif
}

ftxui::Element App::renderOnboardingContent() const {
  ftxui::Elements rows;

  switch (onboardingStep_) {
    case OnboardingStep::Welcome:
      rows.push_back(uiHeaderText("Welcome to Inventatory", uiTitleColor()));
      rows.push_back(styledText("Let's get your workspace ready.", uiSecondaryText()));
      rows.push_back(onboardingPrompt("[ Enter ] Continue"));
      break;
    case OnboardingStep::DataFolder:
      rows.push_back(uiHeaderText("Inventory data folder", uiTitleColor()));
      rows.push_back(styledText("Current folder: " + dataPath_.string(), uiSecondaryText()));
      rows.push_back(onboardingPrompt("[ Enter ] Use this folder   [ B ] Choose another"));
      break;
    case OnboardingStep::BackgroundService:
      rows.push_back(uiHeaderText("Run Inventatory in the background?", uiTitleColor()));
#ifdef _WIN32
      rows.push_back(styledText("Starts with Windows and stays available in the notification area.", uiSecondaryText()));
#else
      rows.push_back(styledText("Starts with Linux as a background service for your user session.", uiSecondaryText()));
#endif
      if (wizardTransition_.phase == WizardTransitionPhase::SelectionHold && wizardSelectedOption_.has_value()) {
        rows.push_back(onboardingChoicePrompt(*wizardSelectedOption_, "Enable", "Skip"));
      } else {
        rows.push_back(onboardingPrompt("[ Y ] Enable   [ N ] Skip"));
      }
      break;
    case OnboardingStep::ScanR1:
      rows.push_back(uiHeaderText("Set up an Inventatory Scan R1?", uiTitleColor()));
      rows.push_back(styledText("Have the scanner nearby, powered on, with Bluetooth enabled.", uiSecondaryText()));
      if (wizardTransition_.phase == WizardTransitionPhase::SelectionHold && wizardSelectedOption_.has_value()) {
        rows.push_back(onboardingChoicePrompt(*wizardSelectedOption_, "Set up now", "Skip"));
      } else {
        rows.push_back(onboardingPrompt("[ Y ] Set up now   [ N ] Skip"));
      }
      break;
    case OnboardingStep::Complete:
      rows.push_back(uiHeaderText("Your workspace is ready.", uiSuccessColor()));
      rows.push_back(onboardingPrompt("[ Enter ] Open Inventatory"));
      break;
  }

  return ftxui::vbox(move(rows));
}

ftxui::Element App::renderOnboardingUi() const {
  return renderOnboardingFrame(renderOnboardingContent());
}

ftxui::Element App::renderWizardContent() const {
  if (page_ == Page::ScanSetup && returnToOnboardingAfterScan_) {
    return renderInventatoryScanSetupContent();
  }
  return renderOnboardingContent();
}

ftxui::Element App::renderWizardUi() const {
  auto content = renderWizardContent();
  if (wizardTransition_.phase == WizardTransitionPhase::Blank) content = ftxui::text("");
  return renderOnboardingFrame(move(content));
}

void App::beginWizardTransition(Page targetPage, OnboardingStep targetOnboardingStep,
                                 ScanSetupStep targetScanSetupStep,
                                 bool targetReturnToOnboardingAfterScan) {
  if (wizardTransition_.phase != WizardTransitionPhase::None) return;
  wizardTransition_.phase = wizardSelectedOption_.has_value() ? WizardTransitionPhase::SelectionHold
                                                                : WizardTransitionPhase::Blank;
  wizardTransition_.startedAt = uiAnimationTicks();
  wizardTransition_.targetPage = targetPage;
  wizardTransition_.targetOnboardingStep = targetOnboardingStep;
  wizardTransition_.targetScanSetupStep = targetScanSetupStep;
  wizardTransition_.targetReturnToOnboardingAfterScan = targetReturnToOnboardingAfterScan;
  bufferedWizardKey_.reset();
  dirty_ = true;
}

void App::applyWizardTransitionTarget() {
  const auto previousPage = page_;
  page_ = wizardTransition_.targetPage;
  onboardingStep_ = wizardTransition_.targetOnboardingStep;
  scanSetupStep_ = wizardTransition_.targetScanSetupStep;
  returnToOnboardingAfterScan_ = wizardTransition_.targetReturnToOnboardingAfterScan;
  inputMode_ = InputMode::None;
  inputBuffer_.clear();
  focusedTarget_ = -1;
  if (previousPage == Page::ScanSetup && page_ == Page::Onboarding) {
    bleWifiSsid_.clear();
    bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
    bleWifiPassword_.clear();
    blePairingCode_.clear();
  }
  dirty_ = true;
}

void App::updateWizardTransition() {
  if (wizardTransition_.phase == WizardTransitionPhase::None) return;

  const auto elapsed = max(0LL, uiAnimationTicks() - wizardTransition_.startedAt);
  if (wizardTransition_.phase == WizardTransitionPhase::SelectionHold) {
    if (elapsed < kWizardSelectionHoldDurationMs) {
      dirty_ = true;
      return;
    }
    wizardSelectedOption_.reset();
    wizardTransition_.phase = WizardTransitionPhase::Blank;
    wizardTransition_.startedAt = uiAnimationTicks();
    dirty_ = true;
    return;
  }
  if (elapsed < kWizardBlankDurationMs) {
    dirty_ = true;
    return;
  }

  applyWizardTransitionTarget();
  wizardTransition_.phase = WizardTransitionPhase::None;
  wizardTransition_.startedAt = -1;
  auto buffered = move(bufferedWizardKey_);
  wizardSelectedOption_.reset();
  dirty_ = true;
  if (buffered.has_value()) handleKey(*buffered);
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
    case OnboardingStep::BackgroundService:
      if (ch == 'y' || ch == 'n') {
        settingsDraft_ = settings_;
        settingsDraft_.backgroundServiceEnabled = ch == 'y';
        settingsDraft_.backgroundConsentAsked = true;
        if (saveSettingsDraft()) {
          wizardSelectedOption_ = ch;
          advanceOnboarding();
        }
      }
      return;
    case OnboardingStep::ScanR1:
      if (ch == 'y') {
        wizardSelectedOption_ = ch;
        returnToOnboardingAfterScan_ = true;
        openInventatoryScanSetup();
      } else if (ch == 'n') {
        wizardSelectedOption_ = ch;
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
  setMessage(started ? "Checking for software updates" : "Already checking for software updates", 4);
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
    if (!saveAppSettings(settingsPath_, settings_)) {
      appSettingsSavePending_ = true;
      persistenceError_ = "Could not save update-check results; they remain in memory.";
      setMessage(persistenceError_ + " Press R to retry.", 5);
    } else {
      appSettingsSavePending_ = false;
    }
    if (result.updateAvailable) setMessage("Inventatory " + result.latestVersion + " is available in Updates", 6);
  } else {
    setMessage("Could not reach the software release channel", 5);
  }
  dirty_ = true;
}

}  // namespace inventatory
