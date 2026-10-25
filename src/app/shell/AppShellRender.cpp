// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - application shell rendering.

#include "App.h"

#include "app/shell/AppNavigation.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>

namespace inventatory {

using namespace std;

namespace {

string currentDateTimeText() {
  const auto now = time(nullptr);
  tm localTime{};
#ifdef _WIN32
  localtime_s(&localTime, &now);
#else
  localtime_r(&now, &localTime);
#endif
  char buffer[32]{};
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
  return buffer;
}

ftxui::Color messageColor(UiMessageColorRole role) {
  switch (role) {
    case UiMessageColorRole::Success:
      return uiSuccessColor();
    case UiMessageColorRole::Warning:
      return uiWarnColor();
    case UiMessageColorRole::Error:
      return uiDangerColor();
    case UiMessageColorRole::Info:
      return uiInfoColor();
  }
  return uiInfoColor();
}

ftxui::Element fixedMessageRow(ftxui::Element element) {
  return element | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, uiMessageRowHeight());
}

ftxui::Element semanticMessageRow(const string& text, UiMessageSeverity severity, bool flashing) {
  const auto presentation = uiMessagePresentation(severity);
  const auto semanticColor = messageColor(presentation.color);
  const auto foreground = flashing ? uiCanvasBg() : semanticColor;
  const auto background = flashing ? semanticColor : uiPanelLeftBg();
  return fixedMessageRow(fullLine(" " + string(presentation.prefix) + text, foreground, background));
}

}  // namespace

// Frame: header, content, context line or action sheet, message. Space opens the action sheet.
ftxui::Element App::renderUi() const {
  uiTargets_.clear();
  if (inventoryRecoveryRequired_) {
    return ftxui::vbox({
        ftxui::filler(),
        styledText("Inventory recovery required", uiDangerColor()),
        ftxui::separator(),
        styledText(inventoryRecoveryDetail_, uiTitleColor()),
        styledText("The active workspace was preserved and Inventatory is locked to prevent data loss.", uiMutedColor()),
        styledText("Press D to choose another Inventatory folder, or Esc to exit.", uiAccentColor()),
        ftxui::filler(),
    }) | ftxui::border | ftxui::bgcolor(uiCanvasBg());
  }
  // Setup is a dedicated surface and must not inherit workspace navigation or chrome.
  if (page_ == Page::Update) {
    return renderUpdateUi() | ftxui::flex | ftxui::bgcolor(uiCanvasBg());
  }
  if (page_ == Page::Onboarding || (page_ == Page::ScanSetup && returnToOnboardingAfterScan_)) {
    return renderWizardUi() | ftxui::flex | ftxui::bgcolor(uiCanvasBg());
  }
  if (workspaceBlockedByTerminalSize()) {
    const auto* active = ftxui::ScreenInteractive::Active();
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
  // The sheet replaces the context line while open.
  if (inputMode_ == InputMode::ActionSheet) body.push_back(renderActionSheetUi());
  else body.push_back(renderSearchBarUi());
  body.push_back(renderMessageUi());
  // xflex keeps the background full width; without it FTXUI sizes the root to its content.
  return ftxui::vbox(move(body)) | ftxui::xflex | ftxui::bgcolor(uiCanvasBg());
}

// True while the resize notice replaces the workspace. Recovery and the wizards are exempt.
bool App::workspaceBlockedByTerminalSize() const {
  if (inventoryRecoveryRequired_ || page_ == Page::Update || page_ == Page::Onboarding ||
      (page_ == Page::ScanSetup && returnToOnboardingAfterScan_)) {
    return false;
  }
  const auto* active = ftxui::ScreenInteractive::Active();
  return active != nullptr && app_navigation::terminalTooSmall(active->dimx(), active->dimy());
}

std::string App::pageName() const {
  switch (page_) {
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
    case Page::VendorSetup:
      return "DigiKey Setup";
    case Page::Settings:
      return "Settings";
    case Page::Onboarding:
      return "First-time setup";
    case Page::Update:
      return "Update";
  }
  return "";
}

// The Actions control and Space both open the action sheet.
ftxui::Element App::renderHeaderUi() const {
  auto self = const_cast<App*>(this);
  const auto nav = [&](Page page, const string& id, const string& label) {
    const bool activePage = page_ == page;
    auto item = activePage
                    ? uiHeaderText(" " + label + " ", uiFocusColor(), uiSelectionBg())
                    : uiBodyText(" " + label + " ", uiSecondaryText(), uiSurfaceBg());
    return target(item, id, UiTargetKind::Navigation, [self, page] { self->changePage(page); });
  };

  ftxui::Elements navigation;
  navigation.push_back(uiHeaderText(" Inventatory ", uiPrimaryText()));
  for (const auto& entry : app_navigation::primaryNavigationEntries()) {
    const auto page = pageFor(entry.page);
    navigation.push_back(nav(page, "nav." + string(entry.id),
                             string(1, entry.shortcut) + " " + entry.label));
  }

  // Clickable route to the action sheet; Space is the keyboard route.
  const bool actionsAvailable = page_ != Page::ScanSetup && page_ != Page::VendorSetup;
  const string actionsLabel = " Actions ";
  auto actions = uiBodyText(actionsLabel, inputMode_ == InputMode::ActionSheet ? uiFocusColor() : uiInteractiveColor(),
                            inputMode_ == InputMode::ActionSheet ? uiSelectionBg() : uiSurfaceBg());

  const auto* active = ftxui::ScreenInteractive::Active();
  const int screenWidth = active != nullptr ? active->dimx() : 120;
  const auto stringWidth = [](const string& text) { return static_cast<int>(ftxui::string_width(text)); };
  // Columns used by the brand, the destinations and the Actions control.
  int fixedColumns = stringWidth(" Inventatory ") + stringWidth(actionsLabel);
  for (const auto& entry : app_navigation::primaryNavigationEntries()) {
    fixedColumns += stringWidth(" " + string(1, entry.shortcut) + " " + entry.label + " ");
  }

  ftxui::Elements header;
  header.push_back(ftxui::hbox(move(navigation)));
  header.push_back(ftxui::filler());
  header.push_back(target(actions, "header.actions", UiTargetKind::Action, [self] { self->activateActionsControl(); },
                          actionsAvailable));
  switch (app_navigation::headerClockFor(screenWidth, fixedColumns)) {
    case app_navigation::HeaderClock::Full:
      header.push_back(uiBodyText(" " + currentDateTimeText() + " ", uiSecondaryText()));
      break;
    case app_navigation::HeaderClock::Compact:
      header.push_back(uiBodyText(" " + currentDateTimeText().substr(11, 5) + " ", uiSecondaryText()));
      break;
    case app_navigation::HeaderClock::Hidden:
      break;
  }

  return ftxui::hbox(move(header)) | ftxui::xflex | ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element App::renderPageUi() const {
  switch (page_) {
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
    case Page::VendorSetup:
      return renderVendorSetupUi();
    case Page::Settings:
      return renderSettingsUi();
    case Page::Onboarding:
      return renderOnboardingUi();
    case Page::Update:
      return renderUpdateUi();
  }

  return ftxui::text("");
}

ftxui::Element App::renderSearchBarUi() const {
  // Stock editing renders inline in the Detail panel, so this line stays a plain search row.
  const bool stockEditing =
      page_ == Page::Stock && (inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue ||
                               inputMode_ == InputMode::VendorPick);
  const bool showsPrompt = !stockEditing &&
                            (inputMode_ == InputMode::EditValue || inputMode_ == InputMode::RackRename ||
                             inputMode_ == InputMode::RackType || inputMode_ == InputMode::RackCreate ||
                             inputMode_ == InputMode::RackJump || inputMode_ == InputMode::RackFilter ||
                             inputMode_ == InputMode::QuantityAdjust || inputMode_ == InputMode::StocktakeCount ||
                             inputMode_ == InputMode::BomReceive || inputMode_ == InputMode::BomPartSearch ||
                             inputMode_ == InputMode::HistorySearch ||
                             inputMode_ == InputMode::HistoryCheckpoint ||
                             inputMode_ == InputMode::ExitConfirmation);

  const auto activeBg = inputMode_ == InputMode::Search || inputMode_ == InputMode::ClosestSearch ||
                                inputMode_ == InputMode::HistorySearch || showsPrompt
                            ? uiRowSelectedBg()
                            : uiPanelLeftBg();
  const auto bodyColor = inputMode_ == InputMode::Search || inputMode_ == InputMode::ClosestSearch ||
                                 inputMode_ == InputMode::HistorySearch
                             ? uiTitleColor()
                             : showsPrompt ? uiLinkColor() : uiMutedColor();
  string contextTitle = "Context";
  string contextText;
  if (inputMode_ == InputMode::Search) {
    contextTitle = "Search";
    contextText = "/" + inputBuffer_ + "_  (filtering live)";
  } else if (inputMode_ == InputMode::ClosestSearch) {
    contextTitle = "Find closest to";
    contextText = ">" + inputBuffer_ + "_  (searching all records live)";
  } else if (inputMode_ == InputMode::HistorySearch) {
    contextTitle = "History search";
    contextText = "/" + inputBuffer_ + "_  (filtering live)";
  } else if (page_ == Page::Stock && closestSearchActive_) {
    contextTitle = "Find closest to";
    contextText = closestSearchQuery_.empty() ? "> type a physical value" : ">" + closestSearchQuery_ +
                  "   F edit target   Esc close";
  } else if (inputMode_ == InputMode::ExitConfirmation) {
    contextTitle = "Unsaved settings";
    contextText = activePrompt();
  } else if (showsPrompt) {
    contextText = activePrompt() + inputBuffer_ + "_";
  } else {
    switch (page_) {
      case Page::Stock:
        contextTitle = "Search";
        contextText = searchQuery_.empty() ? "/ type to filter" : "/" + searchQuery_;
        if (stockDateFilter_ != StockDateFilter::All) {
          contextText += ", " + stockDateFilterName(stockDateFilter_);
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
                            ? "Import a BOM to begin"
                            : to_string(bomProjects_.size()) +
                                  (bomProjects_.size() == 1 ? " project" : " projects");
        } else if (bomView_ == BomView::Build) {
          const auto stops = bomPickPlan().stops.size();
          contextTitle = "Picking";
          contextText = bomDeductPrompt_ ? "finished"
                                         : "stop " + to_string(min(bomBuildStep_ + 1, stops)) + " of " + to_string(stops);
        } else {
          contextText = to_string(bomAnalysis_.lines.size()) + " lines  " +
                        to_string(bomAnalysis_.boards) +
                        (bomAnalysis_.boards == 1 ? " board" : " boards");
        }
        break;
      case Page::History:
        contextTitle = "History";
        if (inventoryCommits_.empty()) {
          contextText = "No inventory commits";
        } else {
          const auto visible = history_page_detail::filteredHistoryIndices(
              inventoryCommits_, historySearchQuery_, historySourceFilter_);
          const auto selected = find(visible.begin(), visible.end(), historySelection_);
          const auto position = selected == visible.end() ? size_t(0) : static_cast<size_t>(selected - visible.begin());
          contextText = visible.empty() ? "No matching commits"
                                        : "commit " + to_string(position + 1) + " / " + to_string(visible.size());
        }
        break;
      case Page::ScanSetup:
        contextTitle = "Setup wizard";
        contextText = "Guided Bluetooth provisioning for Inventatory Scanner R1";
        break;
      case Page::VendorSetup:
        contextTitle = "Setup wizard";
        contextText = "DigiKey credentials";
        break;
      case Page::Settings:
        contextTitle = "Settings";
        contextText = settingsCategoryName(settingsCategory_) +
                      (settingsDirty_ ? ", unsaved changes" : ", saved");
        break;
      case Page::Update:
        contextTitle = "Update wizard";
        contextText = "Inventatory release update";
        break;
      case Page::Onboarding:
        break;
    }
  }

  ftxui::Elements rows;
  auto context = footerField(contextTitle, contextText, uiSecondaryText(), bodyColor, activeBg);
  if (inputMode_ == InputMode::ExitConfirmation) {
    // Mouse route to the prompt, as well as S, D and Esc.
    auto self = const_cast<App*>(this);
    const auto answer = [self](KeyType type, char ch) {
      return [self, type, ch] {
        KeyEvent event;
        event.type = type;
        event.ch = ch;
        self->handleExitConfirmationKey(event);
      };
    };
    context = ftxui::hbox({
                  uiHeaderText(" " + contextTitle + ": ", uiSecondaryText(), activeBg),
                  target(uiSecondaryButton(exitSavePending_ ? "S Retry save" : "S Save"), "exit.save",
                         UiTargetKind::Button, answer(KeyType::Character, 's')),
                  uiBodyText(" ", bodyColor, activeBg),
                  target(uiSecondaryButton(exitSavePending_ ? "D Exit anyway" : "D Discard", uiDangerColor()),
                         "exit.discard", UiTargetKind::Button, answer(KeyType::Character, 'd')),
                  uiBodyText(" ", bodyColor, activeBg),
                  target(uiSecondaryButton("Esc Stay"), "exit.stay", UiTargetKind::Button,
                         answer(KeyType::Escape, '\0')),
                  ftxui::filler(),
              }) |
              ftxui::bgcolor(activeBg);
  }
  if (page_ == Page::Stock && inputMode_ == InputMode::None) {
    auto self = const_cast<App*>(this);
    context = target(context, closestSearchActive_ ? "stock.closest.search" : "stock.search", UiTargetKind::Field,
                     [self] { self->closestSearchActive_ ? self->startClosestSearch() : self->startSearch(); });
  }
  if (page_ == Page::History &&
      (inputMode_ == InputMode::None || inputMode_ == InputMode::HistoryConfirm)) {
    const auto hints = inputMode_ == InputMode::HistoryConfirm
                           ? "Enter Confirm  Esc Cancel"
                           : "Up/Down select   Enter view   v revert   s restore   / search   f filter   Esc back";
    rows.push_back(ftxui::hbox({uiHeaderText(" History: ", uiSecondaryText(), uiPanelLeftBg()),
                                uiBodyText(contextText, uiInfoColor(), uiPanelLeftBg()), ftxui::filler(),
                                styledText(hints, uiMutedColor(), uiPanelLeftBg())}) |
                   ftxui::bgcolor(uiPanelLeftBg()));
  } else if (page_ == Page::Projects && inputMode_ == InputMode::None) {
    string hints;
    if (bomView_ == BomView::Build && bomDeductPrompt_) hints = "Up/Down part   +/- amount taken   y deduct   n keep stock   Bksp back";
    else if (bomView_ == BomView::Build) hints = "Enter rack done   Bksp back   Esc stop picking";
    else if (bomView_ == BomView::Overview) hints = "Enter open   f find in racks   t all lines   Esc projects";
    else if (bomView_ == BomView::Lines) hints = "Enter open its step   f find in racks   Esc back";
    else if (bomView_ == BomView::StartNotice) hints = "Enter start anyway   s get them first   Esc back";
    else if (bomView_ == BomView::Confirm) hints = "Enter use   n none of these   s skip   Esc back";
    else if (bomView_ == BomView::Choose) hints = "Enter use   l store page   / search   v store   s skip   Esc back";
    else if (bomView_ == BomView::Order && bomOrderConfirm_) hints = "y mark ordered   n cancel";
    else if (bomView_ == BomView::Order) hints = "c cart file   l next page   m mark ordered   Esc back";
    else if (bomView_ == BomView::Receive) hints = "Enter receive   a receive all   Esc back";
    else hints = "Enter open   i import BOM   d forget";
    rows.push_back(ftxui::hbox({std::move(context) | ftxui::flex, styledText(hints + " ", uiMutedColor(), activeBg)}) |
                   ftxui::bgcolor(activeBg));
  } else {
    rows.push_back(context);
  }

  // Field list for the CSV import review only; Stock shows fields inline in its Detail panel.
  if (inputMode_ == InputMode::EditFieldMenu && !stockEditing) {
    ftxui::Elements options;
    options.push_back(footerField("Edit fields", "Up/Down field   Enter edit   s save   Esc cancel",
                                  uiAccentColor(), uiMutedColor(), uiPanelLeftBg()));
    for (size_t index = 0; index < menuOptions_.size(); ++index) {
      const auto bg = static_cast<int>(index) == fieldMenuIndex_ ? uiRowSelectedBg() : uiSurfaceBg();
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
  // Scanner firmware, in order: the GitHub download, the chunks the scanner takes, then its install.
  const auto firmwareProgress = [this]() -> optional<ftxui::Element> {
    using namespace chrono;
    constexpr auto kStalledAfter = seconds(30);
    const auto download = scanFirmwareDownloadProgress_;
    if (download && scanFirmwareDownloadFuture_.valid() &&
        scanFirmwareDownloadFuture_.wait_for(seconds(0)) != future_status::ready) {
      const auto done = download->downloaded.load();
      const auto total = download->total.load();
      const auto detail = formatBytes(done) + (total > 0 ? " of " + formatBytes(total) : string());
      return fixedMessageRow(uiTransferProgressRow(
          {"Downloading Scan R1 firmware " + scanFirmwareDownloadVersion_ + " from GitHub:", total > 0, done, total,
           detail}));
    }
    const auto transfer = scanFirmware_.transfer();
    if (transfer.version.empty() || transfer.sentBytes == 0) return nullopt;
    if (transfer.sentBytes < transfer.imageBytes) {
      auto detail = formatBytes(transfer.sentBytes) + " of " + formatBytes(transfer.imageBytes);
      if (transfer.lastChunkAt && steady_clock::now() - *transfer.lastChunkAt >= kStalledAfter) {
        const auto idle = duration_cast<seconds>(steady_clock::now() - *transfer.lastChunkAt).count();
        detail += ", no request for " + to_string(idle) + " s";
      }
      return fixedMessageRow(uiTransferProgressRow(
          {"Sending firmware " + transfer.version + " to the scanner:", true, transfer.sentBytes,
           transfer.imageBytes, detail}));
    }
    if (transfer.status.state == "installing") {
      return fixedMessageRow(
          uiTransferProgressRow({"Scanner is installing firmware " + transfer.version, false, 0, 0, ""}));
    }
    return nullopt;
  };
  if (auto row = firmwareProgress()) return move(*row);

  const auto refreshProgress = [](const auto& run, const char* supplier) -> optional<ftxui::Element> {
    if (run.total == 0 || !run.running()) return nullopt;
    return fixedMessageRow(ftxui::hbox({
                               styledText(string(" Refreshing ") + supplier + " data: ", uiLinkColor()),
                               styledText(uiProgressBar(min(run.completed, run.total), run.total) + " ", uiPrimaryText()),
                               uiHeaderText(to_string(min(run.completed, run.total)) + " of " + to_string(run.total),
                                            uiPrimaryText()),
                               styledText(" parts done", uiMutedColor()),
                               ftxui::filler(),
                           }) |
                           ftxui::bgcolor(uiPanelLeftBg()));
  };
  if (auto row = refreshProgress(digiKeyRefresh_, "DigiKey")) return move(*row);
  if (auto row = refreshProgress(tmeRefresh_, "TME")) return move(*row);
  if (auto row = refreshProgress(mouserRefresh_, "Mouser")) return move(*row);
  if (importSyncRunning_) {
    const auto completed = min(importSyncCompleted_, importSyncTotal_);
    return fixedMessageRow(ftxui::hbox({
                               styledText(" Syncing the import with DigiKey: ", uiLinkColor()),
                               styledText(uiProgressBar(completed, importSyncTotal_) + " ", uiPrimaryText()),
                               uiHeaderText(to_string(completed) + " of " + to_string(importSyncTotal_), uiPrimaryText()),
                               styledText(" rows done", uiMutedColor()),
                               ftxui::filler(),
                           }) |
                           ftxui::bgcolor(uiPanelLeftBg()));
  }
  if (!persistenceError_.empty()) {
    return semanticMessageRow(persistenceError_, UiMessageSeverity::Error, false);
  }
  if (message_.empty()) {
    // Keep the row even when quiet: zero height would make the layout jump on each message.
    return fixedMessageRow(fullLine("", uiMutedColor(), uiPanelLeftBg()));
  }

  const bool flashing = uiMessageAcknowledgementPulseActive(messageFlashStartedAt_, uiAnimationTicks());
  return semanticMessageRow(message_, messageSeverity_, flashing);
}

}  // namespace inventatory
