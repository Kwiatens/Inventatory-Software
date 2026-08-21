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

struct AttentionLine {
  string title;
  const AttentionRow* row = nullptr;
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

ftxui::Element fixedCell(const string& value, int width, ftxui::Color color, bool rightAlign = false) {
  const auto clipped = ellipsize(value, static_cast<size_t>(max(0, rightAlign ? width - 1 : width)));
  auto content = rightAlign ? ftxui::hbox({ftxui::filler(), styledText(clipped, color), ftxui::text(" ")})
                            : ftxui::hbox({styledText(clipped, color), ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Color attentionColor(AttentionSeverity severity) {
  if (severity == AttentionSeverity::Out) return uiDangerColor();
  if (severity == AttentionSeverity::Low) return uiWarnColor();
  return uiSecondaryText();
}

ftxui::Element metricCard(const string& label, size_t value, ftxui::Color valueColor) {
  return ftxui::vbox({
             styledText(label, uiMutedColor()),
             styledText(to_string(value), valueColor) | ftxui::bold,
         }) |
         ftxui::bgcolor(uiRaisedSurfaceBg()) | ftxui::flex;
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
  (void)entry;
  // Activity is background context, so keep it readable without competing
  // with the current stock alerts and primary controls.
  return uiSecondaryText();
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

vector<AttentionLine> attentionLines(const DashboardSnapshot& snapshot) {
  vector<AttentionLine> lines;
  AttentionGroup currentGroup = AttentionGroup::Out;
  bool groupStarted = false;
  for (const auto& row : snapshot.attention) {
    if (!groupStarted || row.group != currentGroup) {
      currentGroup = row.group;
      groupStarted = true;
      const auto title = currentGroup == AttentionGroup::Out
                             ? "OUT OF STOCK"
                             : "LOW STOCK";
      lines.push_back({title, nullptr});
    }
    lines.push_back({{}, &row});
  }
  return lines;
}

ftxui::Element attentionPanel(const DashboardSnapshot& snapshot, int width, int height) {
  // Keep the compact Home table visually centred inside its panel rather than
  // pinning all of its content against the application's left edge.
  const int contentWidth = max(30, width - 4);
  const int quantityWidth = 7;
  const int partWidth = max(16, contentWidth - quantityWidth - 1);
  const auto centred = [&](ftxui::Element row) {
    return ftxui::hbox({
        ftxui::filler(),
        row | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth),
        ftxui::filler(),
    });
  };

  ftxui::Elements rows;
  rows.push_back(centred(ftxui::hbox({
      fixedCell("Part", partWidth, uiMutedColor()),
      ftxui::separator() | ftxui::color(uiDividerColor()),
      fixedCell("Qty", quantityWidth, uiMutedColor(), true),
  })));

  const auto lines = attentionLines(snapshot);
  if (lines.empty()) {
    // Keep the empty panel quiet; the zero in the title already communicates
    // that there are no attention items.
  } else {
    const auto visible = min(lines.size(), static_cast<size_t>(max(4, height - 4)));
    const auto offset = lines.size() > visible
                           ? static_cast<size_t>((uiAnimationTicks() / 1500) % lines.size())
                           : 0;
    for (size_t index = 0; index < visible; ++index) {
      const auto& line = lines[(offset + index) % lines.size()];
      if (line.row == nullptr) {
        const auto headerColor = line.title == "OUT OF STOCK"
                                     ? uiDangerColor()
                                     : uiWarnColor();
        rows.push_back(centred(styledText(" " + line.title, headerColor, uiRaisedSurfaceBg()) |
                               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth)));
        continue;
      }
      const auto& row = *line.row;
      const auto background = index % 2 == 0 ? uiCanvasBg() : uiSurfaceBg();
      const auto accent = attentionColor(row.severity);
      rows.push_back(centred(ftxui::hbox({
          fixedCell(row.partName, partWidth, accent),
          ftxui::separator() | ftxui::color(uiDividerColor()),
          fixedCell(to_string(row.quantity), quantityWidth, accent, true),
      }) | ftxui::bgcolor(background)));
    }
  }
  return panel("NEEDS ATTENTION  " + to_string(snapshot.attention.size()), move(rows), uiPrimaryText(),
               uiDividerColor()) | ftxui::flex;
}

}  // namespace

ftxui::Element App::renderDashboardUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 30;
  const size_t recentLimit = static_cast<size_t>(max(3, screenHeight - 7));
  auto snapshot = buildDashboardSnapshot(store_.items(), activities_, recentLimit, settings_.lowStockThreshold);

  auto metrics = ftxui::hbox({
      metricCard("TOTAL PARTS TRACKED", snapshot.itemCount, uiPrimaryText()), uiDivider(),
      metricCard("TOTAL UNITS TRACKED", snapshot.totalQuantity, uiPrimaryText()), uiDivider(),
      metricCard("LOW STOCK", snapshot.lowStockCount, snapshot.lowStockCount > 0 ? uiWarnColor() : uiSuccessColor()),
      uiDivider(),
      metricCard("OUT OF STOCK", snapshot.outOfStockCount, snapshot.outOfStockCount > 0 ? uiDangerColor() : uiSuccessColor()),
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

  auto queue = attentionPanel(snapshot, alertWidth - 2, screenHeight - 5) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, alertWidth);
  auto activitySide = recentPanel | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, activityWidth) | ftxui::flex;
  auto mainContent = ftxui::hbox({queue, uiDivider(), activitySide}) | ftxui::flex;

  return ftxui::vbox({metrics, uiDivider(), mainContent}) | ftxui::bgcolor(uiCanvasBg());
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
