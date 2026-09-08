// Inventatory - Workspace and background-work lifecycle.

#include "App.h"

#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/StartupRegistration.h"
#include "platform/system/UpdateService.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <thread>
#include <future>

namespace inventatory {

using namespace std;

namespace {
constexpr size_t kPrinterWorkQueueLimit = 32;
}  // namespace

shared_ptr<const App::WorkspaceContext> App::currentWorkspaceContext() const {
  lock_guard<mutex> lock(workspaceMutex_);
  return workspaceContext_;
}

void App::activateWorkspaceContext(const InventatoryDataPaths& paths) {
  {
    lock_guard<mutex> lock(workspaceMutex_);
    const auto previousGeneration = workspaceContext_ == nullptr ? 0 : workspaceContext_->generation;
    auto context = make_shared<WorkspaceContext>();
    context->paths = paths;
    context->generation = advanceWorkspaceGeneration(previousGeneration);
    workspaceContext_ = move(context);
    dataPath_ = paths.dataDirectory;
    inventoryPath_ = paths.inventory;
    printerPath_ = paths.printer;
    activityPath_ = paths.activity;
    inventatoryScanConfigPath_ = paths.scanConfig;
    quickLabelsPath_ = paths.dataDirectory / "quick_labels.conf";
  }
  // Cached idempotency results belong to the old context even if their
  // request id happens to be reused in the newly selected workspace.
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
    deviceRequestCache_.clear();
    deviceRequestOrder_.clear();
  }
  clearQuickLabelPrintCache();
}

bool App::workspaceIsCurrent(WorkspaceGeneration generation) const {
  lock_guard<mutex> lock(workspaceMutex_);
  return workspaceContext_ != nullptr && workspaceGenerationMatches(workspaceContext_->generation, generation);
}

bool App::enqueuePrinterWork(PrinterWork work) {
  if (work.workspaceGeneration == 0 || !workspaceIsCurrent(work.workspaceGeneration)) {
    return false;
  }
  {
    lock_guard<mutex> lock(printerWorkMutex_);
    if (printerWorkQueue_.size() >= kPrinterWorkQueueLimit) {
      return false;
    }
    printerWorkQueue_.push_back(move(work));
  }
  return true;
}

bool App::enqueuePrinterProbe(const string& printerName) {
  const auto context = currentWorkspaceContext();
  if (context == nullptr || trim(printerName).empty()) return false;
  PrinterWork work;
  work.kind = PrinterWorkKind::Probe;
  work.workspaceGeneration = context->generation;
  work.printerName = printerName;
  return enqueuePrinterWork(move(work));
}

void App::processPrinterWork() {
  optional<PrinterWorkResult> completedResult;
  if (printerWorkCompletion_ != nullptr) {
    const auto completion = printerWorkCompletion_;
    lock_guard<mutex> lock(completion->completionMutex);
    if (completion->result.has_value()) {
      completedResult = move(completion->result);
      completion->result.reset();
    } else if (completion->cancelled) {
      printerWorkCompletion_.reset();
      printerWorkActiveKind_.reset();
    } else {
      return;
    }
  }
  if (completedResult.has_value()) {
    const auto result = move(*completedResult);
    printerWorkCompletion_.reset();
    printerWorkActiveKind_.reset();

    if (workspaceIsCurrent(result.work.workspaceGeneration)) {
      switch (result.work.kind) {
        case PrinterWorkKind::Refresh:
          printerQueues_ = move(result.queues);
          printerCheck_ = result.check;
          if (!result.work.printerName.empty()) {
            const auto configuredName = toLower(trim(result.work.printerName));
            const auto it = find_if(printerQueues_.begin(), printerQueues_.end(),
                                    [&](const PrinterQueueInfo& entry) {
                                      return toLower(trim(entry.name)) == configuredName;
                                    });
            if (it != printerQueues_.end()) {
              printerSelection_ = static_cast<size_t>(distance(printerQueues_.begin(), it));
            }
          }
          if (printerSelection_ >= printerQueues_.size()) printerSelection_ = 0;
          if (!result.success && printerCheck_.message.empty()) {
            printerCheck_.message = result.error.empty() ? "Printer check failed" : result.error;
          }
          setMessage(result.success
                         ? (printerCheck_.ok ? "Printer check complete" : "Printer needs attention")
                         : (printerCheck_.message.empty() ? "Printer check failed" : printerCheck_.message),
                     4);
          dirty_ = true;
          break;
        case PrinterWorkKind::Probe:
          printerCheck_ = result.check;
          setMessage(result.check.message.empty()
                         ? (result.success ? "Printer is ready" : "Printer test failed")
                         : result.check.message,
                     4);
          dirty_ = true;
          break;
        case PrinterWorkKind::PrintItem:
          if (result.success) {
            printerFlashUntil_ = time(nullptr) + 3;
            logActivity("print", result.work.item.partName + " label printed");
            const bool saved = saveState();
            setMessage(saved ? result.work.successPrefix + result.work.item.partName
                             : result.work.successPrefix + result.work.item.partName +
                                   "; activity not saved, press R to retry",
                       saved ? 3 : 6);
          } else {
            setMessage("Print failed: " +
                           (result.error.empty() ? string("Printer failed") : result.error),
                       4);
          }
          dirty_ = true;
          break;
        case PrinterWorkKind::PrintWire:
          if (result.success) {
            printerFlashUntil_ = time(nullptr) + 3;
            logActivity("print", "wire label printed");
            const bool saved = saveActivitiesChecked(false);
            if (result.work.quickLabelIdentity.has_value()) {
              storeQuickLabelPrintResult({result.work.requestId, "completed", "", "Label sent"},
                                         *result.work.quickLabelIdentity);
              setMessage(saved ? "Quick label sent" :
                                   "Quick label sent; activity not saved, press R to retry",
                         saved ? 3 : 6);
            } else {
              const auto message = result.work.successPrefix.empty() ? string("Wire label sent")
                                                                      : result.work.successPrefix;
              setMessage(saved ? message : message + "; activity not saved, press R to retry",
                         saved ? 3 : 6);
            }
          } else {
            const auto error = result.error.empty() ? string("Printer failed") : result.error;
            if (result.work.quickLabelIdentity.has_value()) {
              storeQuickLabelPrintResult({result.work.requestId, "failed", "printer_failed", error},
                                         *result.work.quickLabelIdentity);
              setMessage("Quick label failed: " + error, 4);
            } else {
              setMessage(error, 4);
            }
          }
          dirty_ = true;
          break;
        case PrinterWorkKind::PrintRack:
          if (result.success) {
            printerFlashUntil_ = time(nullptr) + 3;
            logActivity("print", result.work.rack.code + " rack label printed");
            const bool saved = saveState();
            const auto message = result.work.rack.code + " rack label sent";
            setMessage(saved ? message : message + "; activity not saved, press R to retry",
                       saved ? 3 : 6);
          } else {
            setMessage("Print failed: " +
                           (result.error.empty() ? string("Printer failed") : result.error),
                       4);
          }
          dirty_ = true;
          break;
      }
    }
  }

  while (true) {
    PrinterWork work;
    {
      lock_guard<mutex> lock(printerWorkMutex_);
      if (printerWorkQueue_.empty()) return;
      work = move(printerWorkQueue_.front());
      printerWorkQueue_.pop_front();
    }
    if (!workspaceIsCurrent(work.workspaceGeneration)) {
      continue;
    }

    const auto workKind = work.kind;
    auto completion = make_shared<PrinterWorkCompletion>();
    PrinterWork workerWork = work;
    try {
      thread([completion, work = move(workerWork)]() mutable {
      PrinterWorkResult result;
      result.work = work;
      try {
        // Constructing a private service also constructs a private Windows
        // spooler backend.  No worker ever touches the UI-owned service or
        // borrows App state, configured strings, or inventory references.
        LabelPrinterService printer;
        printer.setConfiguredPrinter(work.printerName);
        switch (work.kind) {
          case PrinterWorkKind::Refresh:
            result.queues = printer.enumeratePrinters();
            result.check = printer.probeConfiguredPrinter();
            result.success = true;
            break;
          case PrinterWorkKind::Probe:
            result.check = printer.probeConfiguredPrinter();
            result.success = result.check.ok;
            result.error = result.check.message;
            break;
          case PrinterWorkKind::PrintItem:
            result.success = printer.printItemLabel(work.item, &result.error, work.rackLocation);
            break;
          case PrinterWorkKind::PrintWire:
            result.success = printer.printWireLabel(work.text, &result.error);
            break;
          case PrinterWorkKind::PrintRack:
            result.success = printer.printRackLabel(work.rack, &result.error);
            break;
        }
      } catch (...) {
        result.success = false;
        result.error = "Printer operation failed unexpectedly";
      }
      lock_guard<mutex> lock(completion->completionMutex);
      if (!completion->cancelled) completion->result = move(result);
      }).detach();
    } catch (...) {
      // Preserve the work identity when thread creation itself fails.  This
      // keeps a pending quick-label request from being stranded forever.
      PrinterWorkResult result;
      result.work = move(work);
      result.success = false;
      result.error = "Printer worker could not be started";
      lock_guard<mutex> lock(completion->completionMutex);
      completion->result = move(result);
    }
    printerWorkCompletion_ = move(completion);
    printerWorkActiveKind_ = workKind;
    return;
  }
}

void App::stopPrinterWork() {
  {
    lock_guard<mutex> lock(printerWorkMutex_);
    printerWorkQueue_.clear();
  }
  if (printerWorkCompletion_ != nullptr) {
    const auto completion = printerWorkCompletion_;
    lock_guard<mutex> lock(completion->completionMutex);
    completion->cancelled = true;
    completion->result.reset();
    printerWorkCompletion_.reset();
  }
  printerWorkActiveKind_.reset();
  {
    lock_guard<mutex> lock(quickLabelMutex_);
    for (auto& entry : quickLabelPrintResults_) {
      if (entry.second.result.status == "pending") {
        entry.second.result.status = "failed";
        entry.second.result.code = "workspace_changed";
        entry.second.result.message = "Printing was cancelled while the workspace changed";
      }
    }
  }
}

void App::stopWorkspaceBoundWork() {
  stopPrinterWork();
  stopDigiKeyRefresh();

  if (importSyncCancelFlag_ != nullptr) {
    importSyncCancelFlag_->store(true);
  }
  if (importSyncFuture_.valid()) {
    importSyncFuture_.wait();
    importSyncFuture_ = {};
  }
  importSyncCancelFlag_.reset();
  importSyncRunning_ = false;
  importSyncCancelRequested_ = false;
  importSyncGeneration_ = 0;

  if (scanDigiKeyEnrichmentFuture_.valid()) {
    scanDigiKeyEnrichmentFuture_.wait();
    scanDigiKeyEnrichmentFuture_ = {};
  }
  scanDigiKeyEnrichmentQueue_.clear();

  if (bomEnrichmentFuture_.valid()) {
    bomEnrichmentFuture_.wait();
    bomEnrichmentFuture_ = {};
  }
  bomEnrichmentQueue_.clear();
  bomEnrichmentActiveKey_.clear();
  bomEnrichmentProjectId_.clear();
  bomEnrichmentActiveProjectId_.clear();
  bomEnrichmentGeneration_ = 0;
  bomEnrichmentSequence_ = 0;
  bomEnrichmentClient_.reset();

  {
    lock_guard<mutex> lock(scanMutex_);
    scanQueue_.clear();
  }
  {
    lock_guard<mutex> lock(deviceQueueMutex_);
    for (const auto& pending : deviceQuantityQueue_) {
      lock_guard<mutex> pendingLock(pending->mutex);
      pending->cancelled = true;
      pending->result = {};
      pending->result.httpStatus = 503;
      pending->result.error = "Inventatory workspace is changing";
      pending->complete = true;
      pending->ready.notify_one();
    }
    deviceQuantityQueue_.clear();
    deviceStatusQueue_.clear();
    deviceDebugQueue_.clear();
  }
}

void App::requestUserExit() {
  if (importSyncRunning_) {
    setMessage("DigiKey sync is still running; cancel it or wait for completion", 4);
    return;
  }
  if (settingsDirty_) {
    pendingPageAfterSettings_.reset();
    exitSavePending_ = false;
    inputMode_ = InputMode::ExitConfirmation;
    setMessage("Unsaved settings: press S to save, D to discard, or Esc to stay", 5);
    return;
  }
  if (hasPendingPersistence()) {
    pendingPageAfterSettings_.reset();
    exitSavePending_ = true;
    inputMode_ = InputMode::ExitConfirmation;
    setMessage("Unsaved data could not be persisted: press S to retry, D to exit, or Esc to stay", 6);
    return;
  }
  if (backgroundController_.enabled()) {
    backgroundController_.hideConsole(true);
  } else {
    running_ = false;
  }
}

void App::restartDeviceService() {
  mdnsService_.stop();
  server_.stop();
  if (!inventatoryScanConfig_.setupComplete || inventatoryScanConfig_.token.empty() || scannerCredentialSavePending_) {
    setMessage(!inventatoryScanConfig_.setupComplete
                   ? "Scan R1 service is disabled until this workspace is paired"
                   : "Scan R1 service is disabled until its pairing token is stored securely",
               6);
    return;
  }
  server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                               inventatoryScanReplayStatePath(dataPath_));
  if (!server_.start(settings_.deviceServicePort,
                     [this](const DeviceSyncRequest& request, DeviceSyncResponse& response, string& error) {
                       return handleDeviceSync(request, response, error);
                     })) {
    setMessage("Inventatory Scan R1 service failed to restart; terminal still works", 6);
    return;
  }
  if (mdnsService_.start(server_.port())) {
    setMessage("Inventatory Scan R1 bridge restarted on port " + to_string(server_.port()), 5);
  } else {
    setMessage("Inventatory Scan R1 bridge restarted; network discovery unavailable", 5);
  }
  dirty_ = true;
}

void App::completeSettingsExit(bool saveChanges) {
  if (saveChanges) {
    if (!saveSettingsDraft()) return;
  } else {
    cancelSettingsDraft();
  }
  const auto destination = pendingPageAfterSettings_;
  pendingPageAfterSettings_.reset();
  inputMode_ = InputMode::None;
  if (destination.has_value()) {
    changePage(*destination);
  } else {
    requestUserExit();
  }
}

}  // namespace inventatory
