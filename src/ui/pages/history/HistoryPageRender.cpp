// Inventatory - Inventory commit history page.
// Renders the flat, terminal-style two-pane history inspector.

#include "App.h"

#include "HistoryPagePrivate.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;
using history_page_detail::HistoryCommitGroup;
using history_page_detail::HistoryFieldDiff;
using history_page_detail::HistoryRecord;
using history_page_detail::filteredHistoryIndices;
using history_page_detail::groupedHistoryCommits;
using history_page_detail::groupedHistoryRecords;
using history_page_detail::historyCommitImpactSummary;
using history_page_detail::historyCommitType;
using history_page_detail::historyFieldDiffs;
using history_page_detail::historyPaneWidths;
using history_page_detail::historySourceFilterLabel;

namespace {

string historyTime(const InventoryCommit& commit) {
  const auto timestamp = nowTimestampString(commit.timestamp);
  return timestamp.size() > 11 ? timestamp.substr(11) : timestamp;
}

string historyDate(const InventoryCommit& commit) {
  const auto timestamp = nowTimestampString(commit.timestamp);
  return timestamp.size() > 10 ? timestamp.substr(0, 10) : timestamp;
}

string shortCommitId(const string& id) {
  return id.empty() ? "-" : id.substr(0, min<size_t>(8, id.size()));
}

ftxui::Color historyTypeColor(const string& type) {
  if (type == "CORRECTIVE") return uiWarnColor();
  if (type == "CHECKPOINT") return uiLinkColor();
  if (type == "DIGIKEY") return uiInfoColor();
  if (type == "IMPORT") return uiInfoColor();
  if (type == "PROJECT") return uiSuccessColor();
  return uiAccentColor();
}

ftxui::Color historyTypeBackground(const string& type) {
  if (type == "CORRECTIVE") return uiWarningBg();
  if (type == "CHECKPOINT") return uiRaisedSurfaceBg();
  if (type == "PROJECT") return uiActiveSoftBg();
  return uiRaisedSurfaceBg();
}

ftxui::Element historyBadge(const string& label, ftxui::Color foreground, ftxui::Color background) {
  return uiHeaderText(" " + label + " ", foreground, background);
}

ftxui::Element fixedText(const string& value, int width, ftxui::Color foreground,
                         optional<ftxui::Color> background = nullopt) {
  const auto safeWidth = max(1, width);
  return styledText(ellipsize(value, static_cast<size_t>(safeWidth)), foreground, background) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, safeWidth);
}

string changedCount(size_t count, const char* singular, const char* plural) {
  return to_string(count) + " " + (count == 1 ? singular : plural) + " changed";
}

string lowercaseCopy(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(tolower(character));
  });
  return value;
}

string compactHistoryMessage(const InventoryCommit& commit) {
  const auto message = commit.message.empty() ? string("(no message)") : commit.message;
  const auto lower = lowercaseCopy(message);
  const string reversePrefix = "reversed changes from commit ";
  if (lower.rfind(reversePrefix, 0) == 0) return "Reverted " + message.substr(reversePrefix.size());
  if (lower.rfind("digikey refresh batch", 0) == 0) return "DigiKey refresh";
  return message;
}

ftxui::Element historyCommitRow(const InventoryCommit& commit, int width, bool selected) {
  const auto type = historyCommitType(commit);
  const auto impact = historyCommitImpactSummary(commit);
  const bool hasImpact = commit.changedItemCount != 0 || commit.changedRackCount != 0;
  const int safeWidth = max(30, width);
  const bool compact = safeWidth < 70;
  const int timelineWidth = 2;
  const int sequenceWidth = 5;
  const int typeWidth = max(8, static_cast<int>(type.size()) + 2);
  const int timeWidth = 6;
  const int impactWidth = hasImpact && !compact ? min(22, max(12, safeWidth / 4)) : 0;
  const int messageWidth = max(8, safeWidth - timelineWidth - sequenceWidth - typeWidth - timeWidth - impactWidth);
  const auto foreground = selected ? uiPrimaryText() : uiSecondaryText();
  const auto metadata = selected ? uiInfoColor() : uiMutedColor();
  const auto rowBackground = selected ? uiSelectionBg() : uiSurfaceBg();

  auto timeline = styledText("●", historyTypeColor(type), rowBackground) |
                  ftxui::size(ftxui::WIDTH, ftxui::EQUAL, timelineWidth);
  auto sequence = fixedText("#" + to_string(commit.sequence), sequenceWidth, foreground, rowBackground);
  auto badge = historyBadge(type, historyTypeColor(type), historyTypeBackground(type)) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, typeWidth);
  const auto message = compact ? compactHistoryMessage(commit)
                               : (commit.message.empty() ? string("(no message)") : commit.message);
  auto messageElement = fixedText(message, messageWidth, foreground, rowBackground);
  auto time = ftxui::hbox({ftxui::filler(), styledText(historyTime(commit), metadata, rowBackground)}) |
              ftxui::size(ftxui::WIDTH, ftxui::EQUAL, timeWidth);

  ftxui::Elements columns = {move(timeline), move(sequence), move(badge), move(messageElement)};
  if (impactWidth > 0) columns.push_back(fixedText(impact, impactWidth, metadata, rowBackground));
  columns.push_back(move(time));
  auto row = ftxui::hbox(move(columns)) |
             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, safeWidth) |
             ftxui::bgcolor(rowBackground);
  if (selected) row = row | ftxui::select;
  return row;
}

ftxui::Element historyGroupHeader(const HistoryCommitGroup& group, const vector<InventoryCommit>& commits, int width) {
  const auto date = group.indices.empty() ? string() : historyDate(commits[group.indices.front()]);
  const auto count = to_string(group.indices.size()) + (group.indices.size() == 1 ? " commit" : " commits");
  return ftxui::hbox({uiHeaderText(group.label, uiPrimaryText()), styledText("  " + date, uiSecondaryText()),
                      ftxui::filler(), styledText("(" + count + ")", uiMutedColor())}) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, max(1, width)) | ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element historyImpactStrip(const string& icon, const string& value, int width) {
  const auto background = uiRaisedSurfaceBg();
  const int safeWidth = max(8, width);
  return ftxui::hbox({styledText(icon, uiAccentColor(), background) |
                          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 3),
                      uiHeaderText(ellipsize(value, static_cast<size_t>(max(8, safeWidth - 5))), uiPrimaryText(),
                                   background),
                      ftxui::filler()}) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, safeWidth) | ftxui::bgcolor(background);
}

string joinCategoryPath(const vector<string>& values) {
  string joined;
  for (const auto& value : values) {
    if (value.empty()) continue;
    if (!joined.empty()) joined += " > ";
    joined += value;
  }
  return joined;
}

const InventoryItem* historyItemFor(const InventoryCommitDetail& detail, const HistoryRecord& record) {
  if (record.entityType != "item") return nullptr;
  if (const auto* item = detail.snapshot.findById(record.entityId)) return item;
  return detail.parentSnapshot.findById(record.entityId);
}

const InventatoryRack* historyRackFor(const InventoryCommitDetail& detail, const HistoryRecord& record) {
  if (record.entityType != "rack") return nullptr;
  for (const auto& rack : detail.snapshot.racks()) {
    if (rack.id == record.entityId) return &rack;
  }
  for (const auto& rack : detail.parentSnapshot.racks()) {
    if (rack.id == record.entityId) return &rack;
  }
  return nullptr;
}

string recordStatus(const HistoryRecord& record) {
  bool added = false;
  bool deleted = false;
  for (const auto& change : record.changes) {
    if (change.field != "record") continue;
    added = added || change.after == "added";
    deleted = deleted || change.after == "deleted";
  }
  if (deleted) return "DELETED";
  if (added) return "ADDED";
  return "MODIFIED";
}

ftxui::Color recordStatusColor(const string& status) {
  if (status == "ADDED") return uiSuccessColor();
  if (status == "DELETED") return uiDangerColor();
  return uiAccentColor();
}

ftxui::Element historyRecordCard(const InventoryCommitDetail& detail, const HistoryRecord& record, size_t index,
                                  int width, bool selected) {
  const auto* item = historyItemFor(detail, record);
  const auto* rack = historyRackFor(detail, record);
  const auto code = record.label.empty() ? record.entityId : record.label;
  string description;
  string category;
  string mpn;
  if (item != nullptr) {
    description = item->vendorMetadata.detailedDescription;
    if (description.empty()) description = item->vendorMetadata.title;
    if (description.empty()) description = item->partName;
    category = joinCategoryPath(item->vendorMetadata.categoryPath);
    if (category.empty()) category = item->category;
    mpn = item->vendorMetadata.manufacturerPartNumber;
    if (mpn.empty()) mpn = item->sku;
  } else if (rack != nullptr) {
    description = rack->componentType;
    category = "Rack";
  }
  if (description.empty()) description = "No description available";
  if (category.empty()) category = toUpper(record.entityType);

  const auto status = recordStatus(record);
  const auto background = selected ? uiActiveSoftBg() : uiSurfaceBg();
  const int safeWidth = max(12, width);
  const int markerWidth = 1;
  const int numberWidth = 5;
  const int mainWidth = max(1, safeWidth - markerWidth - numberWidth - 1);
  const bool narrow = mainWidth < 24;
  const int rightWidth = narrow ? 0 : max(10, min(28, mainWidth / 2));
  const int descriptionWidth = narrow ? mainWidth : max(10, mainWidth - rightWidth - 1);
  auto marker = styledText(selected ? "▌" : " ", selected ? uiInteractiveColor() : background, background) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, markerWidth);
  auto number = styledText(" " + to_string(index + 1) + " ", uiTitleColor(),
                          selected ? uiSelectionBg() : uiRaisedSurfaceBg()) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, numberWidth);
  auto primary = narrow
                    ? ftxui::hbox({uiHeaderText(ellipsize(code, static_cast<size_t>(mainWidth)),
                                                 recordStatusColor(status)),
                                   ftxui::filler()})
                    : ftxui::hbox({uiHeaderText(ellipsize(code, static_cast<size_t>(max(8, mainWidth - rightWidth - 2))),
                                                 recordStatusColor(status)),
                                   ftxui::filler(), fixedText(category, rightWidth, uiMutedColor())});
  auto secondary = narrow
                       ? fixedText(description, mainWidth, uiSecondaryText())
                       : ftxui::hbox({fixedText(description, descriptionWidth, uiSecondaryText()), ftxui::filler(),
                                      fixedText(mpn.empty() ? string() : "MPN: " + mpn, rightWidth, uiInfoColor())});
  auto content = ftxui::vbox({move(primary), move(secondary)}) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, mainWidth);
  auto card = ftxui::hbox({move(marker), move(number), styledText(" ", background), move(content)}) |
              ftxui::size(ftxui::WIDTH, ftxui::EQUAL, safeWidth) | ftxui::bgcolor(background);
  if (selected) card = card | ftxui::select;
  return card;
}

ftxui::Element historyDiffCell(const string& value, int width, ftxui::Color foreground,
                               optional<ftxui::Color> background = nullopt) {
  const int safeWidth = max(4, width);
  return styledText(" " + ellipsize(value, static_cast<size_t>(max(1, safeWidth - 2))) + " ", foreground, background) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, safeWidth);
}

ftxui::Element historyDiffTable(const vector<HistoryFieldDiff>& diffs, int width) {
  const int safeWidth = max(34, width);
  const int fieldWidth = max(12, safeWidth * 25 / 100);
  const int arrowWidth = 4;
  const int valueWidth = max(8, (safeWidth - fieldWidth - arrowWidth) / 2);
  const auto previousHeader = safeWidth < 68 ? string("Previous") : string("Previous value");
  const auto newHeader = safeWidth < 68 ? string("New") : string("New value");
  const auto headerBackground = uiRaisedSurfaceBg();
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({historyDiffCell("Field", fieldWidth, uiSecondaryText(), headerBackground),
                              historyDiffCell(previousHeader, valueWidth, uiSecondaryText(), headerBackground),
                              historyDiffCell("", arrowWidth, uiSecondaryText(), headerBackground),
                              historyDiffCell(newHeader, valueWidth, uiSecondaryText(), headerBackground)}) |
                 ftxui::bgcolor(headerBackground));
  rows.push_back(uiDivider());
  for (const auto& diff : diffs) {
    rows.push_back(ftxui::hbox({historyDiffCell(diff.field, fieldWidth, uiLabelColor(), uiSurfaceBg()),
                                historyDiffCell(diff.previous, valueWidth, uiDangerColor(), uiDangerBg()),
                                ftxui::hbox({ftxui::filler(), styledText("→", uiSecondaryText()), ftxui::filler()}) |
                                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL, arrowWidth) |
                                    ftxui::bgcolor(uiSurfaceBg()),
                                historyDiffCell(diff.next, valueWidth, uiSuccessColor(), uiActiveSoftBg())}));
  }
  return ftxui::vbox(move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, safeWidth) |
         ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element historyMetaLine(const string& label, const string& value, int width, ftxui::Color valueColor) {
  const int safeWidth = max(1, width);
  const int labelWidth = min(safeWidth, static_cast<int>(label.size()) + 1);
  const int valueWidth = max(1, safeWidth - labelWidth);
  return ftxui::hbox({fixedText(label + " ", labelWidth, uiSecondaryText()),
                      fixedText(value, valueWidth, valueColor)}) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, safeWidth);
}

}  // namespace

ftxui::Element App::renderHistoryUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int contentWidth = max(60, screenWidth - 2);
  const auto paneWidths = historyPaneWidths(contentWidth);
  const int listWidth = paneWidths[0];
  const int detailWidth = paneWidths[1];
  const int detailInnerWidth = max(12, detailWidth);
  const auto groups = groupedHistoryCommits(inventoryCommits_, historySearchQuery_, historySourceFilter_, time(nullptr));
  const auto records = historyDetailValid_ ? groupedHistoryRecords(historyDetail_) : vector<HistoryRecord>();
  const auto recordSelection = records.empty() ? size_t(0) : min(historyRecordSelection_, records.size() - 1);
  auto self = const_cast<App*>(this);

  const bool editingHistorySearch = inputMode_ == InputMode::HistorySearch;
  const auto displayedHistoryQuery = editingHistorySearch ? inputBuffer_ : historySearchQuery_;
  const auto displayedSearchValue = displayedHistoryQuery.empty() ? "/" : "/" + displayedHistoryQuery;
  const int controlsWidth = max(24, listWidth);
  const int filterWidth = min(22, max(18, controlsWidth / 3));
  const int searchWidth = max(12, controlsWidth - filterWidth - 1);
  const auto searchForeground = editingHistorySearch ? uiFocusColor() : uiInteractiveColor();
  const auto searchBackground = editingHistorySearch ? uiActiveSoftBg() : uiRaisedSurfaceBg();
  auto searchControl = target(ftxui::hbox({uiHeaderText(" Search: ", searchForeground, searchBackground),
                                           styledText(displayedSearchValue + (editingHistorySearch ? "_" : " "),
                                                      searchForeground, searchBackground),
                                           ftxui::filler()}) |
                                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL, searchWidth) |
                                    ftxui::bgcolor(searchBackground),
                                "history.search", UiTargetKind::Field, [self] { self->startHistorySearch(); });
  const auto filterBackground = uiRaisedSurfaceBg();
  auto filterControl = target(ftxui::hbox({uiHeaderText(" Filter: ", uiInteractiveColor(), filterBackground),
                                            styledText(historySourceFilterLabel(historySourceFilter_) + " v ",
                                                       uiInteractiveColor(), filterBackground),
                                            ftxui::filler()}) |
                                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, filterWidth) |
                                     ftxui::bgcolor(filterBackground),
                                 "history.filter", UiTargetKind::Field, [self] { self->cycleHistoryFilter(); });
  auto controls = ftxui::hbox({move(searchControl), styledText(" ", uiSurfaceBg()), move(filterControl)}) |
                  ftxui::size(ftxui::WIDTH, ftxui::EQUAL, controlsWidth);

  ftxui::Elements listRows;
  listRows.push_back(move(controls));
  listRows.push_back(uiDivider());
  if (inventoryCommits_.empty()) {
    listRows.push_back(uiSectionHeader("No commits", uiSecondaryText(), uiSurfaceBg()));
    listRows.push_back(fullLine("The first saved inventory state will appear here.", uiMutedColor(), uiSurfaceBg()));
  } else if (groups.empty()) {
    listRows.push_back(uiSectionHeader("No matching commits", uiSecondaryText(), uiSurfaceBg()));
    listRows.push_back(fullLine("Try a different search or source filter.", uiMutedColor(), uiSurfaceBg()));
  } else {
    for (const auto& group : groups) {
      listRows.push_back(historyGroupHeader(group, inventoryCommits_, listWidth));
      for (const auto index : group.indices) {
        const auto& commit = inventoryCommits_[index];
        const bool selected = index == historySelection_;
        auto row = historyCommitRow(commit, listWidth, selected);
        listRows.push_back(target(move(row), "history.commit." + commit.id, UiTargetKind::Row,
                                  [self, index] {
                                    self->historySelection_ = index;
                                    self->historyRecordSelection_ = 0;
                                    self->historyRecordOpen_ = false;
                                    self->refreshHistoryDetail();
                                    self->dirty_ = true;
                                  },
                                  true, true));
      }
      listRows.push_back(uiDivider());
    }
  }
  auto listPanel = ftxui::vbox(move(listRows)) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listWidth) |
                   ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex |
                   ftxui::reflect(historyListPanelBounds_);

  const bool canReverse = historyDetailValid_ && historyDetail_.hasParent && !historyDetail_.changes.empty();
  const bool compactDetail = detailInnerWidth < 68;
  const int actionWidth = max(10, (detailInnerWidth - 1) / 2);
  auto revertAction = target(uiPrimaryButton(compactDetail ? "↶ Revert (v)" : "↶ Revert commit (v)", canReverse) |
                                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, actionWidth),
                             "history.reverse", UiTargetKind::Button,
                             [self] { self->beginHistoryRestore(InventoryRevertMode::Reverse); }, canReverse);
  auto restoreAction = target(uiSecondaryButton(compactDetail ? "◇ Restore (s)" : "◇ Restore state at this point (s)",
                                                nullopt, historyDetailValid_) |
                                  ftxui::size(ftxui::WIDTH, ftxui::EQUAL, actionWidth),
                              "history.restore", UiTargetKind::Button,
                              [self] { self->beginHistoryRestore(InventoryRevertMode::Snapshot); }, historyDetailValid_);

  ftxui::Elements detailRows;
  if (!historyDetailValid_) {
    detailRows.push_back(uiSectionHeader("Commit detail", uiSecondaryText(), uiSurfaceBg()));
    detailRows.push_back(fullLine(groups.empty() ? "Select a matching commit to inspect its changes."
                                                 : "Select a commit to inspect its changes.",
                                  uiMutedColor(), uiSurfaceBg()));
  } else {
    const auto& commit = historyDetail_.commit;
    const auto type = historyCommitType(commit);
    const string parent = historyDetail_.hasParent
                              ? "#" + to_string(commit.sequence - 1) + " (" + shortCommitId(commit.parentId) + ")"
                              : "none";
    const string reference = commit.reference.empty() ? "-" : commit.reference;
    detailRows.push_back(uiSectionHeader("Commit #" + to_string(commit.sequence), historyTypeColor(type), uiSurfaceBg()));
    detailRows.push_back(fixedText(commit.message.empty() ? "(no message)" : commit.message, detailInnerWidth,
                                   uiTitleColor(), uiSurfaceBg()));
    if (detailInnerWidth >= 72) {
      const int metaWidth = (detailInnerWidth - 1) / 2;
      detailRows.push_back(ftxui::hbox({historyMetaLine("Committed:", nowTimestampString(commit.timestamp), metaWidth,
                                                            uiSecondaryText()),
                                        styledText(" ", uiSurfaceBg()),
                                        historyMetaLine("Parent:", parent, metaWidth, uiSecondaryText())}));
      detailRows.push_back(ftxui::hbox({historyMetaLine("Source:", type, metaWidth, historyTypeColor(type)),
                                        styledText(" ", uiSurfaceBg()),
                                        historyMetaLine("Reference:", reference, metaWidth, uiInfoColor())}));
    } else {
      detailRows.push_back(historyMetaLine("Committed:", nowTimestampString(commit.timestamp), detailInnerWidth,
                                           uiSecondaryText()));
      detailRows.push_back(historyMetaLine("Parent:", parent, detailInnerWidth, uiSecondaryText()));
      detailRows.push_back(historyMetaLine("Source:", type, detailInnerWidth, historyTypeColor(type)));
      if (!commit.reference.empty()) {
        detailRows.push_back(historyMetaLine("Reference:", reference, detailInnerWidth, uiInfoColor()));
      }
    }
    if (!commit.revertedCommitId.empty()) {
      detailRows.push_back(historyMetaLine("Corrects:", "#" + shortCommitId(commit.revertedCommitId), detailInnerWidth,
                                           uiWarnColor()));
    }
    detailRows.push_back(ftxui::hbox({historyImpactStrip("P", changedCount(commit.changedItemCount, "part", "parts"),
                                                        actionWidth),
                                      styledText(" ", uiSurfaceBg()),
                                      historyImpactStrip("R", changedCount(commit.changedRackCount, "rack", "racks"),
                                                        actionWidth)}));
    detailRows.push_back(ftxui::hbox({move(revertAction), styledText(" ", uiSurfaceBg()), move(restoreAction)}));
    detailRows.push_back(uiDivider());
    detailRows.push_back(uiSectionHeader("Changed records  (" + to_string(records.size()) + ")", uiSecondaryText(),
                                         uiSurfaceBg()));
    if (records.empty()) {
      detailRows.push_back(fullLine(commit.checkpoint ? "Snapshot-only checkpoint; inventory unchanged."
                                                       : "No inventory field changes.",
                                    uiMutedColor(), uiSurfaceBg()));
    } else {
      for (size_t index = 0; index < records.size(); ++index) {
        auto row = historyRecordCard(historyDetail_, records[index], index, detailInnerWidth, index == recordSelection);
        detailRows.push_back(target(move(row), "history.record." + to_string(index), UiTargetKind::Row,
                                     [self, index] {
                                       self->historyRecordSelection_ = index;
                                       self->historyRecordOpen_ = true;
                                       self->dirty_ = true;
                                     },
                                     true, true));
      }
    }
    detailRows.push_back(uiDivider());
    detailRows.push_back(uiSectionHeader("Field changes", uiSecondaryText(), uiSurfaceBg()));
    if (records.empty()) {
      detailRows.push_back(fullLine("No changed record is available for field-level preview.", uiMutedColor(),
                                    uiSurfaceBg()));
    } else {
      const auto diffs = historyFieldDiffs(records[recordSelection]);
      if (diffs.empty()) {
        detailRows.push_back(fullLine("This record has no field-level values to compare.", uiMutedColor(),
                                      uiSurfaceBg()));
      } else {
        detailRows.push_back(historyDiffTable(diffs, detailInnerWidth));
      }
    }
    detailRows.push_back(uiDivider());
    detailRows.push_back(fullLine(historyRecordOpen_ ? "Esc back to commit  Up/Down next record"
                                                     : "Up/Down select record  Enter view record",
                                  uiDimColor(), uiSurfaceBg()));
  }

  if (inputMode_ == InputMode::HistoryConfirm) {
    detailRows.push_back(uiDivider());
    detailRows.push_back(fullLine("Confirm history action", uiWarnColor(), uiWarningBg()));
    detailRows.push_back(fullLine(ellipsize(historyConfirmationMessage_, static_cast<size_t>(detailInnerWidth)),
                                  uiTitleColor(), uiWarningBg()));
    detailRows.push_back(fullLine("Enter confirm  Esc cancel", uiAccentColor(), uiWarningBg()));
  }

  auto detailPanel = ftxui::vbox(move(detailRows)) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, detailWidth) |
                     ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex |
                     ftxui::reflect(historyDetailPanelBounds_);

  return ftxui::hbox({move(listPanel), uiDivider(), move(detailPanel)}) |
         ftxui::bgcolor(uiCanvasBg()) | ftxui::flex;
}

}  // namespace inventatory
