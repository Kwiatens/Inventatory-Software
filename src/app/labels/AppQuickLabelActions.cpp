// Inventatory - quick-label, wire-label, and stock-filter actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;
using namespace app_actions;

bool App::printWireLabel(const string& text) {
  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("No printer configured", 3);
    openSettings(SettingsCategory::Printer);
    return false;
  }
  auto work = beginPrinterJob(PrinterWorkKind::PrintWire);
  if (!work) return false;
  work->text = text;
  return queuePrinterJob(move(*work));
}

void App::publishConfiguredPrinter() {
  const string name = printerService_.configuredPrinter();
  lock_guard<mutex> lock(quickLabelMutex_);
  configuredPrinterSnapshot_ = name;
}

void App::storeQuickLabelPrintResult(const DeviceQuickLabelPrintResult& result,
                                     const QuickLabelPrintCacheIdentity& identity) {
  if (result.requestId.empty() || identity.workspaceGeneration == 0 ||
      identity.request.requestId != result.requestId) {
    return;
  }
  lock_guard<mutex> lock(quickLabelMutex_);
  const auto known = quickLabelPrintResults_.find(result.requestId);
  const bool wasKnown = known != quickLabelPrintResults_.end();
  if (known != quickLabelPrintResults_.end()) {
    // A late completion from an older operation must not overwrite a newer
    // operation that reused the same request id.
    if (!quickLabelPrintCacheIdentityMatches(known->second.identity, identity)) return;
  }
  quickLabelPrintResults_[result.requestId] = {identity, result};
  if (!wasKnown &&
      find(quickLabelPrintOrder_.begin(), quickLabelPrintOrder_.end(), result.requestId) ==
          quickLabelPrintOrder_.end()) {
    quickLabelPrintOrder_.push_back(result.requestId);
  }
  while (quickLabelPrintOrder_.size() > 64) {
    const auto evict = find_if(quickLabelPrintOrder_.begin(), quickLabelPrintOrder_.end(), [&](const string& id) {
      const auto entry = quickLabelPrintResults_.find(id);
      return entry == quickLabelPrintResults_.end() || entry->second.result.status != "pending";
    });
    if (evict == quickLabelPrintOrder_.end()) break;
    quickLabelPrintResults_.erase(*evict);
    quickLabelPrintOrder_.erase(evict);
  }
}

void App::clearQuickLabelPrintCache() {
  lock_guard<mutex> lock(quickLabelMutex_);
  quickLabelPrintResults_.clear();
  quickLabelPrintOrder_.clear();
}

bool App::printDeviceQuickLabel(const DeviceQuickLabelPrintRequest& request, const string& deviceId,
                                WorkspaceGeneration workspaceGeneration,
                                DeviceQuickLabelPrintResult& result) {
  result.requestId = request.requestId;
  string text;
  string printerName;
  QuickLabelPrintCacheIdentity identity;
  identity.request = request;
  identity.deviceId = deviceId;
  identity.workspaceGeneration = workspaceGeneration;
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    const auto known = quickLabelPrintResults_.find(request.requestId);
    if (request.presetIndex >= 1 && request.presetIndex <= static_cast<int>(settings_.quickLabelPresets.size())) {
      identity.labelText = settings_.quickLabelPresets[static_cast<size_t>(request.presetIndex - 1)];
    }
    if (known != quickLabelPrintResults_.end()) {
      if (quickLabelPrintCacheIdentityMatches(known->second.identity, identity)) {
        result = known->second.result;
        return result.status == "completed";
      }
      // This request id is being reused for a different operation.  Remove
      // the old entry while retaining its order slot for bounded eviction.
      quickLabelPrintResults_.erase(known);
    }
    if (request.revision != settings_.quickLabelRevision) {
      result = {request.requestId, "failed", "stale_presets", "Refresh quick labels"};
    } else if (request.presetIndex < 1 || request.presetIndex > static_cast<int>(settings_.quickLabelPresets.size())) {
      result = {request.requestId, "failed", "missing_preset", "Quick label not found"};
    } else {
      text = settings_.quickLabelPresets[static_cast<size_t>(request.presetIndex - 1)];
    }
    // printer.conf belongs to the active workspace. The app-settings copy can still describe the
    // previously selected workspace during a switch. This runs on a worker thread, so it reads the
    // snapshot the UI thread publishes rather than the UI-owned printer service.
    printerName = configuredPrinterSnapshot_;
  }

  if (!result.status.empty()) {
    storeQuickLabelPrintResult(result, identity);
    return false;
  }

  if (trim(printerName).empty()) {
    result = {request.requestId, "failed", "printer_unconfigured", "No printer configured"};
  } else {
    const auto context = currentWorkspaceContext();
    if (context == nullptr) {
      result = {request.requestId, "failed", "workspace_unavailable", "Workspace is changing"};
    } else {
      result = {request.requestId, "pending", "queued", "Label queued; poll with the same requestId"};
      PrinterWork work;
      work.kind = PrinterWorkKind::PrintWire;
      work.workspaceGeneration = context->generation;
      work.printerName = move(printerName);
      work.text = move(text);
      work.requestId = request.requestId;
      work.quickLabelIdentity = identity;
      if (!enqueuePrinterWork(move(work))) {
        result = {request.requestId, "failed", "printer_queue_full", "Printer queue is full"};
      }
    }
  }

  storeQuickLabelPrintResult(result, identity);
  return result.status == "completed";
}

void App::addQuickLabelPreset() {
  if (settingsDraft_.quickLabelPresets.size() >= kQuickLabelPresetLimit) {
    setMessage("A maximum of 12 quick labels is supported", 3);
    return;
  }
  settingsDraft_.quickLabelPresets.push_back("New label");
  settingsField_ = static_cast<int>(settingsDraft_.quickLabelPresets.size() - 1);
  settingsDirty_ = true;
  beginSettingsFieldEdit(settingsField_);
}

void App::deleteQuickLabelPreset() {
  if (settingsField_ < 0 || settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) return;
  settingsDraft_.quickLabelPresets.erase(settingsDraft_.quickLabelPresets.begin() + settingsField_);
  if (settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) --settingsField_;
  settingsDirty_ = true;
  dirty_ = true;
}

void App::openStockFilterPanel() {
  if (closestSearchActive_) return;
  if (inputMode_ == InputMode::StockFilter) {
    inputMode_ = InputMode::None;
    focusedTargetId_.clear();
    dirty_ = true;
    return;
  }
  inputMode_ = InputMode::StockFilter;
  stockFilterRow_ = 0;
  stockFilterOption_ = stockSortOrderIndex(stockSortOrder_);
  focusedTargetId_.clear();
  dirty_ = true;
}

void App::focusStockFilterOption(int row, int option) {
  stockFilterRow_ = std::clamp(row, 0, kStockFilterRowCount - 1);
  stockFilterOption_ = clampStockFilterOption(stockFilterRow_, option);
  dirty_ = true;
}

// Filters apply at once and the panel stays open, so several choices can be
// made in one visit. The page updates behind the panel.
void App::applyStockDateFilter(StockDateFilter filter) {
  stockDateFilter_ = filter;
  syncSelectionToFilter();
  dirty_ = true;
}

void App::applyStockSortOrder(StockSortOrder order) {
  stockSortOrder_ = order;
  syncSelectionToFilter();
  dirty_ = true;
}

void App::resetStockFilters() {
  resetStockFilterState(stockDateFilter_, stockSortOrder_);
  syncSelectionToFilter();
  setMessage("Stock filters reset", 2);
  dirty_ = true;
}

void App::activateStockFilterOption(int row, int option) {
  switch (row) {
    case 0:
      applyStockSortOrder(stockSortOrderAt(clampStockFilterOption(row, option)));
      break;
    case 1:
      applyStockDateFilter(stockDateFilterAt(clampStockFilterOption(row, option)));
      break;
    default:
      resetStockFilters();
      break;
  }
}

void App::handleStockFilterKey(const KeyEvent& key) {
  const bool up = key.type == KeyType::Up || (key.type == KeyType::Character && key.ch == 'k');
  const bool down = key.type == KeyType::Down || (key.type == KeyType::Character && key.ch == 'j');
  const bool left = key.type == KeyType::Left || (key.type == KeyType::Character && key.ch == 'h');
  const bool right = key.type == KeyType::Right || (key.type == KeyType::Character && key.ch == 'l');
  if (up) {
    focusStockFilterOption(stockFilterRow_ - 1, stockFilterOption_);
  } else if (down) {
    focusStockFilterOption(stockFilterRow_ + 1, stockFilterOption_);
  } else if (left) {
    focusStockFilterOption(stockFilterRow_, stockFilterOption_ - 1);
  } else if (right) {
    focusStockFilterOption(stockFilterRow_, stockFilterOption_ + 1);
  } else if (key.type == KeyType::Enter) {
    activateStockFilterOption(stockFilterRow_, stockFilterOption_);
  } else if (key.type == KeyType::Escape || (key.type == KeyType::Character && key.ch == 'f')) {
    inputMode_ = InputMode::None;
    focusedTargetId_.clear();
    dirty_ = true;
  }
}

void App::moveQuickLabelPreset(int direction) {
  const int destination = settingsField_ + direction;
  if (settingsField_ < 0 || destination < 0 || destination >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) return;
  swap(settingsDraft_.quickLabelPresets[settingsField_], settingsDraft_.quickLabelPresets[destination]);
  settingsField_ = destination;
  settingsDirty_ = true;
  dirty_ = true;
}

void App::testQuickLabelPreset() {
  if (settingsField_ < 0 || settingsField_ >= static_cast<int>(settingsDraft_.quickLabelPresets.size())) {
    setMessage("Select a quick label first", 3);
    return;
  }
  if (settingsDraft_.printerQueue.empty()) {
    setMessage("No printer configured", 4);
    return;
  }
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Printer unavailable while the workspace is changing", 4);
    return;
  }
  if (printerWorkCompletion_ != nullptr) {
    setMessage("A printer job is already running; try again shortly", 3);
    return;
  }
  PrinterWork work;
  work.kind = PrinterWorkKind::PrintWire;
  work.workspaceGeneration = context->generation;
  work.printerName = settingsDraft_.printerQueue;
  work.text = settingsDraft_.quickLabelPresets[settingsField_];
  work.successPrefix = "Quick label sent";
  if (enqueuePrinterWork(move(work))) setMessage("Printer job queued", 3);
  else setMessage("Printer request queue is full; try again shortly", 4);
}

}  // namespace inventatory
