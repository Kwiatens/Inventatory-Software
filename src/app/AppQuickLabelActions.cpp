// Inventatory - quick-label, wire-label, and stock-filter actions.

#include "App.h"
#include "app/AppActionSupport.h"

#include "import/CsvFormat.h"
#include "core/InventorySqlite.h"
#include "platform/DigiKeyApi.h"
#include "platform/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <future>
#include <limits>
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
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Printer unavailable while the workspace is changing", 4);
    return false;
  }
  if (printerWorkCompletion_ != nullptr) {
    setMessage("A printer job is already running; try again shortly", 3);
    return false;
  }
  PrinterWork work;
  work.kind = PrinterWorkKind::PrintWire;
  work.workspaceGeneration = context->generation;
  work.printerName = printerService_.configuredPrinter();
  work.text = text;
  if (!enqueuePrinterWork(move(work))) {
    setMessage("Printer request queue is full; try again shortly", 4);
    return false;
  }
  setMessage("Printer job queued", 3);
  return true;
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
  }

  if (!result.status.empty()) {
    storeQuickLabelPrintResult(result, identity);
    return false;
  }

  // printer.conf belongs to the active workspace. The app-settings copy can
  // still describe the previously selected workspace during a switch.
  string printerName = printerService_.configuredPrinter();
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
  inputMode_ = InputMode::StockFilter;
  stockDateFilterSubmenuOpen_ = false;
  stockFilterSelection_ = stockDateFilter_ != StockDateFilter::All ? 0
                          : stockSortOrder_ == StockSortOrder::Quantity ? 1
                          : stockSortOrder_ == StockSortOrder::Za ? 3 : 2;
  focusedTarget_ = -1;
  dirty_ = true;
}

void App::openStockDateFilterSubmenu() {
  stockDateFilterSubmenuOpen_ = true;
  stockFilterSelection_ = static_cast<int>(stockDateFilter_);
  dirty_ = true;
}

void App::applyStockDateFilter(StockDateFilter filter) {
  stockDateFilter_ = filter;
  stockFilterSelection_ = static_cast<int>(filter);
  stockDateFilterSubmenuOpen_ = false;
  inputMode_ = InputMode::None;
  syncSelectionToFilter();
  setMessage("Stock filter: " + stockDateFilterName(filter), 3);
  dirty_ = true;
}

void App::applyStockSortOrder(StockSortOrder order) {
  stockSortOrder_ = order;
  stockDateFilterSubmenuOpen_ = false;
  inputMode_ = InputMode::None;
  syncSelectionToFilter();
  const auto message = order == StockSortOrder::Az ? "Stock sorted A-Z"
                       : order == StockSortOrder::Za ? "Stock sorted Z-A"
                                                     : "Stock sorted by quantity";
  setMessage(message, 3);
  dirty_ = true;
}

void App::handleStockFilterKey(const KeyEvent& key) {
  const int optionCount = stockDateFilterSubmenuOpen_ ? 5 : 4;
  if (key.type == KeyType::Up || (key.type == KeyType::Character && key.ch == 'k')) {
    stockFilterSelection_ = max(0, stockFilterSelection_ - 1);
  } else if (key.type == KeyType::Down || (key.type == KeyType::Character && key.ch == 'j')) {
    stockFilterSelection_ = min(optionCount - 1, stockFilterSelection_ + 1);
  } else if (key.type == KeyType::Enter) {
    if (stockDateFilterSubmenuOpen_) {
      applyStockDateFilter(static_cast<StockDateFilter>(stockFilterSelection_));
    } else if (stockFilterSelection_ == 0) {
      openStockDateFilterSubmenu();
    } else if (stockFilterSelection_ == 1) {
      applyStockSortOrder(StockSortOrder::Quantity);
    } else if (stockFilterSelection_ == 2) {
      applyStockSortOrder(StockSortOrder::Az);
    } else {
      applyStockSortOrder(StockSortOrder::Za);
    }
    return;
  } else if (key.type == KeyType::Escape || (key.type == KeyType::Character && key.ch == 'f')) {
    if (stockDateFilterSubmenuOpen_) {
      stockDateFilterSubmenuOpen_ = false;
      stockFilterSelection_ = 0;
    } else {
      inputMode_ = InputMode::None;
    }
  } else if (key.type == KeyType::Left && stockDateFilterSubmenuOpen_) {
    stockDateFilterSubmenuOpen_ = false;
    stockFilterSelection_ = 0;
  } else if (key.type == KeyType::Character && key.ch >= '1' && key.ch <= '4' && !stockDateFilterSubmenuOpen_) {
    const auto choice = key.ch - '1';
    if (choice == 0) {
      openStockDateFilterSubmenu();
    } else if (choice == 1) {
      applyStockSortOrder(StockSortOrder::Quantity);
    } else if (choice == 2) {
      applyStockSortOrder(StockSortOrder::Az);
    } else {
      applyStockSortOrder(StockSortOrder::Za);
    }
    return;
  } else {
    return;
  }
  dirty_ = true;
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
