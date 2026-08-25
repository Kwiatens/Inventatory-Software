// Inventatory - Hardware Inventory Management System
// Alert-led dashboard rendering and keyboard handling.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <unordered_set>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

enum class AttentionSeverity {
  Low = 2,
  Out = 3,
};

enum class AttentionGroup {
  Out,
  Low,
};

struct AttentionRow {
  string issue;
  string partName;
  string location;
  string reason;
  int quantity = 0;
  AttentionSeverity severity = AttentionSeverity::Low;
  AttentionGroup group = AttentionGroup::Low;
};

struct DashboardSnapshot {
  size_t itemCount = 0;
  size_t totalQuantity = 0;
  size_t lowStockCount = 0;
  size_t outOfStockCount = 0;
  size_t dataErrorCount = 0;
  size_t missingMetadataCount = 0;
  vector<AttentionRow> attention;
  vector<ActivityEntry> recentEvents;
};

constexpr long long kWarningRowStepMs = 120;
constexpr long long kWarningHoldMs = 1600;
constexpr long long kWarningRestMs = 800;
constexpr long long kScannerTabRollMs = 700;
constexpr long long kScannerMessageCycleMs = 3200;
constexpr int kScannerExpandedHeight = 8;
constexpr int kScannerSlimHeight = 3;

ftxui::Element fixedCell(const string& value, int width, ftxui::Color color, bool rightAlign = false,
                         bool header = false) {
  const auto clipped = ellipsize(value, static_cast<size_t>(max(0, rightAlign ? width - 1 : width)));
  const auto text = header ? uiHeaderText(clipped, color) : uiBodyText(clipped, color);
  auto content = rightAlign ? ftxui::hbox({ftxui::filler(), text, ftxui::text(" ")})
                            : ftxui::hbox({text, ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Element centeredCell(const string& value, int width, ftxui::Color color, bool header = false) {
  const auto clipped = ellipsize(value, static_cast<size_t>(max(0, width)));
  const auto text = header ? uiHeaderText(clipped, color) : uiBodyText(clipped, color);
  return ftxui::hbox({ftxui::filler(), text, ftxui::filler()}) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

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

vector<ActivityEntry> recentEventEntries(const vector<ActivityEntry>& activities, size_t limit) {
  vector<ActivityEntry> entries;
  const auto count = min(activities.size(), limit);
  for (size_t offset = 0; offset < count; ++offset) {
    entries.push_back(activities[activities.size() - 1 - offset]);
  }
  return entries;
}

ftxui::Color attentionBackground(const AttentionRow&) {
  return uiSelectionBg();
}

DashboardSnapshot buildDashboardSnapshot(const vector<InventoryItem>& items, const vector<ActivityEntry>& activities,
                                         size_t recentEventLimit, int lowStockThreshold) {
  DashboardSnapshot snapshot;
  snapshot.itemCount = items.size();
  snapshot.recentEvents = recentEventEntries(activities, recentEventLimit);

  unordered_set<string> seenIds;
  for (const auto& item : items) {
    snapshot.totalQuantity += static_cast<size_t>(max(item.quantity, 0));
    const bool missingMetadata = item.hasMissingMetadata();
    snapshot.missingMetadataCount += missingMetadata ? 1 : 0;
    const bool duplicateId = !seenIds.insert(item.id).second;
    const bool dataError = duplicateId || item.quantity < 0;
    const bool outOfStock = item.quantity <= 0;
    const bool lowStock = isLowStock(item, lowStockThreshold);

    snapshot.dataErrorCount += dataError ? 1 : 0;
    snapshot.outOfStockCount += outOfStock ? 1 : 0;
    snapshot.lowStockCount += lowStock ? 1 : 0;

    if (dataError) continue;

    AttentionRow row;
    row.partName = item.partName;
    row.quantity = item.quantity;
    row.location = trim(item.location).empty() ? "-" : item.location;
    if (outOfStock) {
      row.issue = "OUT";
      row.reason = "Replenish stock";
      row.severity = AttentionSeverity::Out;
      row.group = AttentionGroup::Out;
    } else if (lowStock) {
      row.issue = "LOW";
      row.reason = "Threshold " + to_string(lowStockThreshold);
      row.severity = AttentionSeverity::Low;
      row.group = AttentionGroup::Low;
    } else {
      continue;
    }
    snapshot.attention.push_back(move(row));
  }

  sort(snapshot.attention.begin(), snapshot.attention.end(), [](const AttentionRow& lhs, const AttentionRow& rhs) {
    if (lhs.group != rhs.group) return lhs.group < rhs.group;
    if (lhs.quantity != rhs.quantity) return lhs.quantity < rhs.quantity;
    return toLower(lhs.partName) < toLower(rhs.partName);
  });
  return snapshot;
}

ftxui::Element attentionPanel(const DashboardSnapshot& snapshot, int width, int height) {
  const int contentWidth = max(42, width - 2);
  const int severityWidth = 8;
  const int quantityWidth = 7;
  const int partWidth = max(14, contentWidth - severityWidth - quantityWidth - 2);

  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      fixedCell("STATE", severityWidth, uiMutedColor(), false, true),
      uiDivider(),
      fixedCell("PART", partWidth, uiMutedColor(), false, true),
      uiDivider(),
      fixedCell("QTY", quantityWidth, uiMutedColor(), true, true),
  }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth));

  if (snapshot.attention.empty()) {
    rows.push_back(ftxui::vbox({
                       uiHeaderText("  No stock warnings", uiSuccessColor()),
                       uiBodyText("  Every tracked part is above the configured threshold.", uiMutedColor()),
                   }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth) |
                   ftxui::bgcolor(uiSurfaceBg()));
    return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
  }

  const size_t maxRows = static_cast<size_t>(max(1, height - 4));
  const size_t visible = min(snapshot.attention.size(), maxRows);
  const auto rowCount = max<size_t>(1, visible);
  const auto cycleDuration = static_cast<long long>(rowCount) * kWarningRowStepMs + kWarningHoldMs + kWarningRestMs;
  const auto ticks = uiAnimationTicks();
  const auto cycleIndex = static_cast<size_t>(ticks / cycleDuration);
  const auto phase = ticks % cycleDuration;
  const size_t offset = snapshot.attention.size() > visible ? cycleIndex % snapshot.attention.size() : 0;
  const bool holding = phase >= static_cast<long long>(rowCount) * kWarningRowStepMs &&
                       phase < static_cast<long long>(rowCount) * kWarningRowStepMs + kWarningHoldMs;
  const size_t activeRow = phase < static_cast<long long>(rowCount) * kWarningRowStepMs
                               ? static_cast<size_t>(phase / kWarningRowStepMs)
                               : rowCount;

  for (size_t index = 0; index < visible; ++index) {
    const auto& row = snapshot.attention[(offset + index) % snapshot.attention.size()];
    const bool highlighted = holding || index == activeRow;
    const auto background = highlighted ? attentionBackground(row)
                                        : (index % 2 == 0 ? uiCanvasBg() : uiSurfaceBg());
    rows.push_back(ftxui::hbox({
        centeredCell(row.issue, severityWidth, uiPrimaryText(), true),
        uiDivider(),
        centeredCell(row.partName, partWidth, uiPrimaryText()),
        uiDivider(),
        fixedCell(to_string(row.quantity), quantityWidth, uiPrimaryText(), true),
    }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth) |
                   ftxui::bgcolor(background));
  }

  if (snapshot.attention.size() > visible) {
    rows.push_back(uiBodyText("  Rotating " + to_string(snapshot.attention.size() - visible) + " more warning" +
                                 (snapshot.attention.size() - visible == 1 ? "" : "s") + " · live attention sweep",
                             uiMutedColor()));
  }
  return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
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

ftxui::Element activityPanel(const vector<ActivityEntry>& entries, int width) {
  const int contentWidth = max(28, width - 2);
  ftxui::Elements rows;
  rows.push_back(plainSectionTitle("RECENT ACTIVITY", width));
  if (entries.empty()) {
    rows.push_back(uiBodyText("No activity yet.", uiMutedColor()));
    return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
  }
  for (size_t index = 0; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    const auto fullTimestamp = nowTimestampString(entry.timestamp);
    const auto compactTimestamp = fullTimestamp.size() > 5 ? fullTimestamp.substr(5) : fullTimestamp;
    const int timestampWidth = min(11, max(8, contentWidth / 3));
    const int messageWidth = max(8, contentWidth - timestampWidth - 3);
    rows.push_back(ftxui::hbox({
                         fixedCell(compactTimestamp, timestampWidth, uiSecondaryText()),
                         centeredCell("-", 3, uiSecondaryText()),
                         uiBodyText(ellipsize(entry.kind + " " + entry.message,
                                              static_cast<size_t>(messageWidth)),
                                    uiSecondaryText()),
                         ftxui::filler(),
                     }) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth) |
                     ftxui::bgcolor(index % 2 == 0 ? uiSurfaceBg() : uiCanvasBg()));
  }
  return ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
}

}  // namespace

ftxui::Element App::renderDashboardUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 30;
  const size_t recentLimit = static_cast<size_t>(max(3, screenHeight - 13));
  const auto snapshot = buildDashboardSnapshot(store_.items(), activities_, recentLimit, settings_.lowStockThreshold);

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
  const int scannerHeight = scannerPanelHeight(scannerConnected, scannerDashboardTransitionStartedAt_,
                                               scannerDashboardTransitionExpanding_);

  const int minimumRightWidth = 50;
  const int leftWidth = max(48, min(screenWidth - minimumRightWidth - 1, (screenWidth * 45) / 100));
  const int rightWidth = max(1, screenWidth - leftWidth - 1);
  const int dashboardHeight = max(14, screenHeight - 5);
  const int warningHeight = max(8, dashboardHeight - scannerHeight - 1);

  auto warningSide = ftxui::vbox({
                         attentionPanel(snapshot, leftWidth, warningHeight) | ftxui::flex,
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
                       activityPanel(snapshot.recentEvents, rightWidth) | ftxui::flex,
                   }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, rightWidth) |
                   ftxui::flex;

  return ftxui::hbox({warningSide, uiDivider(), healthSide}) |
         ftxui::bgcolor(uiCanvasBg()) |
         ftxui::flex;
}

void App::handleDashboardKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    switch (tolower(static_cast<unsigned char>(key.ch))) {
      case '1':
      case '\t':
        changePage(Page::Stock);
        break;
      case 'n':
        beginEditCurrentItem(true);
        break;
      case '4':
      case 'r':
      case 'd':
        store_.load(inventoryPath_);
        loadInventoryHistory(inventoryPath_, inventoryHistory_);
        if (inventoryHistory_.empty()) {
          appendInventoryHistory(inventoryHistory_,
                                 makeInventoryHistoryPoint(store_.items(), settings_.lowStockThreshold));
        }
        saveInventoryHistory(inventoryPath_, inventoryHistory_);
        setMessage("Inventory reloaded from the database", 2);
        break;
      case '5':
      case 'i':
        beginCsvImport();
        break;
      case 'm':
        openRackManagement();
        break;
      case 'u':
        openInventatoryScanSetup();
        break;
      case 'f':
        changePage(Page::Stock);
        startSearch();
        break;
      case 'l':
        openPrinterSetup();
        break;
      case '/':
        startSearch();
        break;
      case 'q':
        requestUserExit();
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Enter || key.type == KeyType::Tab) changePage(Page::Stock);
}

}  // namespace inventatory
