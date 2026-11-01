// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - shared item-detail and prompt helpers.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/inventory/VendorProvider.h"
#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <sstream>
#include <system_error>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::openCurrentUrl(const string& url, const string& label) {
  if (trim(url).empty()) {
    setMessage("No " + label + " link stored for this item", 3);
    return;
  }
  if (openUrl(url)) {
    setMessage("Opened " + label + " link", 2);
  } else {
    setMessage("Unable to open " + label + " link", 3);
  }
}

string App::softwareVersion() const {
#ifdef Inventatory_VERSION_STRING
  return Inventatory_VERSION_STRING;
#else
  return "dev";
#endif
}

string App::activePrompt() const {
  if (inputMode_ == InputMode::EditValue && fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
    return menuOptions_[fieldMenuIndex_].label + ": ";
  }
  if (inputMode_ == InputMode::RackRename) return "Rename rack to: ";
  if (inputMode_ == InputMode::RackType) return "Rack type: ";
  if (inputMode_ == InputMode::RackCreate) return "New rack type: ";
  if (inputMode_ == InputMode::OrganizerCreate) return "New organizer, name and size: ";
  if (inputMode_ == InputMode::OrganizerType) return "What goes in " + storagePendingName_ + "? Category, optional: ";
  if (inputMode_ == InputMode::RackJump) return "Go to: ";
  if (inputMode_ == InputMode::RackFilter) return "Rack filter: ";
  if (inputMode_ == InputMode::QuantityAdjust) return "Quantity on hand: ";
  if (inputMode_ == InputMode::StocktakeCount) return "Physical count: ";
  if (inputMode_ == InputMode::BomReceive) return "Received pieces: ";
  if (inputMode_ == InputMode::BomPartSearch) return "Manual search " + vendorDisplayName(bomPicker_.store) + ": ";
  if (inputMode_ == InputMode::HistoryCheckpoint) return "Checkpoint name: ";
  if (inputMode_ == InputMode::ExitConfirmation) return "S save   D discard   Esc cancel";
  return "";
}

}  // namespace inventatory
