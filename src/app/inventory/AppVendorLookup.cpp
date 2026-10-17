// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Vendor lookup for a part added by hand: asks each configured vendor for the part number, then lets the user choose the
// vendor the part came from. The chosen vendor becomes the part's origin, which is where its refreshes look.

#include "App.h"
#include "app/common/AppActionSupport.h"
#include "core/inventory/VendorBagCode.h"
#include "core/inventory/VendorProvider.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/mouser/MouserApi.h"
#include "platform/tme/TmeApi.h"

#include <future>

namespace inventatory {

using namespace std;

// Asks every configured vendor for the part number typed into the form's SKU. The lookups run in the background, and the
// answers reach the form through processVendorLookup.
void App::beginVendorLookup() {
  if (!originProvider(workingCopy_.item).empty()) {
    setMessage("This part already has a vendor; it keeps the vendor it came from", 4, UiMessageSeverity::Warning);
    return;
  }
  if (vendorLookupFuture_.valid()) {
    setMessage("Vendor lookup is already running", 3);
    return;
  }
  const auto query = trim(workingCopy_.item.sku).empty() ? trim(workingCopy_.item.vendorMetadata.manufacturerPartNumber)
                                                          : trim(workingCopy_.item.sku);
  if (query.empty()) {
    setMessage("Enter the part number in the SKU field first", 4, UiMessageSeverity::Warning);
    return;
  }
  const auto digiKeyConfig = loadDigiKeyConfig();
  const auto tmeConfig = loadTmeConfig();
  const auto mouserConfig = loadMouserConfig();
  if (!digiKeyConfig.valid() && !tmeConfig.valid() && !mouserConfig.valid()) {
    setMessage("No vendor is set up: add DigiKey, TME or Mouser credentials in Settings", 5,
               UiMessageSeverity::Warning);
    return;
  }
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("Vendor lookup unavailable while the workspace is changing", 5);
    return;
  }
  const auto manufacturer = trim(workingCopy_.item.manufacturer) == "Unknown" ? string() : trim(workingCopy_.item.manufacturer);

  vendorLookupFuture_ = async(launch::async, [query, manufacturer, digiKeyConfig, tmeConfig, mouserConfig,
                                              generation = context->generation] {
    App::VendorLookupBatch batch;
    batch.query = query;
    batch.workspaceGeneration = generation;

    if (digiKeyConfig.valid()) {
      DigiKeyApiClient client(digiKeyConfig);
      DigiKeyLookupOutcome outcome = DigiKeyLookupOutcome::Failed;
      string error;
      if (auto details = client.lookupProductDetails(query, outcome, &error)) {
        App::VendorLookupCandidate candidate;
        candidate.provider = "digikey";
        candidate.partNumber = trim(details->vendorMetadata.providerProductNumber.empty()
                                        ? details->lookupKey
                                        : details->vendorMetadata.providerProductNumber);
        candidate.manufacturer = trim(details->manufacturerName);
        candidate.packageName = trim(details->packageName);
        candidate.description = trim(details->productDescription);
        candidate.stock = trim(details->quantityAvailable);
        candidate.price = trim(details->unitPrice);
        candidate.parameterCount = details->parameters.size();
        candidate.hasDatasheet = !trim(details->datasheetUrl).empty();
        candidate.digiKey = *details;
        batch.candidates.push_back(move(candidate));
      } else if (outcome == DigiKeyLookupOutcome::NoMatch) {
        batch.notes.push_back("DigiKey lists no product for this part number.");
      } else {
        batch.notes.push_back("DigiKey did not answer: " + (error.empty() ? string("request failed") : error));
      }
    } else {
      batch.notes.push_back("DigiKey is not set up.");
    }

    if (tmeConfig.valid()) {
      TmeApiClient client(tmeConfig);
      TmeLookupOutcome outcome = TmeLookupOutcome::Failed;
      string error;
      if (auto details = client.lookupByPartNumber(query, outcome, &error, manufacturer)) {
        App::VendorLookupCandidate candidate;
        candidate.provider = "tme";
        candidate.partNumber = trim(details->vendorMetadata.providerProductNumber);
        candidate.manufacturer = trim(details->manufacturerName);
        candidate.packageName = trim(details->packageName);
        candidate.description = trim(details->productDescription);
        candidate.stock = trim(details->quantityAvailable);
        candidate.price = trim(details->unitPrice);
        candidate.parameterCount = details->parameters.size();
        candidate.hasDatasheet = !trim(details->datasheetUrl).empty();
        candidate.tme = *details;
        batch.candidates.push_back(move(candidate));
      } else if (outcome == TmeLookupOutcome::Ambiguous) {
        batch.notes.push_back("TME lists more than one product for this part number.");
      } else if (outcome == TmeLookupOutcome::NoMatch) {
        batch.notes.push_back("TME lists no product for this part number.");
      } else {
        batch.notes.push_back("TME did not answer: " + (error.empty() ? string("request failed") : error));
      }
    } else {
      batch.notes.push_back("TME is not set up.");
    }

    if (mouserConfig.valid()) {
      MouserLookupOutcome outcome = MouserLookupOutcome::Failed;
      string error;
      optional<MouserProductDetails> details;
      try {
        MouserApiClient client(mouserConfig);
        details = client.lookupByPartNumber(query, outcome, &error, manufacturer);
      } catch (...) {
        // The key travels in the request URL, so no exception text is shown.
        outcome = MouserLookupOutcome::Failed;
        error = "unexpected error";
      }
      if (details) {
        App::VendorLookupCandidate candidate;
        candidate.provider = kVendorProviderMouser;
        candidate.partNumber = trim(details->vendorMetadata.providerProductNumber);
        candidate.manufacturer = trim(details->manufacturerName);
        candidate.packageName = trim(details->packageName);
        candidate.description = trim(details->productDescription);
        candidate.stock = trim(details->quantityAvailable);
        candidate.price = trim(details->unitPrice);
        candidate.parameterCount = details->parameters.size();
        candidate.hasDatasheet = !trim(details->datasheetUrl).empty();
        candidate.mouser = *details;
        batch.candidates.push_back(move(candidate));
      } else if (outcome == MouserLookupOutcome::Ambiguous) {
        batch.notes.push_back("Mouser lists more than one product for this part number.");
      } else if (outcome == MouserLookupOutcome::NoMatch) {
        batch.notes.push_back("Mouser lists no product for this part number.");
      } else {
        batch.notes.push_back("Mouser did not answer: " + (error.empty() ? string("request failed") : error));
      }
    } else {
      batch.notes.push_back("Mouser is not set up.");
    }
    return batch;
  });

  setMessage("Looking up " + query + " at the vendors that are set up", 6);
  dirty_ = true;
}

// Shows the answers once they arrive. A result for a form that was closed, or for another workspace, changes nothing.
void App::processVendorLookup() {
  if (!vendorLookupFuture_.valid()) return;
  if (vendorLookupFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
  auto batch = vendorLookupFuture_.get();
  vendorLookupFuture_ = {};
  if (!workspaceIsCurrent(batch.workspaceGeneration)) {
    setMessage("Vendor lookup result discarded because the workspace changed", 5, UiMessageSeverity::Warning);
    dirty_ = true;
    return;
  }
  const bool formOpen = page_ == Page::Stock &&
                        (inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue);
  if (!formOpen) {
    setMessage("Vendor lookup finished after the edit was closed; nothing was changed", 4, UiMessageSeverity::Warning);
    dirty_ = true;
    return;
  }

  vendorLookupQuery_ = batch.query;
  vendorLookupNotes_ = batch.notes;
  vendorCandidates_ = move(batch.candidates);
  vendorCandidateIndex_ = 0;
  if (vendorCandidates_.empty()) {
    vendorLookupNotes_.clear();
    string message = "No vendor lists " + batch.query + ". Enter the part's details by hand";
    if (!batch.notes.empty()) message += "; " + batch.notes.front();
    setMessage(message, 6, UiMessageSeverity::Warning);
    dirty_ = true;
    return;
  }
  inputMode_ = InputMode::VendorPick;
  setMessage(to_string(vendorCandidates_.size()) +
                 (vendorCandidates_.size() == 1 ? " vendor lists " : " vendors list ") + batch.query +
                 ": choose the one this part came from",
             6);
  dirty_ = true;
}

// The vendor a part came from is fixed by this choice: its details fill the part, and the vendor is the origin that
// refreshes look up. A part keeps that vendor, so it can be chosen only once.
void App::chooseVendorCandidate(size_t index) {
  if (index >= vendorCandidates_.size()) return;
  if (!originProvider(workingCopy_.item).empty()) {
    setMessage("This part already has a vendor; it keeps the vendor it came from", 4, UiMessageSeverity::Warning);
    return;
  }
  const auto candidate = vendorCandidates_[index];
  auto& item = workingCopy_.item;
  // A part added by hand carries "New Part" until it has a name, so the vendor's description may name it.
  if (trim(item.partName) == "New Part") item.partName.clear();
  if (candidate.provider == "digikey" && candidate.digiKey) {
    app_actions::mergeDigiKeyMetadata(item, *candidate.digiKey);
  } else if (candidate.provider == "tme" && candidate.tme) {
    app_actions::mergeTmeMetadata(item, *candidate.tme);
  } else if (candidate.provider == kVendorProviderMouser && candidate.mouser) {
    app_actions::mergeMouserMetadata(item, *candidate.mouser);
  }
  item.vendorMetadata.provider = candidate.provider;
  if (trim(item.vendorMetadata.providerProductNumber).empty()) {
    item.vendorMetadata.providerProductNumber = candidate.partNumber;
  }

  const string vendorName = vendorDisplayName(candidate.provider);
  vendorCandidates_.clear();
  vendorLookupNotes_.clear();
  vendorCandidateIndex_ = 0;
  inputMode_ = InputMode::EditFieldMenu;
  setMessage("Vendor set to " + vendorName + "; its details are filled in. Press s to save", 5,
             UiMessageSeverity::Success);
  dirty_ = true;
}

void App::handleVendorPickKey(const KeyEvent& key) {
  if (key.type == KeyType::Up) {
    vendorCandidateIndex_ = vendorCandidateIndex_ > 0 ? vendorCandidateIndex_ - 1 : 0;
    dirty_ = true;
  } else if (key.type == KeyType::Down) {
    if (!vendorCandidates_.empty()) {
      vendorCandidateIndex_ = min(vendorCandidateIndex_ + 1, vendorCandidates_.size() - 1);
    }
    dirty_ = true;
  } else if (key.type == KeyType::Enter) {
    chooseVendorCandidate(vendorCandidateIndex_);
  } else if (key.type == KeyType::Escape) {
    vendorCandidates_.clear();
    vendorLookupNotes_.clear();
    vendorCandidateIndex_ = 0;
    inputMode_ = InputMode::EditFieldMenu;
    setMessage("Vendor lookup cancelled", 2);
    dirty_ = true;
  }
}

}  // namespace inventatory
