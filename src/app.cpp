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
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
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
      inventatoryScanConfigPath_(dataPath_ / "inventatory_scan.conf") {
  loadEnvironmentFile(locateDotEnvFile());
  const bool loadedSettings = loadAppSettings(settingsPath_, settings_);
  if (loadedSettings && !settings_.dataDirectory.empty()) {
    dataPath_ = settings_.dataDirectory;
    inventoryPath_ = dataPath_ / "inventory.db";
    printerPath_ = dataPath_ / "printer.conf";
    activityPath_ = dataPath_ / "activity.tsv";
    inventatoryScanConfigPath_ = dataPath_ / "inventatory_scan.conf";
  } else {
    settings_.dataDirectory = dataPath_;
  }
  settingsDraft_ = settings_;
  autoPrintScannedLabels_ = settings_.autoPrintScannedLabels;
  hasStoredDigiKeySecret_ = CredentialStore::read("digikey-client-secret").has_value() ||
                            !loadDigiKeyConfig().clientSecret.empty();
  ensureInventoryDatabaseCopied(inventoryPath_);
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
    saveAppSettings(settingsPath_, settings_);
  } else if (!settings_.printerQueue.empty()) {
    printerService_.setConfiguredPrinter(settings_.printerQueue);
    printerCheck_ = printerService_.probeConfiguredPrinter();
  }
  // Existing settings belong to established users; mark them complete when
  // migrating so the first-run wizard only appears for fresh installs.
  if (loadedSettings && settings_.completedOnboardingVersion == 0) {
    settings_.completedOnboardingVersion = 1;
    settingsDraft_ = settings_;
    saveAppSettings(settingsPath_, settings_);
  }
  if (!startInBackground_ && !loadedSettings) {
    onboardingActive_ = true;
    page_ = Page::Onboarding;
  }
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token);

  if (!server_.start(settings_.deviceServicePort, [this](const DeviceScanRequest& request) { pushScanCode(request); },
                     [this](const DeviceQuantityRequest& request) { return enqueueDeviceQuantity(request); },
                     [this](const DeviceStatusReport& report) { enqueueDeviceStatus(report); },
                     [this](const DeviceDebugReport& report) { enqueueDeviceDebug(report); },
                     [this](const DeviceSyncRequest& request, DeviceSyncResponse& response, string& error) {
                       return handleDeviceSync(request, response, error);
                     })) {
    setMessage("Inventatory Scan R1 service failed to start; terminal still works", 5);
  } else {
    mdnsService_.start(server_.port());
    setMessage("Inventatory Scan R1 service ready", 5);
  }
  beginUpdateCheckIfDue();
}

// The application frame: header, content, context/search-or-actions, message.
// Only the content region varies per screen. There is no persistent action
// bar; press space to open the full action sheet for the current screen.
ftxui::Element App::renderUi() const {
  uiTargets_.clear();
  uiTargets_.reserve(512);
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
  return ftxui::vbox(move(body)) | ftxui::bgcolor(uiCanvasBg());
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
    case Page::ScanSetup:
      return "Scan R1 Setup";
    case Page::Settings:
      return "Settings";
    case Page::Onboarding:
      return "First-time setup";
  }
  return "";
}

// Header region: breadcrumb on the left, condensed system status dots on the
// right (scan server, printer, Inventatory Scan device, auto-label state).
ftxui::Element App::renderHeaderUi() const {
  const auto now = time(nullptr);
  const bool deviceOnline = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15;
  const bool scannerFlashing = now <= scannerFlashUntil_;
  const bool printerFlashing = now <= printerFlashUntil_;

  const auto scanColor =
      scannerFlashing ? uiTitleColor() : (server_.running() ? uiSuccessColor() : uiWarnColor());
  const auto printerColor = printerFlashing ? uiTitleColor()
                            : printerCheck_.ok
                                ? uiSuccessColor()
                                : (printerService_.hasConfiguredPrinter() ? uiWarnColor() : uiDangerColor());
  const auto deviceColor = deviceOnline ? uiSuccessColor() : uiMutedColor();
  const auto autoColor = autoPrintScannedLabels_ ? uiSuccessColor() : uiMutedColor();

  const std::string deviceValue = deviceOnline ? std::to_string(now - deviceLastSeen_) + "s" : "offline";

  auto self = const_cast<App*>(this);
  const auto nav = [&](Page page, const string& id, const string& label) {
    const bool activePage = page_ == page;
    auto item = styledText(" " + label + " ", activePage ? uiFocusColor() : uiSecondaryText(),
                           activePage ? uiSelectionBg() : uiSurfaceBg());
    if (activePage) item = item | ftxui::bold;
    return target(item, id, UiTargetKind::Navigation, [self, page] { self->changePage(page); });
  };

  auto navigation = ftxui::hbox({
      styledText(" Inventatory ", uiPrimaryText()) | ftxui::bold,
      nav(Page::Home, "nav.home", "1 Home"),
      nav(Page::Stock, "nav.stock", "2 Stock"),
      nav(Page::Racks, "nav.racks", "3 Racks"),
      nav(Page::Import, "nav.import", "4 Import"),
      nav(Page::Settings, "nav.settings", "5 Settings"),
  });

  auto dotSep = [] { return styledText("   ", uiDimColor()); };

  return ftxui::hbox({
             navigation,
             ftxui::filler(),
             statusDot("scan", scanColor),
             dotSep(),
             statusDot("printer", printerColor),
             dotSep(),
             statusDot("device", deviceColor, deviceValue),
             dotSep(),
             statusDot(autoPrintScannedLabels_ ? "auto-label on" : "auto-label off", autoColor),
             styledText("   ", uiDividerColor()),
             target(styledText(" Actions  Space ", uiInteractiveColor(), uiRaisedSurfaceBg()), "shell.actions",
                    UiTargetKind::Action, [self] { self->openActionSheet(); }),
             ftxui::text(" "),
         }) |
         ftxui::bgcolor(uiSurfaceBg());
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
    case Page::ScanSetup:
      return renderInventatoryScanSetupUi();
    case Page::Settings:
      return renderSettingsUi();
    case Page::Onboarding:
      return renderOnboardingUi();
  }

  return ftxui::text("");
}

ftxui::Element App::renderSearchBarUi() const {
  // Stock item editing renders inline in the Detail panel (see StockPage.cpp),
  // so this context line stays a plain search row instead of duplicating it.
  const bool stockEditing =
      page_ == Page::Stock && (inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue);
  const bool showsPrompt = !stockEditing &&
                            (inputMode_ == InputMode::EditValue || inputMode_ == InputMode::RackRename ||
                             inputMode_ == InputMode::RackType || inputMode_ == InputMode::RackCreate ||
                             inputMode_ == InputMode::RackJump || inputMode_ == InputMode::RackFilter);

  const auto activeBg = inputMode_ == InputMode::Search || showsPrompt ? uiRowSelectedBg() : uiPanelLeftBg();
  const auto bodyColor = inputMode_ == InputMode::Search ? uiTitleColor() : showsPrompt ? uiLinkColor() : uiMutedColor();
  string contextTitle = "Context";
  string contextText;
  if (inputMode_ == InputMode::Search) {
    contextTitle = "Search";
    contextText = "/" + inputBuffer_ + "_  (filtering live)";
  } else if (showsPrompt) {
    contextText = activePrompt() + inputBuffer_ + "_";
  } else {
    switch (page_) {
      case Page::Home:
        contextText = to_string(store_.items().size()) + " parts · operational overview";
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
      case Page::ScanSetup:
        contextTitle = "Setup wizard";
        contextText = "Guided Bluetooth provisioning for Inventatory Scan R1";
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
  if (!persistenceError_.empty()) {
    return fullLine(persistenceError_, uiDangerColor(), uiPanelLeftBg());
  }
  if (message_.empty()) {
    return ftxui::text("");
  }
  return fullLine(message_, uiAccentColor(), uiPanelLeftBg());
}

int App::run() {
  running_ = true;
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
  processScans();
  processDeviceRequests();
  processDeviceSyncEvents();
  clearMessageIfExpired();
  clearDeleteConfirmationIfExpired();
  processUpdateCheck();
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
  if (backgroundController_.enabled()) {
    backgroundController_.hideConsole(true);
  } else {
    running_ = false;
  }
}

void App::handleKey(const KeyEvent& key) {
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
    case InputMode::ActionSheet:
      handleActionSheetKey(key);
      return;
    case InputMode::None:
      break;
  }

  // Settings fields use their own staged editor rather than a legacy
  // InputMode. While it is active, all characters (including 1-5) and Enter
  // belong to the field and must not trigger global navigation or focus.
  if (page_ == Page::Settings && settingsEditingField_) {
    handleSettingsKey(key);
    return;
  }

  // The setup wizard owns every key while open: its Wi-Fi password and the
  // six-digit verification code must never be interpreted as global shortcuts.
  if (page_ == Page::ScanSetup) {
    handleInventatoryScanSetupKey(key);
    return;
  }

  if (page_ == Page::Onboarding) {
    handleOnboardingKey(key);
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
      case '5': changePage(Page::Settings); return;
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
    case Page::ScanSetup:
      handleInventatoryScanSetupKey(key);
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
    else if (page_ == Page::Import) moveImportSelection(delta);
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

}  // namespace inventatory


