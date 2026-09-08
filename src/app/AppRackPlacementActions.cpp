// Inventatory - Rack placement and movement actions.

// Inventatory - rack navigation, editing, movement, and printing actions.

#include "App.h"
#include "app/AppActionSupport.h"

#include "core/InventorySqlite.h"
#include "platform/DigiKeyApi.h"
#include "platform/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::moveRackSlot(int rowDelta, int columnDelta) {
  rackRow_ = clamp(rackRow_ + rowDelta, 0, 4);
  rackColumn_ = clamp(rackColumn_ + columnDelta, 0, 4);
  dirty_ = true;
}

void App::moveRackPage(int delta) {
  const auto indices = sortedRackIndices();
  if (indices.empty()) {
    rackSelection_ = 0;
    return;
  }
  const auto current = static_cast<int>(min(rackSelection_, indices.size() - 1));
  rackSelection_ = static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(indices.size() - 1)));
  movingRackItemId_.clear();
  movingRackSource_.clear();
  dirty_ = true;
}

void App::beginOrCompleteRackMove() {
  auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return;
  }

  const auto slot = selectedRackSlot();
  auto* item = itemAtRackSlot(store_, rack->id, slot);
  if (movingRackItemId_.empty()) {
    if (item == nullptr) {
      setMessage("Select an occupied slot to move", 2);
      return;
    }
    movingRackItemId_ = item->id;
    movingRackSource_ = rack->code + "-" + slot;
    setMessage("Moving " + item->partName + "; choose an empty slot and press v", 4);
    dirty_ = true;
    return;
  }

  auto* movingItem = store_.findById(movingRackItemId_);
  if (movingItem == nullptr) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
    setMessage("Moving item no longer exists", 3);
    return;
  }
  if (rackLocation(*movingItem, store_.racks()) == rack->code + "-" + slot) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
    setMessage("Move cancelled", 2);
    dirty_ = true;
    return;
  }
  if (item != nullptr) {
    setMessage(rack->code + "-" + slot + " is already occupied", 3);
    return;
  }

  string error;
  captureUndoSnapshot();
  if (!moveItemToRackSlot(store_, *movingItem, *rack, slot, error)) {
    undoSnapshot_.valid = false;
    setMessage(error, 4);
    return;
  }
  movingItem->lastUpdated = time(nullptr);
  const auto target = rack->code + "-" + slot;
  logActivity("rack", movingItem->partName + " moved " + movingRackSource_ + " -> " + target);
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage("Moved to " + target, 2);
  dirty_ = true;
}

void App::unassignSelectedRackItem() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }
  const auto previous = rackLocation(*item, store_.racks());
  captureUndoSnapshot();
  unassignItemFromRack(*item);
  item->lastUpdated = time(nullptr);
  logActivity("rack", item->partName + " unassigned from " + previous);
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage("Rack location intentionally unassigned", 2);
}

void App::autoAssignSelectedRackItem() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return;
  }
  const auto previous = rackLocation(*item, store_.racks());
  captureUndoSnapshot();
  restoreAutomaticRackAssignment(store_, *item);
  item->lastUpdated = time(nullptr);
  const auto next = rackLocation(*item, store_.racks());
  logActivity("rack", item->partName + " AUTO " + previous + " -> " + (next.empty() ? "unassigned" : next));
  movingRackItemId_.clear();
  movingRackSource_.clear();
  saveState();
  setMessage(next.empty() ? "AUTO found no eligible rack placement" : "AUTO assigned " + next, 3);
}

}  // namespace inventatory
