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

namespace {

string printerDetailText(const PrinterQueueInfo* printer, const PrinterCheckResult& check) {
  if (printer == nullptr) {
    return "No printer selected.";
  }

  ostringstream out;
  out << "Name: " << printer->name << '\n';
  out << "Driver: " << (trim(printer->driverName).empty() ? string("-") : printer->driverName) << '\n';
  out << "Port: " << (trim(printer->portName).empty() ? string("-") : printer->portName) << '\n';
  out << "Status: " << printer->statusText << '\n';
  out << "Ready: " << (printer->isReady ? "yes" : "no") << '\n';
  out << "Probe: " << (check.message.empty() ? string("not run") : check.message) << '\n';
  return out.str();
}

}  // namespace

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

ftxui::Element App::renderPrinterSetupUi() const {
  const auto* selectedPrinter = selectedPrinterQueue();

  ftxui::Elements leftRows;
  leftRows.push_back(fullLine("Detected printers", uiAccentColor(), uiPanelLeftBg()));
  if (printerQueues_.empty()) {
    leftRows.push_back(fullLine("No printers found.", uiWarnColor(), uiPanelLeftBg()));
  } else {
    for (size_t index = 0; index < printerQueues_.size(); ++index) {
      const auto& printer = printerQueues_[index];
      const auto bg = index == printerSelection_ ? uiRowSelectedBg()
                                                 : (index % 2 == 0 ? uiRowDarkBg() : uiRowLightBg());
      leftRows.push_back(fullLine((printer.isDefault ? "[*] " : "[ ] ") + printer.name + "  " + printer.portName + "  " +
                                      printer.statusText,
                                  index == printerSelection_ ? uiTitleColor()
                                                             : (printer.isReady ? uiSuccessColor() : uiWarnColor()),
                                  bg));
    }
  }

  ftxui::Elements rightRows;
  rightRows.push_back(fullLine("Printer setup", uiAccentColor(), uiPanelRightBg()));
  rightRows.push_back(ftxui::paragraphAlignLeft("Use arrows to pick the Zebra queue, Enter or t to test, and s to save.")
                         | ftxui::color(uiMutedColor()));
  rightRows.push_back(uiDivider());
  rightRows.push_back(ftxui::paragraphAlignLeft(printerDetailText(selectedPrinter, printerCheck_)) |
                      ftxui::color(uiTitleColor()));
  rightRows.push_back(uiDivider());
  rightRows.push_back(fullLine("Configured queue", uiAccentColor(), uiPanelRightBg()));
  rightRows.push_back(ftxui::paragraphAlignLeft(printerService_.hasConfiguredPrinter()
                                                    ? printerService_.configuredPrinter()
                                                    : string("Not configured")) |
                      ftxui::color(uiLinkColor()));
  rightRows.push_back(uiDivider());
  rightRows.push_back(fullLine("Connection check", uiAccentColor(), uiPanelRightBg()));
  rightRows.push_back(ftxui::paragraphAlignLeft(printerCheck_.message.empty() ? string("No probe run yet")
                                                                             : printerCheck_.message) |
                      ftxui::color(printerCheck_.ok ? uiSuccessColor() : uiWarnColor()));

  return ftxui::hbox({
      ftxui::vbox(move(leftRows)) | ftxui::bgcolor(uiPanelLeftBg()) | ftxui::flex,
      uiDivider(),
      ftxui::vbox(move(rightRows)) | ftxui::bgcolor(uiPanelRightBg()) | ftxui::flex,
  });
}

void App::handlePrinterSetupKey(const KeyEvent& key) {
  if (printerQueues_.empty()) {
    if (key.type == KeyType::Character) {
      const auto ch = tolower(static_cast<unsigned char>(key.ch));
      if (ch == 'r') {
        refreshPrinterState();
        setMessage("Printer list refreshed", 2);
      } else if (ch == 'q') {
        running_ = false;
      }
    } else if (key.type == KeyType::Escape) {
      changePage(Page::Home);
    }
    return;
  }

  if (key.type == KeyType::Character) {
    const auto ch = tolower(static_cast<unsigned char>(key.ch));
    switch (ch) {
      case 'r':
        refreshPrinterState();
        setMessage("Printer list refreshed", 2);
        return;
      case 'q':
        running_ = false;
        return;
      case 't':
        if (const auto* printer = selectedPrinterQueue()) {
          printerService_.setConfiguredPrinter(printer->name);
          printerCheck_ = printerService_.probeConfiguredPrinter();
          setMessage(printerCheck_.message.empty() ? "Printer checked" : printerCheck_.message, 3);
          dirty_ = true;
        }
        return;
      case 's':
        if (const auto* printer = selectedPrinterQueue()) {
          printerService_.setConfiguredPrinter(printer->name);
          printerCheck_ = printerService_.probeConfiguredPrinter();
          saveState();
          changePage(Page::Home);
          setMessage(printerCheck_.ok ? "Printer saved and ready" : "Printer saved, but not ready", 3);
        }
        return;
      default:
        break;
    }
  }

  if (key.type == KeyType::Up) {
    if (printerSelection_ > 0) {
      --printerSelection_;
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Down) {
    if (printerSelection_ + 1 < printerQueues_.size()) {
      ++printerSelection_;
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    if (const auto* printer = selectedPrinterQueue()) {
      printerService_.setConfiguredPrinter(printer->name);
      printerCheck_ = printerService_.probeConfiguredPrinter();
      if (printerCheck_.ok) {
        saveState();
        changePage(Page::Home);
        setMessage("Printer configured and ready", 3);
      } else {
        setMessage(printerCheck_.message.empty() ? "Printer probe failed" : printerCheck_.message, 4);
      }
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Escape) {
    changePage(Page::Home);
  }
}

}  // namespace hims
