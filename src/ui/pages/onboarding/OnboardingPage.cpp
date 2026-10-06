// Inventatory - Focused first-run terminal setup for fresh installs.

#include "App.h"

#include "platform/system/StartupRegistration.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>

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

// The ›i lockup, drawn on the website's pixel grid: one letter pixel is two
// characters wide and one row tall, with a drop shadow one character right
// and half a row down. The rows are the desktop wordmark's solid blocks with
// its box-drawing shadow removed, after the chevron and a one-pixel gap.
constexpr int kLetterRows = 5;
constexpr int kLockupStemColumn = 8;
constexpr int kLockupColumns = 100;  // the widest row plus its shadow column
constexpr array<const char*, kLetterRows> kLockupPixels = {
    "##      ## ###    ## ##    ## ####### ###    ## ########  #####  ########  ######  ######  ##    ##",
    "  ##    ## ####   ## ##    ## ##      ####   ##    ##    ##   ##    ##    ##    ## ##   ##  ##  ##",
    "    ##  ## ## ##  ## ##    ## #####   ## ##  ##    ##    #######    ##    ##    ## ######    ####",
    "  ##    ## ##  ## ##  ##  ##  ##      ##  ## ##    ##    ##   ##    ##    ##    ## ##   ##    ##",
    "##      ## ##   ####   ####   ####### ##   ####    ##    ##   ##    ##     ######  ##   ##    ##",
};
constexpr double kShadowToCanvas = 0.62;
// After a successful update the graphite lockup fades once into the accent gradient.
constexpr long long kRevealDelayMs = 400;
constexpr long long kRevealDurationMs = 900;
// The dot animates in whole steps, like pixels switching, at the UI tick rate.
constexpr long long kDotStepMs = 200;
constexpr long long kDotBlinkMs = 400;

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

uint32_t appearanceRgb(AppearanceColorRole role) {
  return activeUiAppearance().colors[static_cast<size_t>(role)];
}

bool lockupPixel(int row, int column) {
  if (row < 0 || row >= kLetterRows || column < 0) return false;
  const char* pixels = kLockupPixels[static_cast<size_t>(row)];
  return static_cast<size_t>(column) < strlen(pixels) && pixels[column] == '#';
}

// Graphite at rest; `reveal` (0 to 1) blends each row toward the accent gradient.
uint32_t lockupRowRgb(int row, double reveal) {
  const auto graphite = appearanceRgb(AppearanceColorRole::Divider);
  const auto gradient = blendRgb(appearanceRgb(AppearanceColorRole::Interactive),
                                 appearanceRgb(AppearanceColorRole::FocusText),
                                 static_cast<double>(row) / (kLetterRows - 1));
  return blendRgb(graphite, gradient, reveal);
}

// The i's dot is one letter pixel split into four rack slots, drawn as two
// half-block cells: each cell's foreground is its top slot, its background the
// bottom slot. Slots are in reading order: top-left, top-right, bottom-left,
// bottom-right. An empty optional leaves the slot as canvas.
using WordmarkDot = array<optional<uint32_t>, 4>;

ftxui::Element wordmarkDotRow(const WordmarkDot& slots) {
  const auto canvas = appearanceRgb(AppearanceColorRole::CanvasBg);
  const auto cell = [canvas](const optional<uint32_t>& top, const optional<uint32_t>& bottom) {
    return ftxui::text(u8"▀") | ftxui::color(rgbColor(top.value_or(canvas))) |
           ftxui::bgcolor(rgbColor(bottom.value_or(canvas)));
  };
  return ftxui::hbox({
      ftxui::text(string(kLockupStemColumn, ' ')),
      cell(slots[0], slots[2]),
      cell(slots[1], slots[3]),
      ftxui::text(string(kLockupColumns - kLockupStemColumn - 2, ' ')),
  });
}

// One terminal row of the lockup. Each cell shows its top and bottom half
// separately so the half-row drop shadow lines up with the pixels above it.
ftxui::Element lockupRow(int row, double reveal) {
  const auto canvas = appearanceRgb(AppearanceColorRole::CanvasBg);
  const auto face = [reveal](int pixelRow) { return lockupRowRgb(pixelRow, reveal); };
  const auto shadow = [&](int pixelRow) { return blendRgb(face(pixelRow), canvas, kShadowToCanvas); };
  ftxui::Elements runs;
  string run;
  uint32_t runTop = canvas;
  uint32_t runBottom = canvas;
  const auto flush = [&] {
    if (run.empty()) return;
    runs.push_back(ftxui::text(run) | ftxui::color(rgbColor(runTop)) | ftxui::bgcolor(rgbColor(runBottom)));
    run.clear();
  };
  for (int column = 0; column < kLockupColumns; ++column) {
    uint32_t top = canvas;
    uint32_t bottom = canvas;
    if (lockupPixel(row, column)) {
      top = bottom = face(row);
    } else {
      if (lockupPixel(row - 1, column - 1)) top = shadow(row - 1);
      if (lockupPixel(row, column - 1)) bottom = shadow(row);
    }
    if (top != runTop || bottom != runBottom) {
      flush();
      runTop = top;
      runBottom = bottom;
    }
    run += top == canvas && bottom == canvas ? " " : u8"▀";
  }
  flush();
  return ftxui::hbox(move(runs));
}

ftxui::Element alignedOnboardingWordmark(double reveal, const WordmarkDot& dot) {
  ftxui::Elements renderedRows;
  // The dot row and the gap below it are always reserved so the lockup never
  // moves when the dot appears. The last row holds only the shadow's lower half.
  renderedRows.push_back(wordmarkDotRow(dot));
  renderedRows.push_back(ftxui::text(" "));
  for (int row = 0; row <= kLetterRows; ++row) renderedRows.push_back(lockupRow(row, reveal));
  return ftxui::vbox(move(renderedRows));
}

// A lit slot, and an empty one: the accent at 18 % over the canvas.
uint32_t dotLitRgb() { return appearanceRgb(AppearanceColorRole::Interactive); }
uint32_t dotEmptyRgb() {
  return blendRgb(appearanceRgb(AppearanceColorRole::CanvasBg), appearanceRgb(AppearanceColorRole::Interactive), 0.18);
}

// Work of unknown length: one lit slot travels clockwise around empty slots.
WordmarkDot busyDot(long long ticks) {
  static constexpr array<size_t, 4> kClockwise = {0, 1, 3, 2};
  WordmarkDot dot;
  dot.fill(dotEmptyRgb());
  dot[kClockwise[static_cast<size_t>((ticks / kDotStepMs) % 4)]] = dotLitRgb();
  return dot;
}

// A download of known size: slots fill in reading order, one per quarter, and
// the slot being filled blinks.
WordmarkDot progressDot(double fraction, long long ticks) {
  const auto filled = static_cast<size_t>(clamp(fraction, 0.0, 1.0) * 4.0);
  WordmarkDot dot;
  for (size_t slot = 0; slot < dot.size(); ++slot) {
    const bool blinking = slot == filled && (ticks / kDotBlinkMs) % 2 == 0;
    dot[slot] = slot < filled || blinking ? dotLitRgb() : dotEmptyRgb();
  }
  return dot;
}

WordmarkDot failedDot() {
  WordmarkDot dot;
  dot.fill(appearanceRgb(AppearanceColorRole::WarningText));
  return dot;
}

}  // namespace

bool App::wordmarkRevealRunning() const {
  const auto startedAt = wordmarkRevealStartedAt_.load();
  return startedAt >= 0 && uiAnimationTicks() - startedAt < kRevealDelayMs + kRevealDurationMs;
}

ftxui::Element App::renderOnboardingWordmark() const {
  if (page_ != Page::Update) return alignedOnboardingWordmark(0.0, {});
  const auto ticks = uiAnimationTicks();
  switch (updateStep_) {
    case UpdateWizardStep::Preparing:
    case UpdateWizardStep::Verifying:
    case UpdateWizardStep::HandingOff:
      return alignedOnboardingWordmark(0.0, busyDot(ticks));
    case UpdateWizardStep::Downloading: {
      uint64_t downloaded = 0;
      uint64_t total = 0;
      if (updateDownloadState_ != nullptr) {
        lock_guard<mutex> lock(updateDownloadState_->mutex);
        downloaded = updateDownloadState_->downloadedBytes;
        total = updateDownloadState_->totalBytes;
      }
      if (total == 0) return alignedOnboardingWordmark(0.0, busyDot(ticks));
      return alignedOnboardingWordmark(0.0, progressDot(static_cast<double>(downloaded) / static_cast<double>(total), ticks));
    }
    case UpdateWizardStep::Failed:
      return alignedOnboardingWordmark(0.0, failedDot());
    case UpdateWizardStep::Complete: {
      // Success is the one moment the lockup takes its colour: a single fade, no dot.
      const auto startedAt = wordmarkRevealStartedAt_.load();
      if (startedAt < 0) return alignedOnboardingWordmark(1.0, {});
      const auto linear = clamp(static_cast<double>(ticks - startedAt - kRevealDelayMs) / kRevealDurationMs, 0.0, 1.0);
      return alignedOnboardingWordmark(linear * linear * (3.0 - 2.0 * linear), {});
    }
    default:
      return alignedOnboardingWordmark(0.0, {});
  }
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
#ifdef _WIN32
  // The installer owns the Windows shortcuts and only creates the desktop one when asked to, so the
  // wizard must not add it back after the installer user declined it.
  changePage(Page::Stock);
  setMessage("Setup complete.", 5);
#else
  string shortcutError;
  const bool shortcutCreated = createDesktopShortcut(shortcutError);
  changePage(Page::Stock);
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
      rows.push_back(styledText("Current folder: " + dataPath_.u8string(), uiSecondaryText()));
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
  focusedTargetId_.clear();
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
  updateCheckFailed_ = false;
  updateCheckFuture_ = async(launch::async, [version = softwareVersion()] { return checkLatestRelease(version); });
  beginScanFirmwareCheck();
}

void App::beginUpdateChecks() {
  bool started = false;
  if (!updateCheckFuture_.valid()) {
    // A manual check is always allowed, even when the daily background check
    // preference is disabled.
    settings_.lastUpdateCheckUnixSeconds = 0;
    updateCheckFailed_ = false;
    updateCheckFuture_ = async(launch::async, [version = softwareVersion()] { return checkLatestRelease(version); });
    started = true;
  }
  beginScanFirmwareCheck();
  setMessage(started ? "Checking for software updates" : "Already checking for software updates", 4);
  dirty_ = true;
}

void App::processUpdateCheck() {
  if (!updateCheckFuture_.valid() || updateCheckFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
  const auto result = updateCheckFuture_.get();
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
