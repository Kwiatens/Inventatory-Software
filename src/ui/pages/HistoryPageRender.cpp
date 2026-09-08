// Inventatory - Inventory commit history page.
// Browses durable inventory snapshots and their field-level changes.

#include "App.h"

#include "HistoryPagePrivate.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

using history_page_detail::HistoryRecord;
using history_page_detail::groupedHistoryRecords;

namespace history_page_detail {

vector<HistoryRecord> groupedHistoryRecords(const InventoryCommitDetail& detail) {
  vector<HistoryRecord> records;
  unordered_map<string, size_t> indices;
  indices.reserve(detail.changes.size());

  for (const auto& change : detail.changes) {
    const auto key = change.entityType + '\x1f' + change.entityId;
    const auto existing = indices.find(key);
    size_t index = 0;
    if (existing == indices.end()) {
      index = records.size();
      indices.emplace(key, index);
      records.push_back({change.entityType, change.entityId, change.label, {}});
    } else {
      index = existing->second;
    }
    records[index].changes.push_back(change);
  }
  return records;
}

}  // namespace history_page_detail

namespace {



struct HistoryParameterDiff {
  string name;
  string before;
  string after;
};

string shortCommitId(const string& id) {
  return id.empty() ? "-" : id.substr(0, min<size_t>(8, id.size()));
}

string commitMarker(const InventoryCommit& commit) {
  if (commit.checkpoint) return "CHECKPOINT";
  if (commit.corrective) return "CORRECTIVE";
  return commit.source.empty() ? "MANUAL" : toUpper(commit.source);
}

string changeCountText(const InventoryCommit& commit) {
  return to_string(commit.changedItemCount) + " parts, " + to_string(commit.changedRackCount) + " racks";
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

ftxui::Color recordStatusBackground(const string& status) {
  if (status == "DELETED") return uiDangerBg();
  if (status == "ADDED") return uiActiveSoftBg();
  return uiRaisedSurfaceBg();
}

ftxui::Element historyBadge(const string& label, ftxui::Color foreground, ftxui::Color background) {
  return uiHeaderText(" " + label + " ", foreground, background);
}



size_t fieldChangeCount(const HistoryRecord& record) {
  return count_if(record.changes.begin(), record.changes.end(), [](const InventoryFieldChange& change) {
    return change.field != "record";
  });
}

string historyValue(const string& field, const string& value) {
  if (value.empty()) return "(empty)";

  if (field == "last updated" || field == "created at") {
    const bool numeric = all_of(value.begin(), value.end(), [](unsigned char character) {
      return isdigit(character) != 0;
    });
    if (numeric) {
      try {
        const auto timestamp = stoll(value);
        if (timestamp > 0) return nowTimestampString(static_cast<time_t>(timestamp));
      } catch (...) {
        // Preserve an unexpected value exactly as stored.
      }
    }
  }
  return value;
}

vector<string> wrappedHistoryValue(const string& value, int width) {
  vector<string> wrapped;
  const auto softWrapped = wrapText(value, max(1, width));
  for (const auto& line : softWrapped) {
    if (line.empty()) {
      wrapped.push_back({});
      continue;
    }
    for (size_t offset = 0; offset < line.size(); offset += static_cast<size_t>(max(1, width))) {
      wrapped.push_back(line.substr(offset, static_cast<size_t>(max(1, width))));
    }
  }
  if (wrapped.empty()) wrapped.push_back({});
  return wrapped;
}

vector<pair<string, string>> parameterPairs(const string& encoded) {
  vector<pair<string, string>> pairs;
  if (encoded.empty()) return pairs;

  size_t start = 0;
  while (start <= encoded.size()) {
    const auto end = encoded.find(';', start);
    const auto segment = trim(encoded.substr(start, end == string::npos ? string::npos : end - start));
    if (!segment.empty()) {
      const auto separator = segment.find('=');
      if (separator == string::npos) return {};
      pairs.emplace_back(trim(segment.substr(0, separator)), trim(segment.substr(separator + 1)));
    }
    if (end == string::npos) break;
    start = end + 1;
  }
  return pairs;
}

vector<HistoryParameterDiff> parameterDiffs(const string& before, const string& after) {
  const auto beforePairs = parameterPairs(before);
  const auto afterPairs = parameterPairs(after);
  if (beforePairs.empty() && !before.empty()) return {};
  if (afterPairs.empty() && !after.empty()) return {};

  unordered_map<string, string> beforeValues;
  unordered_map<string, string> afterValues;
  vector<string> order;
  beforeValues.reserve(beforePairs.size());
  afterValues.reserve(afterPairs.size());

  for (const auto& pair : beforePairs) {
    beforeValues[pair.first] = pair.second;
    order.push_back(pair.first);
  }
  for (const auto& pair : afterPairs) {
    afterValues[pair.first] = pair.second;
    if (find(order.begin(), order.end(), pair.first) == order.end()) order.push_back(pair.first);
  }

  vector<HistoryParameterDiff> diffs;
  for (const auto& name : order) {
    const auto beforeValue = beforeValues.find(name);
    const auto afterValue = afterValues.find(name);
    const auto oldValue = beforeValue == beforeValues.end() ? "(absent)" : beforeValue->second;
    const auto newValue = afterValue == afterValues.end() ? "(absent)" : afterValue->second;
    if (oldValue != newValue) diffs.push_back({name, oldValue, newValue});
  }
  return diffs;
}

void appendHistoryValueRows(ftxui::Elements& rows, const string& label, const string& value, int width,
                            ftxui::Color color) {
  const auto prefix = "  " + label + ": ";
  const int valueWidth = max(10, width - static_cast<int>(prefix.size()));
  const auto wrapped = wrappedHistoryValue(value, valueWidth);
  for (size_t index = 0; index < wrapped.size(); ++index) {
    const auto continuation = string(prefix.size(), ' ');
    rows.push_back(fullLine((index == 0 ? prefix : continuation) + wrapped[index], color, uiSurfaceBg()));
  }
}

void appendHistoryFieldDiff(ftxui::Elements& rows, const InventoryFieldChange& change, int width) {
  if (change.field == "record") return;

  const auto parameterChanges = (change.field == "parameters" || change.field == "vendor parameters")
                                    ? parameterDiffs(change.before, change.after)
                                    : vector<HistoryParameterDiff>();
  if (!parameterChanges.empty()) {
    rows.push_back(fullLine(prettyLabel(change.field) + "  " + to_string(parameterChanges.size()) +
                                (parameterChanges.size() == 1 ? " parameter changed" : " parameters changed"),
                            uiLabelColor(), uiSurfaceBg()));
    for (const auto& parameter : parameterChanges) {
      rows.push_back(fullLine("  " + prettyLabel(parameter.name), uiSecondaryText(), uiSurfaceBg()));
      appendHistoryValueRows(rows, "Before", parameter.before, width, uiMutedColor());
      appendHistoryValueRows(rows, "After", parameter.after, width, uiPrimaryText());
    }
    return;
  }

  rows.push_back(fullLine(prettyLabel(change.field), uiLabelColor(), uiSurfaceBg()));
  appendHistoryValueRows(rows, "Before", historyValue(change.field, change.before), width, uiMutedColor());
  appendHistoryValueRows(rows, "After", historyValue(change.field, change.after), width,
                         change.after == "deleted" ? uiDangerColor() : uiPrimaryText());
}

ftxui::Element historyRow(const InventoryCommit& commit, int width, bool selected) {
  const auto timestamp = nowTimestampString(commit.timestamp);
  const auto summary = "#" + to_string(commit.sequence) + "  [" + commitMarker(commit) + "]  " + commit.message;
  const auto rowColor = selected ? uiPrimaryText() : uiSecondaryText();
  auto row = ftxui::vbox({
               uiBodyText(ellipsize(summary, static_cast<size_t>(max(10, width - 2))), rowColor),
               uiBodyText(ellipsize("  " + timestamp + "  ·  " + changeCountText(commit),
                                   static_cast<size_t>(max(10, width - 2))),
                          selected ? uiInfoColor() : uiMutedColor()),
           }) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width) |
           ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
  if (selected) row = row | ftxui::select;
  return row;
}

ftxui::Element historyRecordRow(const HistoryRecord& record, int width, bool selected) {
  const auto status = recordStatus(record);
  const auto fieldCount = fieldChangeCount(record);
  const auto summary = status == "MODIFIED"
                           ? to_string(fieldCount) + (fieldCount == 1 ? " field changed" : " fields changed")
                           : status == "ADDED" ? "record added" : "record deleted";
  const auto label = toUpper(record.entityType) + "  " + record.label;
  const int badgeWidth = static_cast<int>(status.size()) + 2;
  const auto labelWidth = max(10, width - badgeWidth - 3);

  auto row = ftxui::vbox({
               ftxui::hbox({historyBadge(status, recordStatusColor(status), recordStatusBackground(status)),
                            uiBodyText(" ", uiSurfaceBg()),
                            uiBodyText(ellipsize(label, static_cast<size_t>(labelWidth)),
                                       selected ? uiPrimaryText() : uiSecondaryText()),
                            ftxui::filler()}),
               uiBodyText(ellipsize("  " + summary, static_cast<size_t>(max(10, width - 2))),
                          selected ? uiInfoColor() : uiMutedColor()),
           }) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width) |
           ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
  if (selected) row = row | ftxui::select;
  return row;
}

}  // namespace

ftxui::Element App::renderHistoryUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int contentWidth = max(60, screenWidth - 2);
  const int listWidth = max(42, min(58, contentWidth * 42 / 100));
  const int detailWidth = max(18, contentWidth - listWidth - 1);
  const auto records = historyDetailValid_ ? groupedHistoryRecords(historyDetail_) : vector<HistoryRecord>();
  const bool recordOpen = historyRecordOpen_ && !records.empty();
  const auto recordSelection = records.empty() ? size_t(0) : min(historyRecordSelection_, records.size() - 1);
  auto self = const_cast<App*>(this);

  ftxui::Elements listRows;
  listRows.push_back(uiSectionHeader("COMMITS  " + to_string(inventoryCommits_.size()), uiSecondaryText(),
                                     uiSurfaceBg()));
  if (inventoryCommits_.empty()) {
    listRows.push_back(fullLine("No inventory commits yet.", uiMutedColor(), uiSurfaceBg()));
    listRows.push_back(fullLine("The first launch creates an Initial inventory baseline.", uiMutedColor(), uiSurfaceBg()));
  } else {
    for (size_t index = 0; index < inventoryCommits_.size(); ++index) {
      const auto& commit = inventoryCommits_[index];
      const bool selected = index == historySelection_;
      auto row = historyRow(commit, listWidth, selected);
      listRows.push_back(target(move(row), "history.commit." + commit.id, UiTargetKind::Row,
                                [self, index] {
                                  self->historySelection_ = index;
                                  self->historyRecordSelection_ = 0;
                                  self->historyRecordOpen_ = false;
                                  self->refreshHistoryDetail();
                                  self->dirty_ = true;
                                }, true, true));
    }
  }
  listRows.push_back(uiDivider());
  listRows.push_back(fullLine("Up/Down select  Enter inspect  C checkpoint", uiDimColor(), uiSurfaceBg()));
  auto listPanel = ftxui::vbox(move(listRows)) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listWidth) |
                   ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex |
                   ftxui::reflect(historyListPanelBounds_);

  const auto canReverse = historyDetailValid_ && historyDetail_.hasParent && !historyDetail_.changes.empty();
  const auto appendHistoryActions = [&](ftxui::Elements& rows) {
    rows.push_back(ftxui::hbox({
        target(uiSecondaryButton("Restore snapshot", historyDetailValid_ ? optional<ftxui::Color>()
                                                                          : optional<ftxui::Color>(uiMutedColor()),
                                  historyDetailValid_),
               "history.restore", UiTargetKind::Button, [self] {
                 self->beginHistoryRestore(InventoryRevertMode::Snapshot);
               },
               historyDetailValid_),
        ftxui::text("  "),
        target(uiSecondaryButton("Reverse changes", uiWarnColor(), canReverse), "history.reverse", UiTargetKind::Button,
               [self] { self->beginHistoryRestore(InventoryRevertMode::Reverse); }, canReverse),
        ftxui::filler(),
    }));
  };

  ftxui::Elements detailRows;
  if (!historyDetailValid_) {
    detailRows.push_back(uiSectionHeader("COMMIT DETAIL", uiSecondaryText(), uiSurfaceBg()));
    detailRows.push_back(fullLine("Select a commit to inspect its inventory changes.", uiMutedColor(), uiSurfaceBg()));
  } else if (recordOpen) {
    const auto& commit = historyDetail_.commit;
    const auto& record = records[recordSelection];
    const auto status = recordStatus(record);
    detailRows.push_back(uiSectionHeader("RECORD DETAIL", uiSecondaryText(), uiSurfaceBg()));
    detailRows.push_back(ftxui::hbox({historyBadge(status, recordStatusColor(status), recordStatusBackground(status)),
                                      uiBodyText("  " + record.entityType + "  " + record.label, uiTitleColor()),
                                      ftxui::filler()}));
    detailRows.push_back(fullLine("Commit #" + to_string(commit.sequence) + "  " + commit.message, uiMutedColor(),
                                  uiSurfaceBg()));
    detailRows.push_back(fullLine("ID: " + record.entityId, uiMutedColor(), uiSurfaceBg()));
    appendHistoryActions(detailRows);
    detailRows.push_back(uiDivider());

    if (status == "ADDED") {
      detailRows.push_back(fullLine("This record was added to the inventory.", uiSuccessColor(), uiSurfaceBg()));
    } else if (status == "DELETED") {
      detailRows.push_back(fullLine("This record was deleted from the inventory.", uiDangerColor(), uiSurfaceBg()));
    }

    for (const auto& change : record.changes) appendHistoryFieldDiff(detailRows, change, detailWidth - 2);
    if (fieldChangeCount(record) == 0) {
      detailRows.push_back(fullLine("No field-level values are available for this record change.", uiMutedColor(),
                                    uiSurfaceBg()));
    }
    detailRows.push_back(uiDivider());
    detailRows.push_back(fullLine("Esc back to changed records  Up/Down next record", uiDimColor(), uiSurfaceBg()));
  } else {
    const auto& commit = historyDetail_.commit;
    const auto kind = commitMarker(commit);
    detailRows.push_back(uiSectionHeader("COMMIT DETAIL", uiSecondaryText(), uiSurfaceBg()));
    detailRows.push_back(ftxui::hbox({historyBadge(kind, commit.checkpoint ? uiWarnColor() : uiAccentColor(),
                                                   commit.checkpoint ? uiWarningBg() : uiRaisedSurfaceBg()),
                                      uiBodyText("  #" + to_string(commit.sequence) + "  " + commit.message,
                                                 uiTitleColor()),
                                      ftxui::filler()}));
    detailRows.push_back(fullLine("Committed: " + nowTimestampString(commit.timestamp) + "  ·  Parent: " +
                                      (historyDetail_.hasParent ? "#" + shortCommitId(commit.parentId) : "none"),
                                  uiSecondaryText(), uiSurfaceBg()));
    detailRows.push_back(fullLine("Source: " + (commit.source.empty() ? "manual" : commit.source), uiSecondaryText(),
                                  uiSurfaceBg()));
    if (!commit.reference.empty()) {
      detailRows.push_back(fullLine("Reference: " + commit.reference, uiSecondaryText(), uiSurfaceBg()));
    }
    if (!commit.revertedCommitId.empty()) {
      detailRows.push_back(fullLine("Corrects: #" + shortCommitId(commit.revertedCommitId), uiWarnColor(), uiSurfaceBg()));
    }
    detailRows.push_back(fullLine("Impact: " + to_string(commit.changedItemCount) + " parts changed  ·  " +
                                      to_string(commit.changedRackCount) + " racks changed",
                                  commit.checkpoint ? uiWarnColor() : uiInfoColor(), uiSurfaceBg()));
    appendHistoryActions(detailRows);
    detailRows.push_back(uiDivider());
    detailRows.push_back(uiSectionHeader("CHANGED RECORDS  " + to_string(records.size()), uiSecondaryText(),
                                         uiSurfaceBg()));
    if (records.empty()) {
      detailRows.push_back(fullLine(commit.checkpoint ? "Snapshot-only checkpoint; inventory unchanged."
                                                       : "No inventory field changes.",
                                    uiMutedColor(), uiSurfaceBg()));
    } else {
      for (size_t index = 0; index < records.size(); ++index) {
        const bool selected = index == recordSelection;
        auto row = historyRecordRow(records[index], detailWidth, selected);
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
    detailRows.push_back(fullLine(records.empty() ? "C checkpoint  ·  Space actions"
                                                   : "Up/Down select record  Enter inspect  ·  Space actions",
                                  uiDimColor(), uiSurfaceBg()));
  }

  if (inputMode_ == InputMode::HistoryConfirm) {
    detailRows.push_back(uiDivider());
    detailRows.push_back(fullLine("CONFIRM HISTORY ACTION", uiWarnColor(), uiWarningBg()));
    detailRows.push_back(fullLine(ellipsize(historyConfirmationMessage_, static_cast<size_t>(max(10, detailWidth - 2))),
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
