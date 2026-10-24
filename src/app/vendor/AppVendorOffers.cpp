// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - price checks for one stock item across DigiKey, TME and Mouser.

#include "App.h"

#include "core/inventory/VendorProvider.h"
#include "core/bom/BomProjectStore.h"
#include "core/vendor/VendorOfferStore.h"
#include "core/vendor/VendorPurchase.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/mouser/MouserApi.h"
#include "platform/tme/TmeApi.h"

#include "platform/system/Console.h"

#include <chrono>
#include <exception>
#include <future>
#include <utility>

namespace inventatory {
using namespace std;

namespace {

OfferAnswer unanswered(const string& provider, const string& lookupKey, string error = string()) {
  OfferAnswer answer;
  answer.provider = provider;
  answer.lookupKey = lookupKey;
  answer.error = move(error);
  return answer;
}

OfferAnswer askDigiKey(const string& productNumber) {
  auto answer = unanswered(kVendorProviderDigiKey, productNumber);
  const auto config = loadDigiKeyConfig();
  if (!config.valid()) {
    answer.error = "DigiKey is not set up";
    return answer;
  }
  DigiKeyApiClient client(config);
  DigiKeyLookupOutcome outcome = DigiKeyLookupOutcome::Failed;
  const auto details = client.lookupProductDetails(productNumber, outcome, &answer.error);
  if (outcome == DigiKeyLookupOutcome::Found && details.has_value()) {
    answer.answered = true;
    answer.offers = details->offers;
  } else if (outcome == DigiKeyLookupOutcome::NoMatch) {
    answer.answered = true;
  }
  return answer;
}

OfferAnswer askMouser(const string& partNumber, const string& manufacturer) {
  auto answer = unanswered(kVendorProviderMouser, partNumber);
  const auto config = loadMouserConfig();
  if (!config.valid()) {
    answer.error = "Mouser is not set up";
    return answer;
  }
  MouserApiClient client(config);
  MouserLookupOutcome outcome = MouserLookupOutcome::Failed;
  const auto product = client.lookupByPartNumber(partNumber, outcome, &answer.error, manufacturer);
  if (outcome == MouserLookupOutcome::Found && product.has_value()) {
    answer.answered = true;
    if (product->offer.has_value()) answer.offers.push_back(*product->offer);
  } else if (outcome == MouserLookupOutcome::NoMatch) {
    answer.answered = true;
  } else if (outcome == MouserLookupOutcome::Ambiguous) {
    // Several products share the number: keep what was saved rather than guess.
    answer.error = "Mouser lists several products for " + partNumber;
  }
  return answer;
}

OfferAnswer askTme(const string& symbol) {
  auto answer = unanswered(kVendorProviderTme, symbol);
  const auto config = loadTmeConfig();
  if (!config.valid()) {
    answer.error = "TME is not set up";
    return answer;
  }
  TmeApiClient client(config);
  TmeLookupOutcome outcome = TmeLookupOutcome::Failed;
  const auto product = client.lookupBySymbol(symbol, outcome, &answer.error);
  if (outcome == TmeLookupOutcome::Found && product.has_value()) {
    answer.answered = true;
    if (product->offer.has_value()) answer.offers.push_back(*product->offer);
  } else if (outcome == TmeLookupOutcome::NoMatch) {
    answer.answered = true;
  } else if (outcome == TmeLookupOutcome::Ambiguous) {
    answer.error = "TME lists several products for " + symbol;
  }
  return answer;
}

// The vendors to ask for an item, with the number each knows it by.
vector<pair<string, string>> offerLookups(const InventoryItem& item, const AppSettings& settings) {
  vector<pair<string, string>> lookups;
  if (const auto number = trim(item.digikeyPartNumber); settings.digiKeyEnabled && !number.empty()) lookups.emplace_back(kVendorProviderDigiKey, number);
  if (const auto number = trim(mouserPartNumberOf(item)); settings.mouserEnabled && !number.empty()) lookups.emplace_back(kVendorProviderMouser, number);
  if (const auto symbol = trim(tmeProductSymbol(item)); settings.tmeEnabled && !symbol.empty()) lookups.emplace_back(kVendorProviderTme, symbol);
  return lookups;
}

}  // namespace

void App::beginOfferRefresh() {
  if (offerRefreshFuture_.valid()) {
    setMessage("Already checking prices", 3);
    return;
  }
  const auto* item = selectedItem();
  if (item == nullptr) return;
  const auto lookups = offerLookups(*item, settings_);
  if (lookups.empty()) {
    setMessage("This part has no number at an enabled vendor to check prices with", 4);
    return;
  }
  const auto context = currentWorkspaceContext();
  if (context == nullptr) return;
  // An "Unknown" manufacturer does not help tell Mouser's products apart.
  const auto manufacturer = trim(item->manufacturer) == "Unknown" ? string() : trim(item->manufacturer);
  // The worker reads credentials too: the keyring can block.
  offerRefreshFuture_ = async(launch::async, [itemId = item->id, generation = context->generation, lookups,
                                              manufacturer] {
    OfferRefreshResult result;
    result.itemId = itemId;
    result.workspaceGeneration = generation;
    for (const auto& [provider, number] : lookups) {
      try {
        if (provider == kVendorProviderDigiKey) {
          result.answers.push_back(askDigiKey(number));
        } else if (provider == kVendorProviderMouser) {
          result.answers.push_back(askMouser(number, manufacturer));
        } else {
          result.answers.push_back(askTme(number));
        }
      } catch (...) {
        // Exception text could carry a request URL with a key, so it is not passed on.
        result.answers.push_back(unanswered(provider, number, vendorDisplayName(provider) + " price check failed"));
      }
    }
    return result;
  });
  setMessage("Checking prices for " + item->partName, 3);
  dirty_ = true;
}

// Refreshes every part at its own vendor: DigiKey parts at DigiKey, TME parts at TME, and so on. A vendor
// that is off or not set up is skipped.
void App::beginAllVendorRefresh() {
  vector<string> started;
  const bool digiKeyReady = settings_.digiKeyEnabled && !trim(settings_.digiKeyClientId).empty() && hasStoredDigiKeySecret_;
  const bool tmeReady = settings_.tmeEnabled && hasStoredTmeToken_ && hasStoredTmeSecret_;
  const bool mouserReady = settings_.mouserEnabled && (hasStoredMouserKey_ || mouserKeyFromEnvironment_);
  if (digiKeyReady && !digiKeyRefresh_.running()) {
    beginDigiKeyRefresh();
    if (digiKeyRefresh_.running()) started.push_back("DigiKey");
  }
  if (tmeReady && !tmeRefresh_.running()) {
    beginTmeRefresh();
    if (tmeRefresh_.running()) started.push_back("TME");
  }
  if (mouserReady && !mouserRefresh_.running()) {
    beginMouserRefresh();
    if (mouserRefresh_.running()) started.push_back("Mouser");
  }
  if (started.empty()) {
    setMessage(digiKeyReady || tmeReady || mouserReady ? "No parts with a number at an enabled vendor were found"
                                                       : "Turn on and set up a vendor first",
               4);
  } else {
    string names;
    for (size_t index = 0; index < started.size(); ++index) {
      names += (index == 0 ? "" : index + 1 == started.size() ? " and " : ", ") + started[index];
    }
    setMessage("Refreshing parts at " + names, 4);
  }
  dirty_ = true;
}

void App::processOfferRefresh() {
  if (!offerRefreshFuture_.valid() ||
      offerRefreshFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
    return;
  }
  const auto result = offerRefreshFuture_.get();
  if (!workspaceIsCurrent(result.workspaceGeneration)) return;
  const auto databasePath = vendorOfferDatabasePath(dataPath_);
  vector<string> updated;
  vector<string> withoutOffer;
  string failure;
  for (const auto& answer : result.answers) {
    const auto name = vendorDisplayName(answer.provider);
    string saveError;
    if (!answer.answered) {
      // Most errors already start with the vendor's name ("DigiKey is not set up").
      const auto error = answer.error.empty() ? name + " did not answer" : answer.error;
      if (failure.empty()) failure = error.rfind(name, 0) == 0 ? error : name + ": " + error;
    } else if (!saveVendorOffers(databasePath, answer.provider, answer.lookupKey, answer.offers, &saveError)) {
      if (failure.empty()) failure = name + ": " + saveError;
    } else if (answer.offers.empty()) {
      withoutOffer.push_back(name);
    } else {
      updated.push_back(name);
    }
  }
  offerCacheValid_ = false;
  const auto joined = [](const vector<string>& names) {
    string text;
    for (size_t index = 0; index < names.size(); ++index) {
      if (index > 0) text += index + 1 == names.size() ? " and " : ", ";
      text += names[index];
    }
    return text;
  };
  if (!failure.empty()) {
    setMessage((updated.empty() ? "Prices not checked. " : "Prices updated from " + joined(updated) + ". ") + failure,
               6, UiMessageSeverity::Warning);
  } else if (updated.empty()) {
    setMessage(joined(withoutOffer) + " had no price for this part", 4);
  } else {
    setMessage("Prices updated from " + joined(updated) +
                   (withoutOffer.empty() ? string() : "; " + joined(withoutOffer) + " had none"),
               4, UiMessageSeverity::Success);
  }
  dirty_ = true;
}

void App::saveItemOffers(const InventoryItem& item, const string& provider, const vector<VendorOffer>& offers) {
  for (const auto& [lookupProvider, number] : offerLookups(item, settings_)) {
    if (lookupProvider != provider) continue;
    string error;
    // The prices came with a lookup that already matched the item, so they are saved under its own number.
    if (saveVendorOffers(vendorOfferDatabasePath(dataPath_), provider, number, offers, &error)) offerCacheValid_ = false;
    return;
  }
}

vector<VendorOffer> App::offersForBomLine(const BomLine& line, const BomMatch& match) const {
  vector<VendorOffer> offers;
  const auto databasePath = vendorOfferDatabasePath(dataPath_);
  const auto add = [&](const string& provider, const string& number) {
    vector<VendorOffer> loaded;
    if (loadVendorOffers(databasePath, provider, number, loaded)) offers.insert(offers.end(), loaded.begin(), loaded.end());
  };
  // A stock part only counts once the line is confirmed to use it.
  if (match.confirmed) {
    if (const auto* item = store_.findById(match.chosenItemId())) {
      for (const auto& [provider, number] : offerLookups(*item, settings_)) add(provider, number);
    }
  }
  if (const auto* project = activeBomProject()) {
    const auto found = project->enrichment.find(bomLineKey(line));
    if (found != project->enrichment.end()) {
      const auto provider = bomEnrichmentProvider(found->second);
      const bool enabled = provider == kVendorProviderDigiKey ? settings_.digiKeyEnabled
                           : provider == kVendorProviderTme   ? settings_.tmeEnabled
                                                              : settings_.mouserEnabled;
      if (!provider.empty() && enabled) add(provider, bomEnrichmentPart(found->second));
    }
  }
  return offers;
}

optional<PurchaseChoice> App::bestOfferForBomLine(const BomLine& line, const BomMatch& match, uint64_t toBuy) const {
  const auto* project = activeBomProject();
  const auto preferred = project == nullptr ? string() : bomPreferredProvider(project->enrichment);
  // An offer that cannot be bought now (out of stock, or priced out of proportion) is not a choice at all.
  vector<VendorOffer> buyable;
  for (const auto& offer : offersForBomLine(line, match)) {
    if (orderableOffer(offer, toBuy).has_value()) buyable.push_back(offer);
  }
  return chooseBestOffer(buyable, toBuy, sharedVendorCurrency(settings_), time(nullptr), preferred);
}

bool App::providerEnabled(const AppSettings& settings, const string& provider) {
  return provider == kVendorProviderDigiKey ? settings.digiKeyEnabled
         : provider == kVendorProviderTme   ? settings.tmeEnabled
         : provider == kVendorProviderMouser ? settings.mouserEnabled
                                             : false;
}

vector<string> App::bomVendorChoices() const {
  vector<string> choices;
  for (const auto* provider : {kVendorProviderDigiKey, kVendorProviderTme, kVendorProviderMouser}) {
    if (providerEnabled(settings_, provider)) choices.push_back(provider);
  }
  return choices;
}

const vector<VendorOffer>& App::selectedItemOffers() const {
  static const vector<VendorOffer> kNone;
  const auto* item = selectedItem();
  if (item == nullptr) return kNone;
  if (offerCacheValid_ && offerCacheItemId_ == item->id) return offerCache_;
  offerCache_.clear();
  offerCacheItemId_ = item->id;
  offerCacheValid_ = true;
  const auto databasePath = vendorOfferDatabasePath(dataPath_);
  for (const auto& [provider, number] : offerLookups(*item, settings_)) {
    vector<VendorOffer> offers;
    // An unreadable price file shows no prices; the next check reports the problem.
    if (loadVendorOffers(databasePath, provider, number, offers)) {
      offerCache_.insert(offerCache_.end(), offers.begin(), offers.end());
    }
  }
  return offerCache_;
}

}  // namespace inventatory
