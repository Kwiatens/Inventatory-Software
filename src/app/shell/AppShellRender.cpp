// Inventatory - application shell rendering.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <utility>

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
        styledText("The active workspace was preserved and Inventatory is locked to prevent data loss.", uiMutedColor()),
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

  const auto activeBg = inputMode_ == InputMode::Search || inputMode_ == InputMode::ClosestSearch || showsPrompt
                            ? uiRowSelectedBg()
                            : uiPanelLeftBg();
  const auto bodyColor = inputMode_ == InputMode::Search || inputMode_ == InputMode::ClosestSearch
                             ? uiTitleColor()
                             : showsPrompt ? uiLinkColor() : uiMutedColor();
  string contextTitle = "Context";
  string contextText;
  if (inputMode_ == InputMode::Search) {
    contextTitle = "Search";
    contextText = "/" + inputBuffer_ + "_  (filtering live)";
  } else if (inputMode_ == InputMode::ClosestSearch) {
    contextTitle = "Find Closest To";
    contextText = ">" + inputBuffer_ + "_  (searching all records live)";
  } else if (page_ == Page::Stock && closestSearchActive_) {
    contextTitle = "Find Closest To";
    contextText = closestSearchQuery_.empty() ? "> type a physical value" : ">" + closestSearchQuery_ +
                  "  · F edit target · Esc close";
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
                        (bomAnalysis_.boards == 1 ? " board" : " boards");
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
    context = target(context, closestSearchActive_ ? "stock.closest.search" : "stock.search", UiTargetKind::Field,
                     [self] { self->closestSearchActive_ ? self->startClosestSearch() : self->startSearch(); });
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
  if (page_ == Page::Projects && bomView_ == BomView::Build && bomAnalysisValid_ &&
      !bomBuildReady(bomAnalysis_) && message_.empty()) {
    return fullLine(to_string(bomAnalysis_.shortCount) +
                        " shortages · stock will not be deducted",
                    uiWarnColor(), uiPanelLeftBg());
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

}  // namespace inventatory
