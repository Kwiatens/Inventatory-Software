// Inventatory - Hardware Inventory Management System
// Dashboard rendering and presentation helpers.

#include "App.h"

#include "ui/pages/dashboard/DashboardPagePrivate.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;
using namespace dashboard_detail;

namespace {

constexpr long long kScannerTabRollMs = 700;
constexpr long long kScannerMessageCycleMs = 3200;
constexpr int kScannerExpandedHeight = 8;
constexpr int kScannerSlimHeight = 3;
ftxui::Element plainSectionTitle(const string& title, int width) {
  const int contentWidth = max(20, width - 2);
  return ftxui::hbox({uiHeaderText(title, uiPrimaryText()), ftxui::filler()}) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth) |
         ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element metricBlock(const string& label, const string& value, ftxui::Color valueColor, int width) {
  return ftxui::vbox({
             uiBodyText(label, uiMutedColor()),
             uiBodyText(value, valueColor),
         }) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width) |
         ftxui::bgcolor(uiRaisedSurfaceBg());
}

ftxui::Element statusLine(const string& label, const string& value, ftxui::Color valueColor, int width) {
  return ftxui::hbox({
             uiBodyText(" " + label, uiSecondaryText()),
             ftxui::filler(),
             uiBodyText(ellipsize(value, static_cast<size_t>(max(8, width / 2))), valueColor),
             uiBodyText(" ", uiSecondaryText()),
         }) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}
string elapsedText(time_t timestamp) {
  if (timestamp <= 0) return "never";
  const auto elapsed = max<long long>(0, static_cast<long long>(time(nullptr) - timestamp));
  if (elapsed < 2) return "now";
  if (elapsed < 60) return to_string(elapsed) + "s ago";
  if (elapsed < 3600) return to_string(elapsed / 60) + "m ago";
  return to_string(elapsed / 3600) + "h ago";
}

ftxui::Element scannerActivityBar(bool connected) {
  constexpr int kSegments = 5;
  const auto ticks = uiAnimationTicks();
  const int phase = static_cast<int>((ticks / 180) % (kSegments * 2 - 2));
  const int active = phase < kSegments ? phase : (kSegments * 2 - 2 - phase);
  ftxui::Elements segments;
  for (int index = 0; index < kSegments; ++index) {
    const bool lit = connected && index == active;
    segments.push_back(uiBodyText(lit ? "▌" : "│", lit ? uiInteractiveColor() : uiDividerColor()));
  }
  return ftxui::vbox(move(segments)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 2);
}

int scannerPanelHeight(bool connected, long long transitionStartedAt, bool expanding) {
  const int target = connected ? kScannerExpandedHeight : kScannerSlimHeight;
  if (transitionStartedAt < 0) return target;
  const auto elapsed = max(0LL, uiAnimationTicks() - transitionStartedAt);
  const auto progress = min(1.0, static_cast<double>(elapsed) / static_cast<double>(kScannerTabRollMs));
  const int start = expanding ? kScannerSlimHeight : kScannerExpandedHeight;
  return max(kScannerSlimHeight,
             min(kScannerExpandedHeight,
                 static_cast<int>(start + (target - start) * progress + (target >= start ? 0.5 : -0.5))));
}

ftxui::Element scannerPanel(const string& state, bool connected, bool /*paired*/, long long transitionStartedAt,
                            bool transitionExpanding, const string& firmware, int rssi, const string& mode,
                            int pendingEvents, time_t lastSeen, time_t lastSync, const string& lastResult,
                            int width) {
  const int contentWidth = max(28, width - 2);
  const int panelHeight = scannerPanelHeight(connected, transitionStartedAt, transitionExpanding);
  ftxui::Elements rows;
  rows.push_back(plainSectionTitle("Inventatory Scanner", width));

  ftxui::Elements body;
  if (connected) {
    body.push_back(uiHeaderText("ONLINE · monitoring inventory", uiSuccessColor()));
    body.push_back(uiBodyText("FW " + (firmware.empty() ? string("unknown") : firmware) + " · RSSI " +
                                  to_string(rssi) + " dBm",
                              uiSecondaryText()));
    body.push_back(uiBodyText("Mode " + (mode.empty() ? string("idle") : mode) + " · queue " +
                                  to_string(max(0, pendingEvents)),
                              uiSecondaryText()));
    body.push_back(uiBodyText("Sync " + elapsedText(lastSync) + " · seen " + elapsedText(lastSeen), uiMutedColor()));
    if (!lastResult.empty()) {
      body.push_back(uiBodyText("Last " + ellipsize(lastResult, static_cast<size_t>(max(12, contentWidth - 2))),
                                lastResult.rfind("ERROR", 0) == 0 ? uiDangerColor() : uiAccentColor()));
    }
  } else {
    string message;
    if (state == "UNPAIRED") {
      message = "Pair Scanner R1 in Settings";
    } else if (state == "WAITING") {
      message = "Please connect the Inventatory Scanner";
    } else {
      const bool firstMessage = (uiAnimationTicks() / kScannerMessageCycleMs) % 2 == 0;
      message = firstMessage ? "Inventatory Scanner Disconnected" : "Please reconnect the device.";
    }
    body.push_back(uiBodyText(ellipsize(message, static_cast<size_t>(max(8, contentWidth))), uiMutedColor()));
  }

  auto bodyElement = ftxui::vbox(move(body)) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, connected ? contentWidth - 3 : contentWidth);
  ftxui::Elements bodyRow;
  if (connected) {
    bodyRow = {scannerActivityBar(true), ftxui::text(" "), move(bodyElement), ftxui::filler()};
  } else {
    bodyRow = {move(bodyElement), ftxui::filler()};
  }
  rows.push_back(ftxui::hbox(move(bodyRow)) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth));
  return ftxui::vbox(move(rows)) |
         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, panelHeight) |
         ftxui::yframe |
         ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element healthPanel(const DashboardSnapshot& snapshot, const string& persistenceError, int width) {
  const int contentWidth = max(28, width - 2);
  const int metricWidth = max(12, (contentWidth - 1) / 2);
  const bool databaseReady = persistenceError.empty();
  ftxui::Elements rows;
  rows.push_back(plainSectionTitle("INVENTORY DATABASE STATUS", width));
  rows.push_back(ftxui::hbox({
      metricBlock("Parts tracked", to_string(snapshot.itemCount), uiPrimaryText(), metricWidth),
      uiDivider(),
      metricBlock("Units tracked", to_string(snapshot.totalQuantity), uiPrimaryText(), metricWidth),
  }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth));
  rows.push_back(uiDivider());
  rows.push_back(ftxui::hbox({
      metricBlock("Low stock", to_string(snapshot.lowStockCount),
                  snapshot.lowStockCount > 0 ? uiWarnColor() : uiSuccessColor(), metricWidth),
      uiDivider(),
      metricBlock("Out of stock", to_string(snapshot.outOfStockCount),
                  snapshot.outOfStockCount > 0 ? uiDangerColor() : uiSuccessColor(), metricWidth),
  }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth));
  rows.push_back(uiDivider());
  rows.push_back(uiHeaderText("DATA QUALITY", uiMutedColor()));
  rows.push_back(statusLine("Missing metadata", to_string(snapshot.missingMetadataCount),
                            snapshot.missingMetadataCount > 0 ? uiWarnColor() : uiSuccessColor(), contentWidth));
  rows.push_back(statusLine("Invalid records", to_string(snapshot.dataErrorCount),
                            snapshot.dataErrorCount > 0 ? uiDangerColor() : uiSuccessColor(), contentWidth));
  if (!databaseReady) {
    rows.push_back(uiBodyText(ellipsize(persistenceError, static_cast<size_t>(contentWidth)), uiDangerColor()));
  }
  return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element activityPanel(const vector<InventoryCommit>& commits, int width, size_t selectedRow, bool active,
                             const function<ftxui::Element(ftxui::Element, size_t)>& wrapRow) {
  const int contentWidth = max(28, width - 2);
  ftxui::Elements rows;
  rows.push_back(plainSectionTitle("RECENT COMMITS", width));
  if (commits.empty()) {
    rows.push_back(uiBodyText("No inventory commits yet.", uiMutedColor()));
    return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
  }
  for (size_t index = 0; index < commits.size(); ++index) {
    const auto& commit = commits[index];
    const auto timestamp = nowTimestampString(commit.timestamp);
    const int timestampWidth = min(16, max(8, contentWidth / 3));
    const int messageWidth = max(8, contentWidth - timestampWidth - 3);
    const bool selected = index == selectedRow;
    auto renderedRow = ftxui::hbox({
                         fixedCell(timestamp, timestampWidth, uiSecondaryText()),
                         centeredCell("#" + to_string(commit.sequence), 5, uiInfoColor()),
                         uiBodyText(ellipsize(commit.message,
                                              static_cast<size_t>(messageWidth)),
                                    uiSecondaryText()),
                         ftxui::filler(),
                     }) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth) |
                     ftxui::bgcolor(selected && active ? uiSelectionBg()
                                                        : (index % 2 == 0 ? uiSurfaceBg() : uiCanvasBg()));
    if (selected) {
      renderedRow = renderedRow | ftxui::select;
    }
    rows.push_back(wrapRow(move(renderedRow), index));
  }
  return ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator |
         ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
}

}  // namespace

void App::moveDashboardSelection(DashboardList list, int delta) {
  const auto warningCount = [&] {
    return buildDashboardSnapshot(store_.items(), settings_.lowStockThreshold).attention.size();
  };
  const size_t count = list == DashboardList::Warnings ? warningCount() : inventoryCommits_.size();
  auto& selection = list == DashboardList::Warnings ? dashboardWarningSelection_ : dashboardActivitySelection_;
  dashboardList_ = list;
  if (count == 0) {
    selection = 0;
    dirty_ = true;
    return;
  }

  const auto current = static_cast<int>(min(selection, count - 1));
  selection = static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(count - 1)));
  dirty_ = true;
}

void App::jumpDashboardSelection(DashboardList list, bool toEnd) {
  const auto count = list == DashboardList::Warnings
                         ? buildDashboardSnapshot(store_.items(), settings_.lowStockThreshold).attention.size()
                         : inventoryCommits_.size();
  selectDashboardRow(list, toEnd && count > 0 ? count - 1 : 0);
}

void App::selectDashboardRow(DashboardList list, size_t position) {
  const auto count = list == DashboardList::Warnings
                         ? buildDashboardSnapshot(store_.items(), settings_.lowStockThreshold).attention.size()
                         : inventoryCommits_.size();
  auto& selection = list == DashboardList::Warnings ? dashboardWarningSelection_ : dashboardActivitySelection_;
  dashboardList_ = list;
  selection = count == 0 ? 0 : min(position, count - 1);
  dirty_ = true;
}



ftxui::Element App::renderDashboardUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const auto snapshot = buildDashboardSnapshot(store_.items(), settings_.lowStockThreshold);

  auto scannerState = scannerDashboardState_;
  if (scannerState == ScannerDashboardState::Unknown) {
    if (trim(inventatoryScanConfig_.token).empty()) {
      scannerState = ScannerDashboardState::Unpaired;
    } else if (trim(inventatoryScanConfig_.deviceId).empty()) {
      scannerState = ScannerDashboardState::Waiting;
    } else {
      const auto now = time(nullptr);
      scannerState = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15
                         ? ScannerDashboardState::Online
                         : ScannerDashboardState::Offline;
    }
  }

  const bool scannerConnected = scannerState == ScannerDashboardState::Online;
  const bool scannerPaired = scannerState != ScannerDashboardState::Unpaired;
  const string scannerStateText = scannerState == ScannerDashboardState::Unpaired ? "UNPAIRED"
                                  : scannerState == ScannerDashboardState::Waiting ? "WAITING"
                                                                                  : scannerState == ScannerDashboardState::Offline ? "OFFLINE"
                                                                                                                                   : "ONLINE";
  const int minimumRightWidth = 50;
  const int leftWidth = max(48, min(screenWidth - minimumRightWidth - 1, (screenWidth * 45) / 100));
  const int rightWidth = max(1, screenWidth - leftWidth - 1);

  auto self = const_cast<App*>(this);
  const auto warningSelection = snapshot.attention.empty()
                                    ? size_t(0)
                                    : min(dashboardWarningSelection_, snapshot.attention.size() - 1);
  const auto activitySelection = inventoryCommits_.empty()
                                     ? size_t(0)
                                     : min(dashboardActivitySelection_, inventoryCommits_.size() - 1);
  const bool warningsActive = dashboardList_ == DashboardList::Warnings;
  const bool activityActive = dashboardList_ == DashboardList::Commits;
  const auto wrapWarningRow = [self](ftxui::Element row, size_t index) {
    return self->target(move(row), "dashboard.warning." + to_string(index), UiTargetKind::Row,
                        [self, index] { self->selectDashboardRow(DashboardList::Warnings, index); }, true, false);
  };
  const auto wrapActivityRow = [self](ftxui::Element row, size_t index) {
    return self->target(move(row), "dashboard.activity." + to_string(index), UiTargetKind::Row,
                        [self, index] {
                          self->selectDashboardRow(DashboardList::Commits, index);
                          if (index < self->inventoryCommits_.size()) {
                            self->historySelection_ = index;
                            self->openSelectedHistoryCommit();
                          }
                        }, true, false);
  };

  auto warningPanel = attentionPanel(snapshot, leftWidth, warningSelection, warningsActive, wrapWarningRow) |
                      ftxui::reflect(dashboardWarningPanelBounds_);
  auto recentPanel = activityPanel(inventoryCommits_, rightWidth, activitySelection, activityActive, wrapActivityRow) |
                     ftxui::reflect(dashboardActivityPanelBounds_);

  auto warningSide = ftxui::vbox({
                         move(warningPanel) | ftxui::flex,
                         uiDivider(),
                         scannerPanel(scannerStateText, scannerConnected, scannerPaired,
                                      scannerDashboardTransitionStartedAt_, scannerDashboardTransitionExpanding_,
                                      deviceFirmwareVersion_, deviceRssi_, deviceMode_, devicePendingEventCount_,
                                      deviceLastSeen_, deviceLastSync_, deviceLastResult_, leftWidth),
                     }) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, leftWidth) |
                     ftxui::flex;

  auto healthSide = ftxui::vbox({
                       healthPanel(snapshot, persistenceError_, rightWidth),
                       uiDivider(),
                       move(recentPanel) | ftxui::flex,
                   }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, rightWidth) |
                   ftxui::flex;

  return ftxui::hbox({warningSide, uiDivider(), healthSide}) |
         ftxui::bgcolor(uiCanvasBg()) |
         ftxui::flex;
}

}  // namespace inventatory
