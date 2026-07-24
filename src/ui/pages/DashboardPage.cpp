// Inventatory - Hardware Inventory Management System
// Attention-first dashboard rendering and keyboard handling.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

enum class AttentionSeverity {
  Metadata = 1,
  Low = 2,
  Out = 3,
  Data = 4,
};

struct AttentionRow {
  string issue;
  string partName;
  string location;
  string reason;
  int quantity = 0;
  AttentionSeverity severity = AttentionSeverity::Metadata;
};

struct DeviceStatus {
  string name;
  bool connected = false;
  bool flashing = false;
  string endpoint;
  string lastActivity;
  string error;
};

struct DashboardSnapshot {
  size_t itemCount = 0;
  size_t totalQuantity = 0;
  size_t lowStockCount = 0;
  size_t outOfStockCount = 0;
  size_t dataErrorCount = 0;
  size_t missingMetadataCount = 0;
  size_t unenrichedCount = 0;
  vector<AttentionRow> attention;
  vector<DeviceStatus> devices;
  vector<ActivityEntry> recentEvents;
  string lastScannedPart;
};

ftxui::Element fixedCell(const string& value, int width, ftxui::Color color, bool rightAlign = false) {
  const auto clipped = ellipsize(value, static_cast<size_t>(max(0, width)));
  auto content = rightAlign ? ftxui::hbox({ftxui::filler(), styledText(clipped, color)})
                            : ftxui::hbox({styledText(clipped, color), ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Color attentionColor(AttentionSeverity severity) {
  if (severity == AttentionSeverity::Data || severity == AttentionSeverity::Out) return uiDangerColor();
  if (severity == AttentionSeverity::Low) return uiWarnColor();
  return uiLinkColor();
}

ftxui::Element metricCard(const string& label, size_t value, ftxui::Color valueColor) {
  return ftxui::vbox({
             styledText(label, uiMutedColor()),
             styledText(to_string(value), valueColor) | ftxui::bold,
         }) |
         ftxui::bgcolor(uiRaisedSurfaceBg()) | ftxui::flex;
}

ftxui::Element statusChip(bool connected, bool flashing) {
  const auto foreground = connected ? uiSuccessColor() : uiDangerColor();
  const auto background = flashing ? uiSelectionBg()
                                    : (connected ? ftxui::Color::RGB(24, 43, 34)
                                                 : ftxui::Color::RGB(47, 27, 27));
  return styledText(connected ? " ONLINE " : " OFFLINE ", foreground, background) | ftxui::bold;
}

ftxui::Element deviceRow(const DeviceStatus& device) {
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      styledText(device.name, uiPrimaryText()),
      ftxui::filler(),
      statusChip(device.connected, device.flashing),
  }));
  rows.push_back(styledText(ellipsize(device.endpoint.empty() ? "-" : device.endpoint, 48), uiMutedColor()));
  if (!device.error.empty()) {
    rows.push_back(styledText(ellipsize(device.error, 48), uiWarnColor()));
  }
  return ftxui::vbox(move(rows));
}

vector<ActivityEntry> recentEventEntries(const vector<ActivityEntry>& activities, size_t limit) {
  vector<ActivityEntry> entries;
  const auto count = min(activities.size(), limit);
  for (size_t offset = 0; offset < count; ++offset) {
    entries.push_back(activities[activities.size() - 1 - offset]);
  }
  return entries;
}

ftxui::Color recentEventColor(const ActivityEntry& entry) {
  if (entry.kind.find("scan") != string::npos) return uiLinkColor();
  if (entry.kind.find("print") != string::npos) return uiAccentColor();
  if (entry.kind.find("delete") != string::npos) return uiDangerColor();
  if (entry.kind.find("add") != string::npos) return uiSuccessColor();
  return uiSecondaryText();
}

DashboardSnapshot buildDashboardSnapshot(const vector<InventoryItem>& items, const vector<ActivityEntry>& activities,
                                         bool scannerRunning, bool deviceConnected, size_t recentEventLimit) {
  DashboardSnapshot snapshot;
  snapshot.itemCount = items.size();
  snapshot.recentEvents = recentEventEntries(activities, recentEventLimit);

  unordered_set<string> seenIds;
  for (const auto& item : items) {
    snapshot.totalQuantity += static_cast<size_t>(max(item.quantity, 0));
    const bool missingMetadata = item.hasMissingMetadata();
    const bool unenriched = toLower(item.catalogueStatus) != "matched";
    snapshot.missingMetadataCount += missingMetadata ? 1 : 0;
    snapshot.unenrichedCount += unenriched ? 1 : 0;

    const bool duplicateId = !seenIds.insert(item.id).second;
    const bool dataError = duplicateId || item.quantity < 0 || item.reorderThreshold < 0;
    const bool outOfStock = item.quantity <= 0;
    // A zero threshold deliberately means that this item has no configured
    // reorder alert. The dashboard must use the same per-item rule shown in
    // the detail pane, rather than silently substituting a category default.
    const int threshold = item.reorderThreshold;
    const bool lowStock = item.quantity > 0 && item.lowStock();

    snapshot.dataErrorCount += dataError ? 1 : 0;
    snapshot.outOfStockCount += outOfStock ? 1 : 0;
    snapshot.lowStockCount += lowStock ? 1 : 0;

    AttentionRow row;
    row.partName = item.partName;
    row.quantity = item.quantity;
    row.location = trim(item.location).empty() ? "-" : item.location;
    if (dataError) {
      row.issue = "DATA";
      row.reason = duplicateId ? "Duplicate identifier" : "Invalid stock value";
      row.severity = AttentionSeverity::Data;
    } else if (outOfStock) {
      row.issue = "OUT";
      row.reason = "Replenish stock";
      row.severity = AttentionSeverity::Out;
    } else if (lowStock) {
      row.issue = "LOW";
      row.reason = "Threshold " + to_string(threshold);
      row.severity = AttentionSeverity::Low;
    } else if (missingMetadata) {
      row.issue = "META";
      row.reason = "Complete part metadata";
      row.severity = AttentionSeverity::Metadata;
    } else if (unenriched) {
      row.issue = "catalogue";
      row.reason = item.catalogueStatus == "database_unavailable" ? "Database unavailable" : "No reviewed match";
      row.severity = AttentionSeverity::Metadata;
    } else {
      continue;
    }
    snapshot.attention.push_back(move(row));
  }

  for (auto it = activities.rbegin(); it != activities.rend(); ++it) {
    if (it->kind.find("scan") != string::npos) {
      snapshot.lastScannedPart = it->message;
      break;
    }
  }

  snapshot.devices = {
      {"Inventatory Scan R1", deviceConnected, false,
       scannerRunning ? "Device service ready" : "Device service offline",
       snapshot.lastScannedPart.empty() ? "Waiting for scans" : snapshot.lastScannedPart,
       deviceConnected ? string() : (scannerRunning ? "No recent device status" : "Scanner server is not running")},
      {"Label Printer", false, false, "Not configured", "No print jobs yet", "Printer integration pending"},
  };

  sort(snapshot.attention.begin(), snapshot.attention.end(), [](const AttentionRow& lhs, const AttentionRow& rhs) {
    if (lhs.severity != rhs.severity) return lhs.severity > rhs.severity;
    if (lhs.quantity != rhs.quantity) return lhs.quantity < rhs.quantity;
    return toLower(lhs.partName) < toLower(rhs.partName);
  });
  return snapshot;
}

ftxui::Element attentionPanel(const DashboardSnapshot& snapshot, int width, int height, size_t offset,
                              bool outOfStockFlashOn) {
  // Keep the compact Home table visually centred inside its panel rather than
  // pinning all of its content against the application's left edge.
  const int contentWidth = max(30, width - 4);
  const int issueWidth = 6;
  const int quantityWidth = 7;
  const int partWidth = max(16, contentWidth - issueWidth - quantityWidth - 2);
  const auto centred = [&](ftxui::Element row) {
    return ftxui::hbox({
        ftxui::filler(),
        row | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth),
        ftxui::filler(),
    });
  };

  ftxui::Elements rows;
  rows.push_back(centred(ftxui::hbox({
      fixedCell("Status", issueWidth, uiMutedColor()),
      ftxui::separator() | ftxui::color(uiDividerColor()),
      fixedCell("Part", partWidth, uiMutedColor()),
      ftxui::separator() | ftxui::color(uiDividerColor()),
      fixedCell("Qty", quantityWidth, uiMutedColor(), true),
  })));

  if (snapshot.attention.empty()) {
    rows.push_back(centred(fullLine("All inventory records are healthy.", uiSuccessColor(), uiSurfaceBg())));
  } else {
    const auto visible = min(snapshot.attention.size(), static_cast<size_t>(max(4, height - 4)));
    for (size_t index = 0; index < visible; ++index) {
      const auto& row = snapshot.attention[(offset + index) % snapshot.attention.size()];
      const auto background = index % 2 == 0 ? uiCanvasBg() : uiSurfaceBg();
      const auto accent = attentionColor(row.severity);
      const bool outOfStock = row.severity == AttentionSeverity::Out;
      const auto partBackground = outOfStock
                                      ? (outOfStockFlashOn ? ftxui::Color::RGB(89, 35, 31)
                                                           : ftxui::Color::RGB(58, 29, 27))
                                      : background;
      rows.push_back(centred(ftxui::hbox({
          fixedCell(row.issue, issueWidth, accent) | ftxui::bold,
          ftxui::separator() | ftxui::color(uiDividerColor()),
          fixedCell(row.partName, partWidth, outOfStock ? uiPrimaryText() : uiPrimaryText()) |
              ftxui::bgcolor(partBackground),
          ftxui::separator() | ftxui::color(uiDividerColor()),
          fixedCell(to_string(row.quantity), quantityWidth, accent, true),
      }) | ftxui::bgcolor(background)));
    }
  }
  return panel("STOCK STATUS  " + to_string(snapshot.attention.size()), move(rows), uiFocusColor(),
               uiDividerColor()) | ftxui::flex;
}

}  // namespace

ftxui::Element App::renderDashboardUi() const {
  const auto now = time(nullptr);
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 30;
  const bool deviceConnected = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15;
  const size_t recentLimit = static_cast<size_t>(max(3, screenHeight - 10));
  auto snapshot = buildDashboardSnapshot(store_.items(), activities_, server_.running(), deviceConnected, recentLimit);

  if (snapshot.devices.size() > 1) {
    snapshot.devices[1].connected = printerService_.hasConfiguredPrinter();
    snapshot.devices[1].flashing = now <= printerFlashUntil_;
    snapshot.devices[1].endpoint = printerService_.hasConfiguredPrinter() ? "Printer connected" : "Not configured";
    snapshot.devices[1].lastActivity = printerSummary();
    snapshot.devices[1].error = printerCheck_.ok ? string() :
        (printerCheck_.message.empty() ? "Printer is not ready" : printerCheck_.message);
  }
  if (!snapshot.devices.empty()) snapshot.devices[0].flashing = now <= scannerFlashUntil_;

  auto self = const_cast<App*>(this);
  auto operations = ftxui::hbox({
      styledText("OPERATIONS", uiSecondaryText()), ftxui::text("   "),
      target(styledText(" Review stock ", uiInteractiveColor(), uiRaisedSurfaceBg()), "home.stock",
             UiTargetKind::Button, [self] { self->changePage(Page::Stock); }), ftxui::text("  "),
      target(styledText(" Add part ", uiInteractiveColor(), uiRaisedSurfaceBg()), "home.add",
             UiTargetKind::Button, [self] { self->beginEditCurrentItem(true); }), ftxui::text("  "),
      target(styledText(" Import CSV ", uiInteractiveColor(), uiRaisedSurfaceBg()), "home.import",
             UiTargetKind::Button, [self] { self->changePage(Page::Import); }), ftxui::text("  "),
      target(styledText(" Set up Scan R1 ", uiFocusColor(), uiRaisedSurfaceBg()), "home.scan.setup",
             UiTargetKind::Button, [self] { self->openInventatoryScanSetup(); }), ftxui::text("  "),
      target(styledText(" Settings ", uiInteractiveColor(), uiRaisedSurfaceBg()), "home.settings",
             UiTargetKind::Button, [self] { self->openSettings(); }),
      ftxui::filler(),
  });

  auto metrics = ftxui::hbox({
      metricCard("TOTAL PARTS TRACKED", snapshot.itemCount, uiPrimaryText()), uiDivider(),
      metricCard("TOTAL UNITS TRACKED", snapshot.totalQuantity, uiPrimaryText()), uiDivider(),
      metricCard("LOW STOCK", snapshot.lowStockCount, snapshot.lowStockCount > 0 ? uiWarnColor() : uiSuccessColor()),
      uiDivider(),
      metricCard("OUT OF STOCK", snapshot.outOfStockCount, snapshot.outOfStockCount > 0 ? uiDangerColor() : uiSuccessColor()),
      uiDivider(),
      metricCard("NOT IN catalogue", snapshot.unenrichedCount, snapshot.unenrichedCount > 0 ? uiLinkColor() : uiSuccessColor()),
  });

  const int alertWidth = max(50, (screenWidth - 1) / 2);
  const int activityWidth = max(50, screenWidth - alertWidth - 1);
  const int activityTextWidth = max(30, activityWidth - 4);
  ftxui::Elements activityRows;
  if (snapshot.recentEvents.empty()) {
    activityRows.push_back(styledText("No activity yet.", uiMutedColor()));
  } else {
    for (const auto& entry : snapshot.recentEvents) {
      activityRows.push_back(styledText(
          ellipsize(nowTimestampString(entry.timestamp) + "  " + entry.kind + "  " + entry.message,
                    static_cast<size_t>(activityTextWidth)),
          recentEventColor(entry)));
    }
  }
  auto recentPanel = panel("RECENT ACTIVITY", move(activityRows), uiSecondaryText(), uiDividerColor()) | ftxui::flex;

  // Keep the alert list stable. Inventory triage needs a predictable start
  // point, not a timer-driven carousel that can move a critical row away.
  auto queue = attentionPanel(snapshot, alertWidth - 2, screenHeight - 9, 0, true) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, alertWidth);
  auto activitySide = recentPanel | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, activityWidth) | ftxui::flex;
  auto mainContent = ftxui::hbox({queue, uiDivider(), activitySide}) | ftxui::flex;

  return ftxui::vbox({operations, uiDivider(), metrics, uiDivider(), mainContent}) |
         ftxui::bgcolor(uiCanvasBg());
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
          appendInventoryHistory(inventoryHistory_, makeInventoryHistoryPoint(store_.items()));
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
