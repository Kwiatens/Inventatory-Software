// Inventatory - Inventory commit history page.
// Browses durable inventory snapshots and their field-level changes.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

string shortCommitId(const string& id) {
  return id.empty() ? "-" : id.substr(0, min<size_t>(8, id.size()));
}

string commitMarker(const InventoryCommit& commit) {
  if (commit.checkpoint) return "checkpoint";
  if (commit.corrective) return "corrective";
  return commit.source;
}

string changeCountText(const InventoryCommit& commit) {
  return to_string(commit.changedItemCount) + " parts, " + to_string(commit.changedRackCount) + " racks";
}

ftxui::Element historyRow(const InventoryCommit& commit, int width, bool selected, bool active) {
  const auto timestamp = nowTimestampString(commit.timestamp);
  const auto summary = "#" + to_string(commit.sequence) + " " + shortCommitId(commit.id) + " " +
                       commitMarker(commit) + " " + commit.message;
  auto row = ftxui::vbox({
               uiBodyText(ellipsize(summary, static_cast<size_t>(max(10, width - 2))),
                          selected && active ? uiPrimaryText() : uiSecondaryText()),
               uiBodyText(ellipsize("  " + timestamp + "  " + changeCountText(commit),
                                   static_cast<size_t>(max(10, width - 2))),
                          selected && active ? uiInfoColor() : uiMutedColor()),
           }) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width) |
           ftxui::bgcolor(selected && active ? uiSelectionBg()
                                              : (commit.corrective ? uiActiveSoftBg()
                                                                    : (commit.sequence % 2 == 0 ? uiSurfaceBg()
                                                                                                 : uiCanvasBg())));
  if (selected) row = row | ftxui::select;
  return row;
}

string changeValue(const string& value) {
  return value.empty() ? "(empty)" : value;
}

}  // namespace

ftxui::Element App::renderHistoryUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int contentWidth = max(60, screenWidth - 2);
  const int listWidth = max(42, min(58, contentWidth * 42 / 100));
  const int detailWidth = max(18, contentWidth - listWidth - 1);
  auto self = const_cast<App*>(this);

  ftxui::Elements listRows;
  listRows.push_back(fullLine("COMMITS  " + to_string(inventoryCommits_.size()), uiSecondaryText(), uiSurfaceBg()));
  if (inventoryCommits_.empty()) {
    listRows.push_back(fullLine("No inventory commits yet.", uiMutedColor(), uiSurfaceBg()));
    listRows.push_back(fullLine("The first launch creates an Initial inventory baseline.", uiMutedColor(), uiSurfaceBg()));
  } else {
    for (size_t index = 0; index < inventoryCommits_.size(); ++index) {
      const auto& commit = inventoryCommits_[index];
      const bool selected = index == historySelection_;
      auto row = historyRow(commit, listWidth, selected, true);
      listRows.push_back(target(move(row), "history.commit." + commit.id, UiTargetKind::Row,
                                [self, index] {
                                  self->historySelection_ = index;
                                  self->refreshHistoryDetail();
                                  self->dirty_ = true;
                                }, true, true));
    }
  }
  listRows.push_back(uiDivider());
  listRows.push_back(fullLine("Up/Down select  Enter open  C checkpoint", uiDimColor(), uiSurfaceBg()));
  auto listPanel = ftxui::vbox(move(listRows)) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listWidth) |
                   ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;

  ftxui::Elements detailRows;
  detailRows.push_back(fullLine("COMMIT DETAIL", uiSecondaryText(), uiSurfaceBg()));
  if (!historyDetailValid_) {
    detailRows.push_back(fullLine("Select a commit to inspect its inventory changes.", uiMutedColor(), uiSurfaceBg()));
  } else {
    const auto& commit = historyDetail_.commit;
    detailRows.push_back(fullLine("#" + to_string(commit.sequence) + "  " + shortCommitId(commit.id) +
                                      "  " + commit.message,
                                  uiTitleColor(), uiSurfaceBg()));
    detailRows.push_back(fullLine("Time: " + nowTimestampString(commit.timestamp) + "  Source: " + commit.source,
                                  uiSecondaryText(), uiSurfaceBg()));
    if (!commit.reference.empty()) {
      detailRows.push_back(fullLine("Reference: " + commit.reference, uiSecondaryText(), uiSurfaceBg()));
    }
    detailRows.push_back(fullLine("Parent: " + (historyDetail_.hasParent ? shortCommitId(commit.parentId) : "none"),
                                  uiSecondaryText(), uiSurfaceBg()));
    detailRows.push_back(fullLine("Changed: " + changeCountText(commit) +
                                      (commit.checkpoint ? "  [checkpoint]" : ""),
                                  commit.checkpoint ? uiWarnColor() : uiInfoColor(), uiSurfaceBg()));
    detailRows.push_back(uiDivider());
    if (historyDetail_.changes.empty()) {
      detailRows.push_back(fullLine(commit.checkpoint ? "Snapshot-only checkpoint; no field changes."
                                                       : "No inventory field changes.",
                                    uiMutedColor(), uiSurfaceBg()));
    } else {
      for (const auto& change : historyDetail_.changes) {
        const auto title = change.entityType + "  " + change.label + "  " + change.field;
        const auto values = changeValue(change.before) + " -> " + changeValue(change.after);
        detailRows.push_back(fullLine(ellipsize(title, static_cast<size_t>(max(10, detailWidth - 2))),
                                      uiLabelColor(), uiSurfaceBg()));
        detailRows.push_back(fullLine("  " + ellipsize(values, static_cast<size_t>(max(10, detailWidth - 2))),
                                      change.after == "deleted" ? uiDangerColor() : uiSecondaryText(),
                                      uiSurfaceBg()));
      }
    }
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
                     ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;

  return ftxui::hbox({move(listPanel), uiDivider(), move(detailPanel)}) |
         ftxui::bgcolor(uiCanvasBg()) | ftxui::flex;
}

void App::handleHistoryKey(const KeyEvent& key) {
  if (inputMode_ == InputMode::HistoryCheckpoint) {
    if (key.type == KeyType::Escape) {
      cancelHistoryAction();
      return;
    }
    if (key.type == KeyType::Backspace) {
      if (!inputBuffer_.empty()) inputBuffer_.pop_back();
      dirty_ = true;
      return;
    }
    if (key.type == KeyType::Enter) {
      const auto name = trim(inputBuffer_);
      if (name.empty()) {
        setMessage("Checkpoint name cannot be empty", 3);
        return;
      }
      InventoryCommitDraft draft;
      draft.source = "checkpoint";
      draft.message = name;
      draft.checkpoint = true;
      const bool saved = saveInventoryState(draft);
      cancelHistoryAction();
      if (saved) setMessage("Checkpoint created", 3);
      return;
    }
    if (key.type == KeyType::Character && inputBuffer_.size() < 160) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
    }
    return;
  }

  if (inputMode_ == InputMode::HistoryConfirm) {
    if (key.type == KeyType::Enter) {
      applyHistoryRevert(pendingHistoryRevertMode_);
    } else if (key.type == KeyType::Escape) {
      cancelHistoryAction();
    }
    return;
  }

  if (key.type == KeyType::Escape) {
    changePage(Page::Home);
    return;
  }
  if (key.type == KeyType::Up) {
    moveHistorySelection(-1);
    return;
  }
  if (key.type == KeyType::Down) {
    moveHistorySelection(1);
    return;
  }
  if (key.type == KeyType::PageUp) {
    moveHistorySelection(-10);
    return;
  }
  if (key.type == KeyType::PageDown) {
    moveHistorySelection(10);
    return;
  }
  if (key.type == KeyType::Home) {
    if (!inventoryCommits_.empty()) {
      historySelection_ = 0;
      refreshHistoryDetail();
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::End) {
    if (!inventoryCommits_.empty()) {
      historySelection_ = inventoryCommits_.size() - 1;
      refreshHistoryDetail();
      dirty_ = true;
    }
    return;
  }
  if (key.type != KeyType::Character) return;

  const auto ch = static_cast<char>(tolower(static_cast<unsigned char>(key.ch)));
  if (ch == 'c') {
    beginHistoryCheckpoint();
  } else if (ch == 's') {
    beginHistoryRestore(InventoryRevertMode::Snapshot);
  } else if (ch == 'v') {
    beginHistoryRestore(InventoryRevertMode::Reverse);
  } else if (ch == 'r') {
    refreshInventoryCommits();
    setMessage("Inventory history reloaded", 2);
    dirty_ = true;
  }
}

}  // namespace inventatory
