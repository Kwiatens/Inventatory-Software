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
  // Print the slot's own part; the Stock selection is a sorted position and must not be involved.
  const auto itemId = item->id;
  ensureInventoryIdentifiers(store_.items());
  const auto* printable = store_.findById(itemId);
  if (printable == nullptr) {
    setMessage("Selected part is no longer available", 2);
    return false;
  }
  return printLabelForItem(*printable, "Printed label for ", true);
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
