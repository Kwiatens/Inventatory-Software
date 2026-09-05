// Inventatory - Focused first-run terminal setup for fresh Windows installs.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>

#include <ftxui/screen/string.hpp>

namespace inventatory {

using namespace std;

namespace {

constexpr long long kWizardCrossfadeDurationMs = 800;
constexpr long long kWizardSubstepCrossfadeDurationMs = 1200;
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

float clampUnit(float value) {
  return max(0.0F, min(1.0F, value));
}

ftxui::Color appearanceColorWithAlpha(AppearanceColorRole role, uint8_t alpha) {
  const auto rgb = activeUiAppearance().colors[static_cast<size_t>(role)];
  return ftxui::Color::RGBA(static_cast<uint8_t>((rgb >> 16) & 0xFFu),
                            static_cast<uint8_t>((rgb >> 8) & 0xFFu),
                            static_cast<uint8_t>(rgb & 0xFFu), alpha);
}

ftxui::Color wizardCanvasOverlay(float visibleContent) {
  const auto alpha = static_cast<uint8_t>(255.0F * (1.0F - clampUnit(visibleContent)));
  return appearanceColorWithAlpha(AppearanceColorRole::CanvasBg, alpha);
}

ftxui::Element fadeWizardContent(ftxui::Element content, float visibleContent) {
  const auto overlay = wizardCanvasOverlay(visibleContent);
  return ftxui::dbox({move(content), ftxui::filler() | ftxui::color(overlay) | ftxui::bgcolor(overlay)});
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

ftxui::Element alignedOnboardingWordmark() {
  // Keep each letter in a fixed-width cell. The diagonal rows are narrower
  // than the top rows; composing them as one string pushes the final letters
  // sideways as the row changes.
  const vector<vector<string>> glyphs = {
      {u8"\u2588\u2588\u2557", u8"\u2588\u2588\u2551", u8"\u2588\u2588\u2551", u8"\u2588\u2588\u2551", u8"\u2588\u2588\u2551", u8"\u255a\u2550\u255d"},
      {u8"\u2588\u2588\u2588\u2557   \u2588\u2588\u2557", u8"\u2588\u2588\u2588\u2588\u2557  \u2588\u2588\u2551", u8"\u2588\u2588\u2554\u2588\u2588\u2557 \u2588\u2588\u2551", u8"\u2588\u2588\u2551\u255a\u2588\u2588\u2557\u2588\u2588\u2551", u8"\u2588\u2588\u2551 \u255a\u2588\u2588\u2588\u2588\u2551", u8"\u255a\u2550\u255d  \u255a\u2550\u2550\u2550\u255d"},
      {u8"\u2588\u2588\u2557   \u2588\u2588\u2557", u8"\u2588\u2588\u2551   \u2588\u2588\u2551", u8"\u2588\u2588\u2551   \u2588\u2588\u2551", u8"\u255a\u2588\u2588\u2557 \u2588\u2588\u2554\u255d", u8" \u255a\u2588\u2588\u2588\u2588\u2554\u255d ", u8"  \u255a\u2550\u2550\u2550\u255d  "},
      {u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557", u8"\u2588\u2588\u2554\u2550\u2550\u2550\u2550\u255d", u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557", u8"\u255a\u2550\u2550\u2550\u2550\u2588\u2588\u2551", u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2551", u8"\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u255d"},
      {u8"\u2588\u2588\u2588\u2557   \u2588\u2588\u2557", u8"\u2588\u2588\u2588\u2588\u2557  \u2588\u2588\u2551", u8"\u2588\u2588\u2554\u2588\u2588\u2557 \u2588\u2588\u2551", u8"\u2588\u2588\u2551\u255a\u2588\u2588\u2557\u2588\u2588\u2551", u8"\u2588\u2588\u2551 \u255a\u2588\u2588\u2588\u2588\u2551", u8"\u255a\u2550\u255d  \u255a\u2550\u2550\u2550\u255d"},
      {u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557", u8"\u255a\u2550\u2550\u2588\u2588\u2554\u2550\u2550\u255d", u8"   \u2588\u2588\u2551   ", u8"   \u2588\u2588\u2551   ", u8"   \u2588\u2588\u2551   ", u8"   \u255a\u2550\u255d   "},
      {u8" \u2588\u2588\u2588\u2588\u2588\u2557 ", u8"\u2588\u2588\u2554\u2550\u2550\u2588\u2588\u2557", u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2551", u8"\u2588\u2588\u2554\u2550\u2550\u2588\u2588\u2551", u8"\u2588\u2588\u2551  \u2588\u2588\u2551", u8"\u255a\u2550\u255d  \u255a\u2550\u255d"},
      {u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2588\u2557", u8"\u255a\u2550\u2550\u2550\u2588\u2588\u2554\u2550\u2550\u255d", u8"   \u2588\u2588\u2551   ", u8"   \u2588\u2588\u2551   ", u8"   \u2588\u2588\u2551   ", u8"   \u255a\u2550\u255d   "},
      {u8" \u2588\u2588\u2588\u2588\u2588\u2588\u2557 ", u8"\u2588\u2588\u2554\u2550\u2550\u2550\u2588\u2588\u2557", u8"\u2588\u2588\u2551   \u2588\u2588\u2551", u8"\u2588\u2588\u2551   \u2588\u2588\u2551", u8"\u255a\u2588\u2588\u2588\u2588\u2588\u2588\u2554\u255d", u8" \u255a\u2550\u2550\u2550\u2550\u2550\u255d "},
      {u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2557", u8"\u2588\u2588\u2554\u2550\u2550\u2588\u2588\u2557", u8"\u2588\u2588\u2588\u2588\u2588\u2588\u2554\u255d", u8"\u2588\u2588\u2554\u2550\u2550\u2550\u255d", u8"\u2588\u2588\u2551", u8"\u255a\u2550\u255d"},
      {u8"\u2588\u2588\u2557   \u2588\u2588\u2557", u8"\u255a\u2588\u2588\u2557 \u2588\u2588\u2554\u255d", u8" \u255a\u2588\u2588\u2588\u2588\u2554\u255d ", u8"  \u255a\u2588\u2588\u2554\u255d  ", u8"   \u2588\u2588\u2551   ", u8"   \u255a\u2550\u255d   "},
  };

  vector<int> glyphWidths;
  glyphWidths.reserve(glyphs.size());
  for (const auto& glyph : glyphs) {
    int width = 0;
    for (const auto& line : glyph) width = max(width, ftxui::string_width(line));
    glyphWidths.push_back(width);
  }

  ftxui::Elements rows;
  for (size_t row = 0; row < 6; ++row) {
    string line;
    for (size_t glyphIndex = 0; glyphIndex < glyphs.size(); ++glyphIndex) {
      const auto& glyphLine = glyphs[glyphIndex][row];
      line += glyphLine;
      const auto padding = glyphWidths[glyphIndex] - ftxui::string_width(glyphLine);
      if (padding > 0) line.append(static_cast<size_t>(padding), ' ');
    }
    rows.push_back(styledText(line, onboardingGradientColor(static_cast<int>(row))));
  }
  return ftxui::vbox(move(rows));
}

}  // namespace

ftxui::Element App::renderOnboardingWordmark() const {
  return alignedOnboardingWordmark();
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
  onboardingActive_ = false;
  changePage(Page::Home);
  setMessage("Setup complete.", 5);
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
      rows.push_back(styledText("Starts with Windows and stays available in the notification area.", uiSecondaryText()));
      if (wizardTransition_.phase == WizardTransitionPhase::FadeOut && wizardSelectedOption_.has_value()) {
        rows.push_back(onboardingChoicePrompt(*wizardSelectedOption_, "Enable", "Skip"));
      } else {
        rows.push_back(onboardingPrompt("[ Y ] Enable   [ N ] Skip"));
      }
      break;
    case OnboardingStep::ScanR1:
      rows.push_back(uiHeaderText("Set up an Inventatory Scan R1?", uiTitleColor()));
      rows.push_back(styledText("Have the scanner nearby, powered on, with Bluetooth enabled.", uiSecondaryText()));
      if (wizardTransition_.phase == WizardTransitionPhase::FadeOut && wizardSelectedOption_.has_value()) {
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
  if (wizardTransition_.phase == WizardTransitionPhase::FadeOut) {
    const auto elapsed = max(0LL, uiAnimationTicks() - wizardTransition_.startedAt);
    const auto midpoint = wizardTransition_.durationMs / 2;
    const auto visible = 1.0F - clampUnit(static_cast<float>(elapsed) / static_cast<float>(midpoint));
    content = fadeWizardContent(move(content), visible);
  } else if (wizardTransition_.phase == WizardTransitionPhase::FadeIn) {
    const auto elapsed = max(0LL, uiAnimationTicks() - wizardTransition_.startedAt);
    const auto midpoint = wizardTransition_.durationMs / 2;
    const auto visible = clampUnit(static_cast<float>(elapsed - midpoint) / static_cast<float>(midpoint));
    content = fadeWizardContent(move(content), visible);
  }
  return renderOnboardingFrame(move(content));
}

void App::beginWizardTransition(Page targetPage, OnboardingStep targetOnboardingStep,
                                 ScanSetupStep targetScanSetupStep,
                                 bool targetReturnToOnboardingAfterScan) {
  if (wizardTransition_.phase != WizardTransitionPhase::None) return;
  wizardTransition_.phase = WizardTransitionPhase::FadeOut;
  wizardTransition_.startedAt = uiAnimationTicks();
  wizardTransition_.durationMs = page_ == Page::ScanSetup || targetPage == Page::ScanSetup
                                     ? kWizardSubstepCrossfadeDurationMs
                                     : kWizardCrossfadeDurationMs;
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
  const auto midpoint = wizardTransition_.durationMs / 2;
  if (wizardTransition_.phase == WizardTransitionPhase::FadeOut && elapsed >= midpoint) {
    applyWizardTransitionTarget();
    wizardTransition_.phase = WizardTransitionPhase::FadeIn;
  }
  if (elapsed < wizardTransition_.durationMs) {
    dirty_ = true;
    return;
  }

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
          advanceOnboarding();
          wizardSelectedOption_ = ch;
        }
      }
      return;
    case OnboardingStep::ScanR1:
      if (ch == 'y') {
        returnToOnboardingAfterScan_ = true;
        openInventatoryScanSetup();
        wizardSelectedOption_ = ch;
      } else if (ch == 'n') {
        advanceOnboarding();
        wizardSelectedOption_ = ch;
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
