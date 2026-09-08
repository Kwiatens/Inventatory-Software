// Inventatory - Inventory commit history page.
// Handles history navigation and actions.

#include "App.h"

#include "HistoryPagePrivate.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace inventatory {

using namespace std;
using history_page_detail::HistoryRecord;
using history_page_detail::groupedHistoryRecords;

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

  if (historyRecordOpen_) {
    const auto records = historyDetailValid_ ? groupedHistoryRecords(historyDetail_) : vector<HistoryRecord>();
    if (records.empty()) {
      historyRecordOpen_ = false;
      historyRecordSelection_ = 0;
    } else if (key.type == KeyType::Escape) {
      historyRecordOpen_ = false;
      dirty_ = true;
      return;
    } else if (key.type == KeyType::Up || key.type == KeyType::PageUp || key.type == KeyType::Down ||
               key.type == KeyType::PageDown || key.type == KeyType::Home || key.type == KeyType::End) {
      const auto current = static_cast<int>(min(historyRecordSelection_, records.size() - 1));
      int delta = 0;
      if (key.type == KeyType::Up) delta = -1;
      if (key.type == KeyType::PageUp) delta = -10;
      if (key.type == KeyType::Down) delta = 1;
      if (key.type == KeyType::PageDown) delta = 10;
      if (key.type == KeyType::Home) {
        historyRecordSelection_ = 0;
        dirty_ = true;
        return;
      }
      if (key.type == KeyType::End) {
        historyRecordSelection_ = records.size() - 1;
        dirty_ = true;
        return;
      }
      historyRecordSelection_ = static_cast<size_t>(clamp(current + delta, 0,
                                                           static_cast<int>(records.size() - 1)));
      dirty_ = true;
      return;
    }
  }

  if (key.type == KeyType::Escape) {
    changePage(Page::Home);
    return;
  }
  if (key.type == KeyType::Enter) {
    if (!historyDetailValid_) return;
    const auto records = groupedHistoryRecords(historyDetail_);
    if (records.empty()) {
      setMessage("The selected commit has no changed records", 3);
      return;
    }
    historyRecordSelection_ = min(historyRecordSelection_, records.size() - 1);
    historyRecordOpen_ = true;
    dirty_ = true;
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
      historyRecordSelection_ = 0;
      historyRecordOpen_ = false;
      refreshHistoryDetail();
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::End) {
    if (!inventoryCommits_.empty()) {
      historySelection_ = inventoryCommits_.size() - 1;
      historyRecordSelection_ = 0;
      historyRecordOpen_ = false;
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

void App::moveHistoryRecordSelection(int delta) {
  if (!historyDetailValid_) return;
  const auto records = groupedHistoryRecords(historyDetail_);
  if (records.empty()) {
    historyRecordSelection_ = 0;
    historyRecordOpen_ = false;
    dirty_ = true;
    return;
  }
  const auto current = static_cast<int>(min(historyRecordSelection_, records.size() - 1));
  historyRecordSelection_ = static_cast<size_t>(clamp(current + delta, 0,
                                                       static_cast<int>(records.size() - 1)));
  dirty_ = true;
}

}  // namespace inventatory
