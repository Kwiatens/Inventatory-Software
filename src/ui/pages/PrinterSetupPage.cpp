// HIMS - Hardware Inventory Management System
// Printer setup wizard rendering and keyboard handling.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace hims {

using namespace std;

void App::refreshPrinterState() {
  printerQueues_ = printerService_.enumeratePrinters();
  const auto configured = printerService_.configuredPrinterInfo();
  if (configured) {
    const auto configuredName = toLower(trim(configured->name));
    const auto it = find_if(printerQueues_.begin(), printerQueues_.end(), [&](const PrinterQueueInfo& entry) {
      return toLower(trim(entry.name)) == configuredName;
    });
    if (it != printerQueues_.end()) {
      printerSelection_ = static_cast<size_t>(distance(printerQueues_.begin(), it));
    }
  }

  if (printerSelection_ >= printerQueues_.size()) {
    printerSelection_ = 0;
  }

  printerCheck_ = printerService_.probeConfiguredPrinter();
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

  string error;
  if (!printerService_.printItemLabel(item, &error, rackLocation(item, store_.racks()))) {
    setMessage("Print failed: " + error, 4);
    refreshPrinterState();
    return false;
  }

  logActivity("print", item.partName + " label printed");
  saveState();
  refreshPrinterState();
  setMessage(successPrefix + item.partName, 2);
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

}  // namespace hims
