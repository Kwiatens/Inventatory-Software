// Inventatory - Rack printer actions.

// Inventatory - rack navigation, editing, movement, and printing actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <system_error>

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

  auto work = beginPrinterJob(PrinterWorkKind::PrintRack);
  if (!work) return false;
  work->rack = *rack;
  work->symbolStandard = settings_.symbolStandard;
  return queuePrinterJob(move(*work));
}

}  // namespace inventatory
