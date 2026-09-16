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
using history_page_detail::HistorySourceFilter;
using history_page_detail::filteredHistoryIndices;
using history_page_detail::groupedHistoryRecords;

namespace {

HistorySourceFilter nextHistoryFilter(HistorySourceFilter filter) {
  switch (filter) {
    case HistorySourceFilter::All:
      return HistorySourceFilter::Manual;
    case HistorySourceFilter::Manual:
      return HistorySourceFilter::Corrective;
    case HistorySourceFilter::Corrective:
      return HistorySourceFilter::DigiKey;
    case HistorySourceFilter::DigiKey:
      return HistorySourceFilter::Checkpoint;
    case HistorySourceFilter::Checkpoint:
      return HistorySourceFilter::Import;
    case HistorySourceFilter::Import:
      return HistorySourceFilter::Project;
    case HistorySourceFilter::Project:
      return HistorySourceFilter::All;
  }
  return HistorySourceFilter::All;
}

}  // namespace

void App::startHistorySearch() {
  historySearchBeforeEdit_ = historySearchQuery_;
  inputBuffer_ = historySearchQuery_;
  inputMode_ = InputMode::HistorySearch;
  setMessage("Type to filter immediately; Enter keeps it, Esc restores the previous filter", 3);
  dirty_ = true;
}

void App::handleHistorySearchKey(const KeyEvent& key) {
  if (key.type == KeyType::Escape) {
    historySearchQuery_ = historySearchBeforeEdit_;
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    syncHistorySelectionToFilter();
    setMessage("History search cancelled", 2);
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      inputBuffer_.pop_back();
      historySearchQuery_ = inputBuffer_;
      syncHistorySelectionToFilter();
    }
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Enter) {
    historySearchQuery_ = inputBuffer_;
    inputMode_ = InputMode::None;
    syncHistorySelectionToFilter();
    setMessage(historySearchQuery_.empty() ? "History search cleared" : "History search applied", 2);
    dirty_ = true;
    return;
  }
  if (key.type != KeyType::Character || inputBuffer_.size() >= 160) return;
  if (static_cast<unsigned char>(key.ch) < 32 || static_cast<unsigned char>(key.ch) > 126) return;
  inputBuffer_.push_back(key.ch);
  historySearchQuery_ = inputBuffer_;
  syncHistorySelectionToFilter();
  dirty_ = true;
}

void App::cycleHistoryFilter() {
  historySourceFilter_ = nextHistoryFilter(historySourceFilter_);
  syncHistorySelectionToFilter();
  setMessage("History filter: " + history_page_detail::historySourceFilterLabel(historySourceFilter_), 2);
  dirty_ = true;
}

void App::syncHistorySelectionToFilter() {
  const auto visible = filteredHistoryIndices(inventoryCommits_, historySearchQuery_, historySourceFilter_);
  historyRecordSelection_ = 0;
  historyRecordOpen_ = false;

  if (visible.empty()) {
    historySelection_ = 0;
    historyDetail_ = {};
    historyDetailValid_ = false;
    dirty_ = true;
    return;
  }

  const auto selectedId = historySelection_ < inventoryCommits_.size()
                              ? inventoryCommits_[historySelection_].id
                              : string();
  const auto selected = find_if(visible.begin(), visible.end(), [&](size_t index) {
    return !selectedId.empty() && inventoryCommits_[index].id == selectedId;
  });
  historySelection_ = selected == visible.end() ? visible.front() : *selected;
  refreshHistoryDetail();
  dirty_ = true;
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
        setMessage("Checkpoint name cannot be empty", 3, UiMessageSeverity::Warning);
        return;
      }
      InventoryCommitDraft draft;
      draft.source = "checkpoint";
      draft.message = name;
      draft.checkpoint = true;
      const bool saved = saveInventoryState(draft);
      cancelHistoryAction();
      if (saved) setMessage("Checkpoint created", 3, UiMessageSeverity::Success);
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
      setMessage("The selected commit has no changed records", 3, UiMessageSeverity::Warning);
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
    const auto visible = filteredHistoryIndices(inventoryCommits_, historySearchQuery_, historySourceFilter_);
    if (!visible.empty()) {
      historySelection_ = visible.front();
      historyRecordSelection_ = 0;
      historyRecordOpen_ = false;
      refreshHistoryDetail();
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::End) {
    const auto visible = filteredHistoryIndices(inventoryCommits_, historySearchQuery_, historySourceFilter_);
    if (!visible.empty()) {
      historySelection_ = visible.back();
      historyRecordSelection_ = 0;
      historyRecordOpen_ = false;
      refreshHistoryDetail();
      dirty_ = true;
    }
    return;
  }
  if (key.type != KeyType::Character) return;

  const auto ch = static_cast<char>(tolower(static_cast<unsigned char>(key.ch)));
  if (ch == '/') {
    startHistorySearch();
  } else if (ch == 'f') {
    cycleHistoryFilter();
  } else if (ch == 'c') {
    beginHistoryCheckpoint();
  } else if (ch == 's') {
    beginHistoryRestore(InventoryRevertMode::Snapshot);
  } else if (ch == 'v') {
    beginHistoryRestore(InventoryRevertMode::Reverse);
  } else if (ch == 'r') {
    refreshInventoryCommits();
    setMessage("Inventory history reloaded", 2, UiMessageSeverity::Success);
    dirty_ = true;
  }
}

void App::moveHistorySelection(int delta) {
  const auto visible = filteredHistoryIndices(inventoryCommits_, historySearchQuery_, historySourceFilter_);
  historyRecordSelection_ = 0;
  historyRecordOpen_ = false;
  if (visible.empty()) {
    historySelection_ = 0;
    historyDetailValid_ = false;
    dirty_ = true;
    return;
  }

  const auto selected = find(visible.begin(), visible.end(), historySelection_);
  const int current = selected == visible.end() ? 0 : static_cast<int>(selected - visible.begin());
  const int next = clamp(current + delta, 0, static_cast<int>(visible.size() - 1));
  historySelection_ = visible[static_cast<size_t>(next)];
  refreshHistoryDetail();
  dirty_ = true;
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
