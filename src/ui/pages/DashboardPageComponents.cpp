// Inventatory - Dashboard warning snapshot and table components.

#include "ui/pages/DashboardPagePrivate.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace inventatory::dashboard_detail {

using namespace std;

ftxui::Element fixedCell(const string& value, int width, ftxui::Color color, bool rightAlign,
                         bool header) {
  const auto clipped = ellipsize(value, static_cast<size_t>(max(0, rightAlign ? width - 1 : width)));
  const auto text = header ? uiHeaderText(clipped, color) : uiBodyText(clipped, color);
  auto content = rightAlign ? ftxui::hbox({ftxui::filler(), text, ftxui::text(" ")})
                            : ftxui::hbox({text, ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Element centeredCell(const string& value, int width, ftxui::Color color, bool header) {
  const auto clipped = ellipsize(value, static_cast<size_t>(max(0, width)));
  const auto text = header ? uiHeaderText(clipped, color) : uiBodyText(clipped, color);
  return ftxui::hbox({ftxui::filler(), text, ftxui::filler()}) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}
ftxui::Color attentionBackground(const AttentionRow&) {
  return uiSelectionBg();
}

DashboardSnapshot buildDashboardSnapshot(const vector<InventoryItem>& items, int lowStockThreshold) {
  DashboardSnapshot snapshot;
  snapshot.itemCount = items.size();

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
      row.reason = "Threshold " + to_string(effectiveReorderThreshold(item, lowStockThreshold));
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

ftxui::Element attentionPanel(const DashboardSnapshot& snapshot, int width, size_t selectedRow, bool active,
                              const function<ftxui::Element(ftxui::Element, size_t)>& wrapRow) {
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

  for (size_t index = 0; index < snapshot.attention.size(); ++index) {
    const auto& row = snapshot.attention[index];
    const bool selected = index == selectedRow;
    const auto background = selected && active ? attentionBackground(row)
                                               : (index % 2 == 0 ? uiCanvasBg() : uiSurfaceBg());
    auto renderedRow = ftxui::hbox({
        centeredCell(row.issue, severityWidth, uiPrimaryText(), true),
        uiDivider(),
        centeredCell(row.partName, partWidth, uiPrimaryText()),
        uiDivider(),
        fixedCell(to_string(row.quantity), quantityWidth, uiPrimaryText(), true),
    }) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, contentWidth) |
                   ftxui::bgcolor(background);
    if (selected) {
      renderedRow = renderedRow | ftxui::select;
    }
    rows.push_back(wrapRow(move(renderedRow), index));
  }
  return ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator |
         ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
}

}  // namespace inventatory::dashboard_detail
