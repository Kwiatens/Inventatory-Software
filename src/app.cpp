// Inventatory - Hardware Inventory Management System
// Terminal application controller and app-level state management.

#include "App.h"

#include "platform/DigiKeyApi.h"
#include "platform/CredentialStore.h"
#include "platform/StartupRegistration.h"
#include "platform/UpdateService.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <thread>
#include <future>

namespace inventatory {

using namespace std;

namespace {

string currentDateTimeText() {
  const auto now = time(nullptr);
  tm localTime{};
  localtime_s(&localTime, &now);
  char buffer[32]{};
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
  return buffer;
}

}  // namespace


KeyEvent translateEvent(const ftxui::Event& event) {
  if (event == ftxui::Event::Return) {
    return {KeyType::Enter, '\0'};
  }
  if (event == ftxui::Event::Escape) {
    return {KeyType::Escape, '\0'};
  }
  if (event == ftxui::Event::CtrlH) {
    return {KeyType::CtrlBackspace, '\0'};
  }
  if (event == ftxui::Event::CtrlZ) {
    return {KeyType::CtrlZ, '\0'};
  }
  if (event == ftxui::Event::Backspace) {
    if (controlModifierPressed()) {
      return {KeyType::CtrlBackspace, '\0'};
    }
    return {KeyType::Backspace, '\0'};
  }
  if (event == ftxui::Event::Tab) {
    return {KeyType::Tab, '\0'};
  }
  if (event == ftxui::Event::TabReverse) {
    return {KeyType::TabReverse, '\0'};
  }
  if (event == ftxui::Event::ArrowUp) {
    return {KeyType::Up, '\0'};
  }
  if (event == ftxui::Event::ArrowDown) {
    return {KeyType::Down, '\0'};
  }
  if (event == ftxui::Event::ArrowLeft) {
    return {KeyType::Left, '\0'};
  }
  if (event == ftxui::Event::ArrowRight) {
    return {KeyType::Right, '\0'};
  }
  if (event == ftxui::Event::Home) {
    return {KeyType::Home, '\0'};
  }
  if (event == ftxui::Event::End) {
    return {KeyType::End, '\0'};
  }
  if (event == ftxui::Event::PageUp) {
    return {KeyType::PageUp, '\0'};
  }
  if (event == ftxui::Event::PageDown) {
    return {KeyType::PageDown, '\0'};
  }
  if (event == ftxui::Event::Delete) {
    return {KeyType::Delete, '\0'};
  }
  if (event.is_character() && !event.character().empty()) {
    return {KeyType::Character, event.character()[0]};
  }
  return {KeyType::Unknown, '\0'};
}

App::App(bool startInBackground, BackgroundController& backgroundController)
    : backgroundController_(backgroundController),
      startInBackground_(startInBackground),
      root_(filesystem::current_path()),
      settingsPath_(appSettingsPath()),
      dataPath_(discoverInventatoryDataPath()),
      inventoryPath_(dataPath_ / "inventory.db"),
      printerPath_(dataPath_ / "printer.conf"),
      activityPath_(dataPath_ / "activity.tsv"),
      inventatoryScanConfigPath_(dataPath_ / "inventatory_scan.conf"),
      quickLabelsPath_(dataPath_ / "quick_labels.conf") {
  error_code settingsFileError;
  const bool settingsFileExists = filesystem::exists(settingsPath_, settingsFileError);
  const bool loadedSettings = loadAppSettings(settingsPath_, settings_);
  applyUiAppearance(settings_.appearance);
  if (loadedSettings && !settings_.dataDirectory.empty()) {
    dataPath_ = settings_.dataDirectory;
    inventoryPath_ = dataPath_ / "inventory.db";
    printerPath_ = dataPath_ / "printer.conf";
    activityPath_ = dataPath_ / "activity.tsv";
    inventatoryScanConfigPath_ = dataPath_ / "inventatory_scan.conf";
    quickLabelsPath_ = dataPath_ / "quick_labels.conf";
  } else {
    settings_.dataDirectory = dataPath_;
  }
  loadQuickLabels(quickLabelsPath_, settings_.quickLabelPresets, settings_.quickLabelRevision);
  settingsDraft_ = settings_;
  autoPrintScannedLabels_ = settings_.autoPrintScannedLabels;
  hasStoredDigiKeySecret_ = CredentialStore::read("digikey-client-secret").has_value();
  loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
  loadState();
  if (!loadedSettings) {
    settings_.printerQueue = printerService_.configuredPrinter();
    const auto environment = loadDigiKeyConfig();
    settings_.digiKeyClientId = environment.clientId;
    settings_.digiKeyAccountId = environment.accountId;
    settings_.digiKeySite = environment.site;
    settings_.digiKeyLanguage = environment.language;
    settings_.digiKeyCurrency = environment.currency;
    settingsDraft_ = settings_;
    if (!settingsFileExists) {
      saveAppSettings(settingsPath_, settings_);
    } else {
      setMessage("Settings file is invalid; defaults are in use temporarily. Finish setup or reset it explicitly.", 8);
    }
  } else if (!settings_.printerQueue.empty()) {
    printerService_.setConfiguredPrinter(settings_.printerQueue);
    printerCheck_ = printerService_.probeConfiguredPrinter();
  }
  if (onboardingRequired(startInBackground_, loadedSettings, settings_.completedOnboardingVersion)) {
    onboardingActive_ = true;
    page_ = Page::Onboarding;
  }
  if (!inventoryRecoveryRequired_) {
    server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                 appSettingsDirectory() / "inventatory-scan-replay.state");

    if (!server_.start(settings_.deviceServicePort,
                     [this](const DeviceSyncRequest& request, DeviceSyncResponse& response, string& error) {
                       return handleDeviceSync(request, response, error);
                     })) {
      setMessage("Inventatory Scan R1 service failed to start; terminal still works", 5);
    } else {
      if (mdnsService_.start(server_.port())) {
        setMessage("Inventatory Scan R1 service ready", 5);
      } else {
        setMessage("Inventatory Scan R1 service ready; network discovery unavailable", 5);
      }
    }
  }
  beginUpdateCheckIfDue();
}

// The application frame: header, content, context/search-or-actions, message.
// Only the content region varies per screen. There is no persistent action
// bar; press space to open the full action sheet for the current screen.
ftxui::Element App::renderUi() const {
  uiTargets_.clear();
  uiTargets_.reserve(512);
  // Fresh setup is intentionally a dedicated terminal surface. It must not
  // inherit any workspace navigation, operational state, or search chrome.
  if (page_ == Page::Onboarding || (page_ == Page::ScanSetup && returnToOnboardingAfterScan_)) {
    return renderWizardUi() | ftxui::flex | ftxui::bgcolor(uiCanvasBg());
  }
  if (inventoryRecoveryRequired_) {
    return ftxui::vbox({
        ftxui::filler(),
        styledText("INVENTORY RECOVERY REQUIRED", uiDangerColor()),
        ftxui::separator(),
        styledText(inventoryRecoveryDetail_, uiTitleColor()),
        styledText("The database was preserved and Inventatory is locked to prevent data loss.", uiMutedColor()),
        styledText("Press D to choose another Inventatory folder, or Esc to exit.", uiAccentColor()),
        ftxui::filler(),
    }) | ftxui::border | ftxui::bgcolor(uiCanvasBg());
  }
  const auto* active = ftxui::ScreenInteractive::Active();
  if (active != nullptr && (active->dimx() < 100 || active->dimy() < 30)) {
    return ftxui::vbox({
               ftxui::filler(),
               ftxui::hbox({ftxui::filler(),
                            styledText("Inventatory needs a terminal of at least 100 x 30", uiWarnColor()),
                            ftxui::filler()}),
               ftxui::hbox({ftxui::filler(),
                            styledText("Current: " + to_string(active->dimx()) + " x " +
                                           to_string(active->dimy()),
                                       uiMutedText()),
                            ftxui::filler()}),
               ftxui::filler(),
           }) |
           ftxui::bgcolor(uiCanvasBg());
  }
  ftxui::Elements body;
  body.push_back(renderHeaderUi());
  body.push_back(uiDivider());
  body.push_back(renderPageUi() | ftxui::flex);
  body.push_back(uiDivider());
  // Bottom sheet takes the place of the search/context line while open; the
  // content region above it flexes up to make room.
  if (inputMode_ == InputMode::ActionSheet) {
    body.push_back(renderActionSheetUi());
  } else {
    body.push_back(renderSearchBarUi());
  }
  body.push_back(renderMessageUi());
  // FTXUI sizes a root element from its natural content unless it is made
  // flexible. Keep the application background and every full-width chrome row
  // attached to the actual terminal width so no right-edge strip is exposed.
  return ftxui::vbox(move(body)) | ftxui::xflex | ftxui::bgcolor(uiCanvasBg());
}

// Human-readable name of the active screen, used in the header breadcrumb.
std::string App::pageName() const {
  switch (page_) {
    case Page::Home:
      return "Home";
    case Page::Stock:
      return "Stock";
    case Page::Racks:
      return "Racks";
    case Page::Import:
      return "Import";
    case Page::Projects:
      return "Projects";
    case Page::History:
      return "History";
    case Page::ScanSetup:
      return "Scan R1 Setup";
    case Page::DigiKeySetup:
      return "DigiKey Setup";
    case Page::Settings:
      return "Settings";
    case Page::Onboarding:
      return "First-time setup";
  }
  return "";
}

// Header region: navigation only. The action sheet remains keyboard-accessible
// through Space without taking permanent space from the dashboard.
ftxui::Element App::renderHeaderUi() const {
  auto self = const_cast<App*>(this);
  const auto nav = [&](Page page, const string& id, const string& label) {
    const bool activePage = page_ == page;
    auto item = activePage
                    ? uiHeaderText(" " + label + " ", uiFocusColor(), uiSelectionBg())
                    : uiBodyText(" " + label + " ", uiSecondaryText(), uiSurfaceBg());
    return target(item, id, UiTargetKind::Navigation, [self, page] { self->changePage(page); });
  };

  auto navigation = ftxui::hbox({
      uiHeaderText(" Inventatory ", uiPrimaryText()),
      nav(Page::Home, "nav.home", "1 Home"),
      nav(Page::Stock, "nav.stock", "2 Stock"),
      nav(Page::Racks, "nav.racks", "3 Racks"),
      nav(Page::Import, "nav.import", "4 Import"),
      nav(Page::Projects, "nav.projects", "5 Projects"),
      nav(Page::History, "nav.history", "6 History"),
      nav(Page::Settings, "nav.settings", "7 Settings"),
  });

  ftxui::Elements header;
  header.push_back(navigation);
  header.push_back(ftxui::filler());
  header.push_back(uiBodyText(" " + currentDateTimeText() + " ", uiSecondaryText()));

  return ftxui::hbox(move(header)) | ftxui::xflex | ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element App::renderPageUi() const {
  switch (page_) {
    case Page::Home:
      return renderDashboardUi();
    case Page::Stock:
      return renderStockUi();
    case Page::Racks:
      return renderRackManagementUi();
    case Page::Import:
      return renderImportCsvUi();
    case Page::Projects:
      return renderBomProjectUi();
    case Page::History:
      return renderHistoryUi();
    case Page::ScanSetup:
      return renderInventatoryScanSetupUi();
    case Page::DigiKeySetup:
      return renderDigiKeySetupUi();
    case Page::Settings:
      return renderSettingsUi();
    case Page::Onboarding:
      return renderOnboardingUi();
  }

  return ftxui::text("");
}

ftxui::Element App::renderSearchBarUi() const {
  if (page_ == Page::Home && inputMode_ == InputMode::None) {
    return fullLine("", uiMutedColor(), uiPanelLeftBg());
  }

  // Stock item editing renders inline in the Detail panel (see StockPage.cpp),
  // so this context line stays a plain search row instead of duplicating it.
  const bool stockEditing =
      page_ == Page::Stock && (inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue);
  const bool showsPrompt = !stockEditing &&
                            (inputMode_ == InputMode::EditValue || inputMode_ == InputMode::RackRename ||
                             inputMode_ == InputMode::RackType || inputMode_ == InputMode::RackCreate ||
                             inputMode_ == InputMode::RackJump || inputMode_ == InputMode::RackFilter ||
                             inputMode_ == InputMode::QuantityAdjust || inputMode_ == InputMode::StocktakeCount ||
                             inputMode_ == InputMode::HistoryCheckpoint || inputMode_ == InputMode::HistoryConfirm ||
                             inputMode_ == InputMode::ExitConfirmation);

  const auto activeBg = inputMode_ == InputMode::Search || showsPrompt ? uiRowSelectedBg() : uiPanelLeftBg();
  const auto bodyColor = inputMode_ == InputMode::Search ? uiTitleColor() : showsPrompt ? uiLinkColor() : uiMutedColor();
  string contextTitle = "Context";
  string contextText;
  if (inputMode_ == InputMode::Search) {
    contextTitle = "Search";
    contextText = "/" + inputBuffer_ + "_  (filtering live)";
  } else if (inputMode_ == InputMode::ExitConfirmation) {
    contextTitle = "Unsaved settings";
    contextText = activePrompt();
  } else if (inputMode_ == InputMode::HistoryConfirm) {
    contextTitle = "Confirm history action";
    contextText = activePrompt();
  } else if (showsPrompt) {
    contextText = activePrompt() + inputBuffer_ + "_";
  } else {
    switch (page_) {
      case Page::Home:
        contextText = to_string(store_.items().size()) + " parts";
        break;
      case Page::Stock:
        contextTitle = "Search";
        contextText = searchQuery_.empty() ? "/ type to filter" : "/" + searchQuery_;
        if (stockDateFilter_ != StockDateFilter::All) {
          contextText += " · " + stockDateFilterName(stockDateFilter_);
        }
        break;
      case Page::Racks:
        contextTitle = "Rack filter";
        contextText = rackFilter_.empty() ? "All racks" : rackFilter_;
        break;
      case Page::Import:
        contextTitle = "Import progress";
        contextText = importCandidates_.empty() ? "Choose a CSV file to begin"
                                                : to_string(importSelection_ + 1) + " / " +
                                                      to_string(importCandidates_.size()) + " rows";
        break;
      case Page::Projects:
        contextTitle = "Project";
        if (!bomAnalysisValid_) {
          contextText = bomProjects_.empty()
                            ? "Import a KiCad BOM to begin"
                            : to_string(bomProjects_.size()) +
                                  (bomProjects_.size() == 1 ? " project" : " projects");
        } else if (bomView_ == BomView::Build) {
          contextText = "build · " + to_string(bomBuildStep_ + 1) + " / " +
                        to_string(bomBuildSteps().size()) + " stops";
        } else {
          contextText = to_string(bomAnalysis_.lines.size()) + " lines · " +
                        to_string(bomAnalysis_.boards) +
                        (bomAnalysis_.boards == 1 ? " board · " : " boards · ") +
                        to_string(bomAnalysis_.readyCount) + " ready · " +
                        to_string(bomAnalysis_.shortCount) + " short";
        }
        break;
      case Page::History:
        contextTitle = "History";
        contextText = inventoryCommits_.empty()
                          ? "No inventory commits"
                          : "commit " + to_string(historySelection_ + 1) + " / " +
                                to_string(inventoryCommits_.size());
        break;
      case Page::ScanSetup:
        contextTitle = "Setup wizard";
        contextText = "Guided Bluetooth provisioning for Inventatory Scan R1";
        break;
      case Page::DigiKeySetup:
        contextTitle = "Setup wizard";
        contextText = "DigiKey credentials";
        break;
      case Page::Settings:
        contextTitle = "Settings";
        contextText = settingsCategoryName(settingsCategory_) +
                      (settingsDirty_ ? " · unsaved changes" : " · saved");
        break;
    }
  }

  ftxui::Elements rows;
  auto context = footerField(contextTitle, contextText, uiSecondaryText(), bodyColor, activeBg);
  if (page_ == Page::Stock && inputMode_ == InputMode::None) {
    auto self = const_cast<App*>(this);
    context = target(context, "stock.search", UiTargetKind::Field, [self] { self->startSearch(); });
  }
  rows.push_back(context);

  // The field-name list here is only for the CSV import review flow; Stock
  // item editing shows the same information inline in the Detail panel.
  if (inputMode_ == InputMode::EditFieldMenu && !stockEditing) {
    ftxui::Elements options;
    options.push_back(footerField("Edit fields", "\xE2\x86\x91\xE2\x86\x93 field  \xE2\x8F\x8E edit  s save  esc cancel",
                                  uiAccentColor(), uiMutedColor(), uiPanelLeftBg()));
    for (size_t index = 0; index < menuOptions_.size(); ++index) {
      const auto bg = static_cast<int>(index) == fieldMenuIndex_ ? uiRowSelectedBg() : (index % 2 == 0 ? uiRowDarkBg() : uiRowLightBg());
      auto option = fullLine("  " + menuOptions_[index].label,
                             static_cast<int>(index) == fieldMenuIndex_ ? uiTitleColor() : uiMutedColor(), bg);
      if (static_cast<int>(index) == fieldMenuIndex_) {
        option = option | ftxui::select;
      }
      options.push_back(option);
    }
    rows.push_back(ftxui::vbox(move(options)));
  }

  return ftxui::vbox(move(rows));
}

ftxui::Element App::renderMessageUi() const {
  const bool refreshingDigiKey = digiKeyRefreshTotal_ > 0 &&
                                 (!digiKeyRefreshQueue_.empty() || digiKeyRefreshFuture_.valid());
  if (refreshingDigiKey) {
    const auto completed = min(digiKeyRefreshCompleted_, digiKeyRefreshTotal_);
    return ftxui::hbox({
               styledText(" " + uiLoadingSpinner() + " DigiKey enrichment", uiLinkColor()),
               styledText("   ", uiLinkColor()),
               uiProgressBar(static_cast<double>(completed) / static_cast<double>(digiKeyRefreshTotal_), 28,
                             uiLinkColor()),
               styledText(" " + to_string(completed) + "/" + to_string(digiKeyRefreshTotal_) + " items",
                          uiMutedColor()),
               ftxui::filler(),
           }) |
           ftxui::bgcolor(uiPanelLeftBg());
  }
  if (importSyncRunning_) {
    const auto total = max<size_t>(1, importSyncTotal_);
    const auto completed = min(importSyncCompleted_, importSyncTotal_);
    return ftxui::hbox({
               styledText(" " + uiLoadingSpinner() + " DigiKey import sync", uiLinkColor()),
               styledText("   ", uiLinkColor()),
               uiProgressBar(static_cast<double>(completed) / static_cast<double>(total), 28, uiLinkColor()),
               styledText(" " + to_string(completed) + "/" + to_string(importSyncTotal_) + " rows",
                          uiMutedColor()),
               ftxui::filler(),
           }) |
           ftxui::bgcolor(uiPanelLeftBg());
  }
  const bool enrichingBom = page_ == Page::Projects && bomEnrichmentTotal_ > 0 &&
                            (!bomEnrichmentQueue_.empty() || bomEnrichmentFuture_.valid());
  if (enrichingBom) {
    const auto dispatched = bomEnrichmentTotal_ - bomEnrichmentQueue_.size();
    return ftxui::hbox({
        styledText(" " + uiLoadingSpinner() + " DigiKey lookup", uiLinkColor()),
        uiProgressBar(static_cast<double>(dispatched) / static_cast<double>(bomEnrichmentTotal_), 28, uiLinkColor()),
        styledText(" " + to_string(dispatched) + "/" + to_string(bomEnrichmentTotal_) + " suggestions", uiMutedColor()),
        ftxui::filler(),
    }) | ftxui::bgcolor(uiPanelLeftBg());
  }
  if (!persistenceError_.empty()) {
    return fullLine(persistenceError_, uiDangerColor(), uiPanelLeftBg());
  }
  if (message_.empty()) {
    return ftxui::text("");
  }

  // Keep the pulse aligned with the 100 ms redraw ticker so it reads as a
  // regular confirmation animation instead of an uneven flicker.
  constexpr long long kFlashHalfPeriodMs = 200;
  constexpr long long kFlashCycles = 3;
  const auto elapsed = max(0LL, uiAnimationTicks() - messageFlashStartedAt_);
  const auto flashDuration = kFlashHalfPeriodMs * kFlashCycles * 2;
  const bool flashing = messageFlashStartedAt_ >= 0 && elapsed < flashDuration &&
                        ((elapsed / kFlashHalfPeriodMs) % 2 == 0);
  return ftxui::hbox({
             styledText(message_, flashing ? uiCanvasBg() : uiAccentColor(),
                        flashing ? uiInteractiveColor() : uiPanelLeftBg()),
             ftxui::filler(),
         }) |
         ftxui::bgcolor(uiPanelLeftBg());
}

int App::run() {
#if defined(_WIN32)
  // Windows Terminal does not advertise truecolor through TERM/COLORTERM.
  // Select it before any semantic palette helper constructs an RGB color;
  // otherwise FTXUI collapses the graphite/cyan palette to ANSI colors.
  ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
#endif
  running_ = true;
  // Rewrite the per-user startup entry on every launch so installations that
  // used the old direct console entry are migrated to the headless launcher.
  string startupError;
  if (!setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, startupError)) {
    setMessage("Unable to update Windows startup: " + startupError, 5);
  }
  // Windows sign-in launches this process with --background. Keep that path
  // free of FTXUI so only the Scan R1 bridge and notification-area handler run.
  backgroundController_.start(startInBackground_ || settings_.backgroundServiceEnabled, startInBackground_, [this] {
    backgroundQuitRequested_.store(true);
  }, [this] { foregroundRequested_.store(true); });

  if (startInBackground_) {
    runBackgroundLoop();
  } else {
    runInteractiveLoop();
  }

  saveState();
  backgroundController_.stop();
  mdnsService_.stop();
  server_.stop();
  return 0;
}

void App::processBackgroundWork() {
  if (inventoryRecoveryRequired_) {
    clearMessageIfExpired();
    return;
  }
  processScans();
  processDeviceRequests();
  processDeviceSyncEvents();
  updateDashboardScannerState();
  clearMessageIfExpired();
  clearDeleteConfirmationIfExpired();
  processUpdateCheck();
  processScanFirmwareCheck();
  processScanDigiKeyEnrichment();
  processDigiKeyRefresh();
  processImportSync();
  processBomEnrichment();
}

void App::runBackgroundLoop() {
  while (running_) {
    if (backgroundQuitRequested_.exchange(false)) {
      running_ = false;
      break;
    }
    if (foregroundRequested_.exchange(false)) {
      runInteractiveLoop();
      continue;
    }
    processBackgroundWork();
    this_thread::sleep_for(chrono::milliseconds(100));
  }
}

void App::runInteractiveLoop() {
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  screen.ForceHandleCtrlZ(false);
  screen.TrackMouse();
  auto renderer = ftxui::Renderer([this] { return renderUi(); });
  auto component = ftxui::CatchEvent(renderer, [this, &screen](ftxui::Event event) {
    if (event == ftxui::Event::Custom) {
      if (backgroundQuitRequested_.exchange(false)) {
        running_ = false;
        screen.ExitLoopClosure()();
        return true;
      }
      updateWizardTransition();
      processBackgroundWork();
      return true;
    }

    if (event.is_mouse()) {
      return handleMouse(event.mouse());
    }

    const auto key = translateEvent(event);
    if (key.type == KeyType::Unknown && event != ftxui::Event::Escape) {
      return false;
    }

    handleKey(key);
    if (!running_) {
      screen.ExitLoopClosure()();
    }
    return true;
  });

  thread ticker([this, &screen] {
    while (running_) {
      screen.PostEvent(ftxui::Event::Custom);
      this_thread::sleep_for(chrono::milliseconds(100));
    }
  });

  screen.Loop(component);
  running_ = false;
  if (ticker.joinable()) {
    ticker.join();
  }
}

void App::requestUserExit() {
  if (importSyncRunning_) {
    setMessage("DigiKey sync is still running; cancel it or wait for completion", 4);
    return;
  }
  if (settingsDirty_) {
    pendingPageAfterSettings_.reset();
    inputMode_ = InputMode::ExitConfirmation;
    setMessage("Unsaved settings: press S to save, D to discard, or Esc to stay", 5);
    return;
  }
  if (backgroundController_.enabled()) {
    backgroundController_.hideConsole(true);
  } else {
    running_ = false;
  }
}

void App::restartDeviceService() {
  mdnsService_.stop();
  server_.stop();
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                               appSettingsDirectory() / "inventatory-scan-replay.state");
  if (!server_.start(settings_.deviceServicePort,
                     [this](const DeviceSyncRequest& request, DeviceSyncResponse& response, string& error) {
                       return handleDeviceSync(request, response, error);
                     })) {
    setMessage("Inventatory Scan R1 service failed to restart; terminal still works", 6);
    return;
  }
  if (mdnsService_.start(server_.port())) {
    setMessage("Inventatory Scan R1 bridge restarted on port " + to_string(server_.port()), 5);
  } else {
    setMessage("Inventatory Scan R1 bridge restarted; network discovery unavailable", 5);
  }
  dirty_ = true;
}

void App::completeSettingsExit(bool saveChanges) {
  if (saveChanges) {
    if (!saveSettingsDraft()) return;
  } else {
    cancelSettingsDraft();
  }
  const auto destination = pendingPageAfterSettings_;
  pendingPageAfterSettings_.reset();
  inputMode_ = InputMode::None;
  if (destination.has_value()) {
    changePage(*destination);
  } else {
    requestUserExit();
  }
}

void App::handleKey(const KeyEvent& key) {
  if (inventoryRecoveryRequired_) {
    if (key.type == KeyType::Escape) running_ = false;
    if (key.type == KeyType::Character && (key.ch == 'd' || key.ch == 'D')) chooseInventatoryFolder();
    dirty_ = true;
    return;
  }
  if (wizardTransition_.phase != WizardTransitionPhase::None) {
    if (!bufferedWizardKey_.has_value()) bufferedWizardKey_ = key;
    return;
  }
  switch (inputMode_) {
    case InputMode::Search:
      handleSearchKey(key);
      return;
    case InputMode::EditFieldMenu:
      handleEditMenuKey(key);
      return;
    case InputMode::EditValue:
      handleEditValueKey(key);
      return;
    case InputMode::RackRename:
    case InputMode::RackType:
    case InputMode::RackCreate:
    case InputMode::RackJump:
    case InputMode::RackFilter:
      handleRackValueKey(key);
      return;
    case InputMode::StockFilter:
      handleStockFilterKey(key);
      return;
    case InputMode::StocktakeCount:
      handleStocktakeCountKey(key);
      return;
    case InputMode::QuantityAdjust:
      handleQuantityAdjustKey(key);
      return;
    case InputMode::BomRestock:
      handleBomRestockKey(key);
      return;
    case InputMode::HistoryCheckpoint:
    case InputMode::HistoryConfirm:
      handleHistoryKey(key);
      return;
    case InputMode::ExitConfirmation:
      handleExitConfirmationKey(key);
      return;
    case InputMode::ActionSheet:
      handleActionSheetKey(key);
      return;
    case InputMode::None:
      break;
  }

  // Settings fields use their own staged editor rather than a shared
  // InputMode. While it is active, all characters (including 1-5) and Enter
  // belong to the field and must not trigger global navigation or focus.
  if (page_ == Page::Settings && (settingsEditingField_ || appearancePickerOpen_)) {
    handleSettingsKey(key);
    return;
  }

  // The setup wizard owns every key while open: its Wi-Fi password and the
  // six-digit verification code must never be interpreted as global shortcuts.
  if (page_ == Page::ScanSetup) {
    handleInventatoryScanSetupKey(key);
    return;
  }

  if (page_ == Page::DigiKeySetup) {
    handleDigiKeySetupKey(key);
    return;
  }

  if (page_ == Page::Onboarding) {
    handleOnboardingKey(key);
    return;
  }

  if (page_ == Page::Import && importCommitPending_ && key.type == KeyType::Character && key.ch == 'R') {
    finishImportReview();
    return;
  }
  if (key.type == KeyType::Character && key.ch == 'R' && !persistenceError_.empty()) {
    retrySaveState();
    return;
  }

  // Delete confirmation is modal. Route Enter/Escape to the confirmation
  // handler before the normal focus system can activate an underlying target.
  if (page_ == Page::Stock && deleteConfirmationActive()) {
    handleStockKey(key);
    return;
  }

  if (key.type == KeyType::Tab) {
    moveUiFocus(1);
    return;
  }
  if (key.type == KeyType::TabReverse) {
    moveUiFocus(-1);
    return;
  }
  if (key.type == KeyType::Enter && activateFocusedTarget()) {
    return;
  }

  if (key.type == KeyType::Character) {
    switch (key.ch) {
      case '1': changePage(Page::Home); return;
      case '2': changePage(Page::Stock); return;
      case '3': changePage(Page::Racks); return;
      case '4': changePage(Page::Import); return;
      case '5': changePage(Page::Projects); return;
      case '6': changePage(Page::History); return;
      case '7': changePage(Page::Settings); return;
      default: break;
    }
  }

  // Open the action sheet, or run a contextual action by its accelerator.
  // Registry actions win over per-page handlers, keeping one source of truth.
  if (key.type == KeyType::Character && key.ch == ' ') {
    openActionSheet();
    return;
  }
  if (dispatchAction(key)) {
    return;
  }

  switch (page_) {
    case Page::Home:
      handleDashboardKey(key);
      break;
    case Page::Stock:
      handleStockKey(key);
      break;
    case Page::Racks:
      handleRackManagementKey(key);
      break;
    case Page::Import:
      handleImportCsvKey(key);
      break;
    case Page::Projects:
      handleBomProjectKey(key);
      break;
    case Page::History:
      handleHistoryKey(key);
      break;
    case Page::ScanSetup:
      handleInventatoryScanSetupKey(key);
      break;
    case Page::DigiKeySetup:
      handleDigiKeySetupKey(key);
      break;
    case Page::Settings:
      handleSettingsKey(key);
      break;
  }
}

ftxui::Element App::target(ftxui::Element element, string id, UiTargetKind kind,
                           function<void()> activate, bool enabled, bool focusable) const {
  const auto index = uiTargets_.size();
  const bool hovered = hoveredTargetId_ == id;
  const bool focused = focusable && static_cast<int>(index) == focusedTarget_;
  if (hovered) element = element | ftxui::bgcolor(uiHoverBg());
  if (focused) element = element | ftxui::color(uiFocusColor()) | ftxui::bgcolor(uiSelectionBg()) | ftxui::bold;
  uiTargets_.push_back(UiTarget{move(id), kind, {}, enabled, focusable, move(activate)});
  return element | ftxui::reflect(uiTargets_.back().bounds);
}

bool App::handleMouse(const ftxui::Mouse& mouse) {
  const auto contains = [&](const UiTarget& target) {
    return uiBoxContains(target.bounds, mouse.x, mouse.y);
  };

  string hovered;
  for (auto it = uiTargets_.rbegin(); it != uiTargets_.rend(); ++it) {
    if (it->enabled && contains(*it)) {
      hovered = it->id;
      break;
    }
  }
  if (hoveredTargetId_ != hovered) {
    hoveredTargetId_ = hovered;
    dirty_ = true;
  }

  if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
    const int delta = mouse.button == ftxui::Mouse::WheelUp ? -1 : 1;
    if (page_ == Page::Stock) moveSelection(delta);
    else if (page_ == Page::Home && inputMode_ == InputMode::None) {
      if (uiBoxContains(dashboardWarningPanelBounds_, mouse.x, mouse.y)) {
        moveDashboardSelection(DashboardList::Warnings, delta);
      } else if (uiBoxContains(dashboardActivityPanelBounds_, mouse.x, mouse.y)) {
        moveDashboardSelection(DashboardList::Commits, delta);
      }
    }
    else if (page_ == Page::Import) moveImportSelection(delta);
    else if (page_ == Page::Projects) {
      if (bomView_ == BomView::Split) {
        if (uiBoxContains(bomReadyPanelBounds_, mouse.x, mouse.y)) {
          bomSplitShortFocused_ = false;
          moveBomSelection(delta);
        } else if (uiBoxContains(bomShortPanelBounds_, mouse.x, mouse.y)) {
          bomSplitShortFocused_ = true;
          moveBomSelection(delta);
        }
      } else {
        moveBomSelection(delta);
      }
    }
    else if (page_ == Page::Settings && settingsCategory_ == SettingsCategory::Printer) {
      if (delta < 0 && printerSelection_ > 0) --printerSelection_;
      if (delta > 0 && printerSelection_ + 1 < printerQueues_.size()) ++printerSelection_;
      dirty_ = true;
    }
    return true;
  }

  if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
    for (size_t reverse = uiTargets_.size(); reverse > 0; --reverse) {
      auto& hit = uiTargets_[reverse - 1];
      if (hit.enabled && contains(hit)) {
        focusedTarget_ = static_cast<int>(reverse - 1);
        hit.activate();
        dirty_ = true;
        return true;
      }
    }
  }
  return !hovered.empty();
}

void App::moveUiFocus(int delta) {
  if (uiTargets_.empty()) return;
  if (focusedTarget_ < 0 && page_ == Page::Import) {
    const auto primary = find_if(uiTargets_.begin(), uiTargets_.end(), [](const UiTarget& target) {
      return target.enabled && target.focusable && target.id == "import.choose";
    });
    if (primary != uiTargets_.end()) {
      focusedTarget_ = static_cast<int>(distance(uiTargets_.begin(), primary));
      dirty_ = true;
      return;
    }
  }
  int next = focusedTarget_;
  for (size_t count = 0; count < uiTargets_.size(); ++count) {
    next = (next + delta + static_cast<int>(uiTargets_.size())) % static_cast<int>(uiTargets_.size());
    if (uiTargets_[static_cast<size_t>(next)].enabled && uiTargets_[static_cast<size_t>(next)].focusable) {
      focusedTarget_ = next;
      dirty_ = true;
      return;
    }
  }
}

bool App::activateFocusedTarget() {
  if (focusedTarget_ < 0 || focusedTarget_ >= static_cast<int>(uiTargets_.size())) return false;
  auto& selected = uiTargets_[static_cast<size_t>(focusedTarget_)];
  if (!selected.enabled || !selected.focusable || !selected.activate) return false;
  selected.activate();
  dirty_ = true;
  return true;
}

void App::handleSearchKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    inputBuffer_.push_back(key.ch);
    searchQuery_ = inputBuffer_;
    syncSelectionToFilter();
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      searchQuery_ = inputBuffer_;
      syncSelectionToFilter();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    inputMode_ = InputMode::None;
    syncSelectionToFilter();
    setMessage(searchQuery_.empty() ? "Filter cleared" : "Filter kept", 2);
    return;
  }

  if (key.type == KeyType::Escape) {
    searchQuery_ = searchQueryBeforeEdit_;
    inputBuffer_ = searchQuery_;
    inputMode_ = InputMode::None;
    syncSelectionToFilter();
    setMessage("Search cancelled", 2);
  }
}

void App::handleEditMenuKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (key.ch == 's' || key.ch == 'S') {
      saveWorkingCopy();
    }
    return;
  }

  if (key.type == KeyType::Up) {
    fieldMenuIndex_ = max(0, fieldMenuIndex_ - 1);
    dirty_ = true;
  } else if (key.type == KeyType::Down) {
    fieldMenuIndex_ = min(fieldMenuIndex_ + 1, static_cast<int>(menuOptions_.size()) - 1);
    dirty_ = true;
  } else if (key.type == KeyType::Enter) {
    if (fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
      inputBuffer_ = currentFieldValue(menuOptions_[fieldMenuIndex_].field);
      inputMode_ = InputMode::EditValue;
      setMessage("Editing " + fieldLabel(menuOptions_[fieldMenuIndex_].field), 2);
    }
  } else if (key.type == KeyType::Escape) {
    cancelInput();
  }
}

void App::handleEditValueKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    inputBuffer_.push_back(key.ch);
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    if (fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
      commitEditField(menuOptions_[fieldMenuIndex_].field, inputBuffer_);
    }
    inputBuffer_.clear();
    inputMode_ = InputMode::EditFieldMenu;
    return;
  }

  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::EditFieldMenu;
    setMessage("Edit cancelled", 2);
  }
}

void App::handleRackValueKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    inputBuffer_.push_back(key.ch);
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    const auto value = inputBuffer_;
    const auto mode = inputMode_;
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    switch (mode) {
      case InputMode::RackRename:
        renameSelectedRack(value);
        break;
      case InputMode::RackType:
        changeSelectedRackType(value);
        break;
      case InputMode::RackCreate:
        createRackWithType(value);
        break;
      case InputMode::RackJump:
        jumpToRack(value);
        break;
      case InputMode::RackFilter:
        rackFilter_ = trim(value);
        syncRackSelection();
        setMessage(rackFilter_.empty() ? "Rack filter cleared" : "Rack filter applied", 2);
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Rack input cancelled", 2);
  }
}

void App::handleQuantityAdjustKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (isdigit(static_cast<unsigned char>(key.ch)) != 0 && inputBuffer_.size() < 9U) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) inputBuffer_.pop_back();
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Enter) {
    const auto value = inputBuffer_;
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setSelectedQuantityFromInput(value);
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Quantity change cancelled", 2);
  }
}

void App::handleBomRestockKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (isdigit(static_cast<unsigned char>(key.ch)) != 0 && inputBuffer_.size() < 9U) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) inputBuffer_.pop_back();
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    bomRestockItemId_.clear();
    inputMode_ = InputMode::None;
    setMessage("Stock receipt cancelled", 2);
    return;
  }
  if (key.type != KeyType::Enter) return;

  const auto text = inputBuffer_;
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  auto* item = store_.findById(bomRestockItemId_);
  bomRestockItemId_.clear();
  if (item == nullptr) {
    setMessage("The matched stock item no longer exists; add it manually from Stock", 5);
    return;
  }
  int received = 0;
  try {
    size_t parsed = 0;
    received = stoi(text, &parsed);
    if (parsed != text.size() || received <= 0) throw invalid_argument("quantity");
  } catch (...) {
    setMessage("Enter a positive received quantity", 3);
    return;
  }

  captureUndoSnapshot();
  const auto before = item->quantity;
  item->quantity = before > numeric_limits<int>::max() - received ? numeric_limits<int>::max() : before + received;
  item->lastUpdated = time(nullptr);
  reconcileRackAssignment(store_, *item);
  logActivity("receipt", item->partName + " received " + to_string(received) + " (now " +
                              to_string(item->quantity) + ")");
  const bool saved = saveState("stock_receipt", activeBomProjectId_);
  refreshBomAnalysis();
  setMessage(saved ? item->partName + " receipt saved; BOM re-analyzed"
                   : "Receipt is in memory; press R to retry saving, then recheck the BOM",
             6);
  dirty_ = true;
}

void App::handleExitConfirmationKey(const KeyEvent& key) {
  if (key.type == KeyType::Escape) {
    pendingPageAfterSettings_.reset();
    inputMode_ = InputMode::None;
    setMessage("Stayed on the current screen", 2);
    return;
  }
  if (key.type != KeyType::Character) return;
  const auto ch = static_cast<char>(tolower(static_cast<unsigned char>(key.ch)));
  if (ch == 's') {
    completeSettingsExit(true);
  } else if (ch == 'd') {
    completeSettingsExit(false);
  }
}

}  // namespace inventatory
