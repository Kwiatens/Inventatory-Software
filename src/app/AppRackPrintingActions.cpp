// Inventatory - Rack printer actions.

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

bool App::printSelectedRackPartLabel() {
  auto* item = selectedRackItem();
  if (item == nullptr) {
    setMessage("No part in this slot", 2);
    return false;
  }
  const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& candidate) {
    return candidate.id == item->id;
  });
  if (it != store_.items().end()) {
    selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
  }
  return printSelectedLabel();
}

bool App::printSelectedRackLabel() {
  const auto* rack = selectedRack();
  if (rack == nullptr) {
    setMessage("No rack selected", 2);
    return false;
  }

  if (!printerService_.hasConfiguredPrinter()) {
    setMessage("No printer configured", 3);
    openPrinterSetup();
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
  work.kind = PrinterWorkKind::PrintRack;
  work.workspaceGeneration = context->generation;
  work.printerName = printerService_.configuredPrinter();
  work.rack = *rack;
  if (!enqueuePrinterWork(move(work))) {
    setMessage("Printer request queue is full; try again shortly", 4);
    return false;
  }
  setMessage("Printer job queued", 3);
  return true;
}

}  // namespace inventatory
