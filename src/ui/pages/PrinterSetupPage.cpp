// Inventatory - Hardware Inventory Management System
// Printer setup wizard rendering and keyboard handling.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

void App::refreshPrinterState() {
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;

  // Enumeration and probing can enter the Windows spooler and take seconds.
  // Capture only the queue name and let the UI-tick worker publish a result.
  {
    lock_guard<mutex> lock(printerWorkMutex_);
    for (const auto& queued : printerWorkQueue_) {
      if (queued.kind == PrinterWorkKind::Refresh &&
          queued.workspaceGeneration == context->generation) {
        return;
      }
    }
  }
  if (printerWorkActiveKind_.has_value() && *printerWorkActiveKind_ == PrinterWorkKind::Refresh) {
    return;
  }
  printerCheck_ = {false, "Checking printer queues..."};
  PrinterWork work;
  work.kind = PrinterWorkKind::Refresh;
  work.workspaceGeneration = context->generation;
  work.printerName = printerService_.configuredPrinter();
  if (!enqueuePrinterWork(move(work))) return;
  setMessage("Checking printer queues...", 3);
  dirty_ = true;
}

void App::openPrinterSetup() {
  openSettings(SettingsCategory::Printer);
  setMessage("Select the Zebra queue, then test and save it", 3);
}

PrinterQueueInfo* App::selectedPrinterQueue() {
  if (printerQueues_.empty()) {
    return nullptr;
  }
  printerSelection_ = min(printerSelection_, printerQueues_.size() - 1);
  return &printerQueues_[printerSelection_];
}

const PrinterQueueInfo* App::selectedPrinterQueue() const {
  if (printerQueues_.empty()) {
    return nullptr;
  }
  const auto index = min(printerSelection_, printerQueues_.size() - 1);
  return &printerQueues_[index];
}

bool App::printSelectedLabel() {
  auto* item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return false;
  }

  ensureInventoryIdentifiers(store_.items());
  item = selectedItem();
  if (item == nullptr) {
    setMessage("No item selected", 2);
    return false;
  }

  return printLabelForItem(*item, "Printed label for ", true);
}

bool App::printLabelForItem(const InventoryItem& item, const string& successPrefix, bool openSetupOnMissingPrinter) {
  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("No printer configured", 3);
    if (openSetupOnMissingPrinter) {
      openPrinterSetup();
    }
    return false;
  }

  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Printer unavailable while the workspace is changing", 4);
    return false;
  }
  if (printerWorkFuture_.valid()) {
    setMessage("A printer job is already running; try again shortly", 3);
    return false;
  }

  PrinterWork work;
  work.kind = PrinterWorkKind::PrintItem;
  work.workspaceGeneration = context->generation;
  work.printerName = printerService_.configuredPrinter();
  work.item = item;
  work.rackLocation = rackLocation(item, store_.racks());
  work.successPrefix = successPrefix;
  if (!enqueuePrinterWork(move(work))) return false;
  setMessage("Printer job queued", 3);
  return true;
}

string App::printerSummary() const {
  if (!printerService_.hasConfiguredPrinter()) {
    return "Printer: not configured";
  }

  if (printerCheck_.ok) {
    return "Printer ready";
  }

  if (!printerCheck_.message.empty()) {
    return "Printer needs attention: " + printerCheck_.message;
  }

  return "Printer connected";
}

}  // namespace inventatory
