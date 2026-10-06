// Inventatory - Inventory history persistence workflows.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "core/storage/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::refreshInventoryMovements() {
  inventoryMovements_ = loadInventoryMovements(inventoryPath_);
}

void App::refreshInventoryCommits(bool afterOwnSave) {
  vector<InventoryCommit> loadedCommits;
  if (!loadInventoryCommits(inventoryPath_, loadedCommits, !afterOwnSave)) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not reload inventory history: " + inventoryPath_.u8string();
    persistenceError_ = inventoryRecoveryDetail_ + ". The previous history was preserved.";
    return;
  }
  if (afterOwnSave && !loadedCommits.empty()) {
    // The history was validated when the workspace loaded and every commit since was derived from its
    // stored parent in the same transaction, so checking the newest commit (its summary, snapshot and
    // relation to its parent) is what a full pass would add.
    InventoryCommitDetail newest;
    if (!loadInventoryCommit(inventoryPath_, loadedCommits.front().id, newest)) {
      inventoryRecoveryRequired_ = true;
      inventoryRecoveryDetail_ = "Inventatory could not reload inventory history: " + inventoryPath_.u8string();
      persistenceError_ = inventoryRecoveryDetail_ + ". The previous history was preserved.";
      return;
    }
  }
  InventoryCommitDetail loadedDetail;
  size_t loadedSelection = 0;
  if (!loadedCommits.empty()) {
    loadedSelection = min(historySelection_, loadedCommits.size() - 1);
    if (!loadInventoryCommit(inventoryPath_, loadedCommits[loadedSelection].id, loadedDetail)) {
      inventoryRecoveryRequired_ = true;
      inventoryRecoveryDetail_ = "Inventatory could not reload inventory history details: " + inventoryPath_.u8string();
      persistenceError_ = inventoryRecoveryDetail_ + ". The previous history was preserved.";
      return;
    }
  }
  inventoryCommits_ = move(loadedCommits);
  historyRecordSelection_ = 0;
  historyRecordOpen_ = false;
  historySelection_ = loadedSelection;
  if (inventoryCommits_.empty()) {
    historySelection_ = 0;
    historyDetailValid_ = false;
    historyDetail_ = {};
    return;
  }
  historyDetail_ = move(loadedDetail);
  historyDetailValid_ = true;
  syncHistorySelectionToFilter();
}

void App::refreshHistoryDetail() {
  if (inventoryCommits_.empty()) {
    historyDetail_ = {};
    historyDetailValid_ = false;
    historySelection_ = 0;
    return;
  }
  historySelection_ = min(historySelection_, inventoryCommits_.size() - 1);
  InventoryCommitDetail loadedDetail;
  if (!loadInventoryCommit(inventoryPath_, inventoryCommits_[historySelection_].id, loadedDetail)) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not reload inventory history details: " + inventoryPath_.u8string();
    persistenceError_ = inventoryRecoveryDetail_ + ". The previous history was preserved.";
    return;
  }
  historyDetail_ = move(loadedDetail);
  historyDetailValid_ = true;
}

void App::beginHistoryCheckpoint() {
  inputBuffer_.clear();
  inputMode_ = InputMode::HistoryCheckpoint;
  setMessage("Enter a non-empty checkpoint name, then press Enter", 4);
}

void App::beginHistoryRestore(InventoryRevertMode mode) {
  if (!historyDetailValid_) refreshHistoryDetail();
  if (!historyDetailValid_) {
    setMessage("The selected inventory commit could not be loaded", 4);
    return;
  }
  if (persistedStoreValid_ && !inventoryCommitDiff(persistedStore_, store_).empty()) {
    setMessage("Save the current inventory before using History", 4);
    return;
  }
  if (mode == InventoryRevertMode::Reverse && (!historyDetail_.hasParent || historyDetail_.changes.empty())) {
    setMessage("The selected commit has no reversible inventory changes", 4);
    return;
  }
  if (mode == InventoryRevertMode::Reverse) {
    InventoryStore preview;
    string conflict;
    if (!prepareInventoryCommitReverse(historyDetail_, store_, preview, conflict)) {
      setMessage(conflict.empty() ? "Reverse blocked because later changes conflict" : conflict, 6);
      return;
    }
  }
  pendingHistoryRevertMode_ = mode;
  inputBuffer_.clear();
  inputMode_ = InputMode::HistoryConfirm;
  dirty_ = true;
}

void App::cancelHistoryAction() {
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  dirty_ = true;
}

bool App::applyHistoryRevert(InventoryRevertMode mode) {
  if (!historyDetailValid_) refreshHistoryDetail();
  if (!historyDetailValid_) {
    setMessage("The selected inventory commit could not be loaded", 4);
    cancelHistoryAction();
    return false;
  }
  if (persistedStoreValid_ && !inventoryCommitDiff(persistedStore_, store_).empty()) {
    setMessage("Save the current inventory before using History", 4);
    cancelHistoryAction();
    return false;
  }

  InventoryStore target;
  if (mode == InventoryRevertMode::Snapshot) {
    target = historyDetail_.snapshot;
  } else {
    string conflict;
    if (!prepareInventoryCommitReverse(historyDetail_, store_, target, conflict)) {
      setMessage(conflict.empty() ? "Reverse blocked because later changes conflict" : conflict, 6);
      cancelHistoryAction();
      return false;
    }
  }

  if (inventoryCommitDiff(store_, target).empty()) {
    setMessage("The selected operation would not change inventory", 3);
    cancelHistoryAction();
    return false;
  }

  store_ = move(target);
  InventoryCommitDraft draft;
  draft.source = "revert";
  draft.reference = historyDetail_.commit.id;
  draft.corrective = true;
  draft.revertedCommitId = historyDetail_.commit.id;
  draft.message = mode == InventoryRevertMode::Snapshot
                      ? "Restored snapshot from commit #" + to_string(historyDetail_.commit.sequence)
                      : "Reversed changes from commit #" + to_string(historyDetail_.commit.sequence);
  const bool saved = saveInventoryState(draft);
  cancelHistoryAction();
  syncSelectionToFilter();
  syncRackSelection();
  if (saved) setMessage(mode == InventoryRevertMode::Snapshot ? "Snapshot restored" : "Changes reversed", 4);
  return saved;
}

}  // namespace inventatory
