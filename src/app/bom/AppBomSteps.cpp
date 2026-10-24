// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - the project page's steps: confirm stock matches, choose parts to buy, order them and
// receive them. Find in racks never waits for these; they only get the parts a full build still lacks.

#include "App.h"

#include "core/inventory/InventoryInternals.h"
#include "core/inventory/VendorProvider.h"
#include "core/storage/AtomicFile.h"
#include "core/transfer/CsvExport.h"
#include "core/vendor/VendorOfferStore.h"
#include "core/vendor/VendorPurchase.h"
#include "platform/system/Console.h"
#include "ui/shared/NumericPrompt.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kLastStep = 3;

string lineName(const BomLine& line) { return trim(line.designation + "  " + packageFromFootprint(line.footprint)); }

bool namedStore(const string& value) {
  return value == kVendorProviderDigiKey || value == kVendorProviderTme || value == kVendorProviderMouser;
}

// "TME", "DigiKey and TME".
string storeList(const set<string>& providers) {
  vector<string> names;
  for (const auto& provider : providers) names.push_back(vendorDisplayName(provider));
  string text;
  for (size_t index = 0; index < names.size(); ++index) {
    text += (index == 0 ? "" : index + 1 == names.size() ? " and " : ", ") + names[index];
  }
  return text;
}

string joinedDesignators(const BomLine& line) {
  string text;
  for (const auto& designator : line.designators) text += (text.empty() ? "" : " ") + designator;
  return text;
}

// The same order as chooseBestOffer: an offer the maker still makes comes before an obsolete one, then the
// cheaper total wins.
bool orderBeats(const VendorOffer& offer, const OfferCost& cost, const VendorOffer& current,
                const OfferCost& currentCost) {
  if (offerIsObsolete(offer) != offerIsObsolete(current)) return !offerIsObsolete(offer);
  return cost.totalMicros < currentCost.totalMicros;
}

}  // namespace

size_t App::bomMatchIndex(const string& lineKey) const {
  if (!bomAnalysisValid_) return string::npos;
  for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
    if (bomLineKey(bomAnalysis_.lines[bomAnalysis_.matches[index].lineIndex]) == lineKey) return index;
  }
  return string::npos;
}

void App::refreshBomProgress() {
  bomProgress_ = {};
  bomPurchases_.clear();
  bomOrderCost_.clear();
  bomOrderStores_.clear();
  const auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_) return;
  bomProgress_ = bomProjectProgress(bomAnalysis_, project->enrichment, store_);

  const auto databasePath = vendorOfferDatabasePath(dataPath_);
  map<string, int64_t> totals;
  set<string> stores;
  for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
    const auto& match = bomAnalysis_.matches[index];
    const auto key = bomLineKey(bomAnalysis_.lines[match.lineIndex]);
    const auto state = bomLineState(match, key, project->enrichment);
    if (state == BomLineState::InStock || state == BomLineState::ToConfirm) continue;
    BomPurchase purchase;
    purchase.matchIndex = index;
    purchase.lineKey = key;
    purchase.state = state;
    purchase.toBuy = bomQuantityToBuy(match);
    if (state == BomLineState::Ordered) {
      purchase.order = bomLineOrder(project->enrichment, key);
      purchase.provider = purchase.order->provider;
      purchase.part = purchase.order->part;
    } else if (state == BomLineState::Chosen) {
      const auto& stored = project->enrichment.at(key);
      purchase.provider = bomEnrichmentProvider(stored);
      purchase.part = bomEnrichmentPart(stored);
    }
    if (!purchase.provider.empty() && !purchase.part.empty()) {
      vector<VendorOffer> offers;
      loadVendorOffers(databasePath, purchase.provider, purchase.part, offers);
      // The cheapest order that can be bought now, by the same rule the part was chosen with.
      for (const auto& offer : offers) {
        const auto cost = orderableOffer(offer, max<uint64_t>(1, purchase.toBuy));
        if (!cost.has_value()) continue;
        if (!purchase.cost.has_value() || orderBeats(offer, *cost, *purchase.offer, *purchase.cost)) {
          purchase.offer = offer;
          purchase.cost = cost;
        }
      }
      // An ordered part keeps its store number and page even once the store has sold out.
      if (!purchase.offer.has_value() && !offers.empty()) purchase.offer = offers.front();
    }
    if (state == BomLineState::Chosen) {
      stores.insert(purchase.provider);
      if (purchase.cost.has_value()) totals[purchase.offer->currency] += purchase.cost->totalMicros;
    }
    bomPurchases_.push_back(move(purchase));
  }
  for (const auto& [currency, total] : totals) {
    bomOrderCost_ += (bomOrderCost_.empty() ? "" : " + ") + formatMicros(total) + " " + currency;
  }
  bomOrderStores_ = storeList(stores);

  // Where each ordered part will go, received in list order into a copy of stock so slots never collide.
  if (none_of(bomPurchases_.begin(), bomPurchases_.end(),
              [](const BomPurchase& purchase) { return purchase.state == BomLineState::Ordered; })) {
    return;
  }
  InventoryStore preview = store_;
  auto overrides = project->overrides;
  for (auto& purchase : bomPurchases_) {
    if (purchase.state != BomLineState::Ordered) continue;
    string itemId;
    const auto quantity = static_cast<int>(min<uint64_t>(purchase.order->quantity, numeric_limits<int>::max()));
    const auto place = applyBomReceipt(preview, overrides, purchase, quantity, &itemId);
    const auto* existing = store_.findById(itemId);
    const auto* received = preview.findById(itemId);
    purchase.stockPart = existing != nullptr ? "adds to " + existing->partName
                                             : "new: " + (received == nullptr ? string() : received->partName);
    purchase.goesTo = place.empty() ? string("No location") : place;
  }
}

void App::applyBomChoicesFromOtherProjects() {
  auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_) return;
  // A part the user chose for the same line in another project is the choice here too.
  bool reused = false;
  for (const auto& match : bomAnalysis_.matches) {
    if (match.sufficient) continue;
    const auto key = bomLineKey(bomAnalysis_.lines[match.lineIndex]);
    const auto current = project->enrichment.find(key);
    if (current != project->enrichment.end() && bomEnrichmentChosen(current->second)) continue;
    if (const auto elsewhere = bomChosenElsewhere(bomProjects_, project->id, key); !elsewhere.empty()) {
      project->enrichment[key] = elsewhere;
      reused = true;
    }
  }
  if (reused) saveBomProjects();
  refreshBomProgress();
}

void App::openBomOverview() {
  closeBomPartPicker();
  bomOrderConfirm_ = false;
  bomView_ = BomView::Overview;
  dirty_ = true;
}

void App::openBomStep(size_t step, const string& atLineKey) {
  auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_) return;
  step = min(step, kLastStep);
  bomOverviewSelection_ = step;
  bomStepKeys_.clear();
  bomStepPosition_ = 0;
  bomStepDone_ = 0;
  bomStepSkipped_ = 0;
  const auto positionAt = [&](const string& key) {
    const auto found = find(bomStepKeys_.begin(), bomStepKeys_.end(), key);
    if (found != bomStepKeys_.end()) bomStepPosition_ = static_cast<size_t>(distance(bomStepKeys_.begin(), found));
  };

  if (step == 0) {
    for (const auto& match : bomAnalysis_.matches) {
      if (match.needsConfirmation()) bomStepKeys_.push_back(bomLineKey(bomAnalysis_.lines[match.lineIndex]));
    }
    if (bomStepKeys_.empty()) {
      setMessage("Nothing to confirm", 2);
      return;
    }
    positionAt(atLineKey);
    bomView_ = BomView::Confirm;
    showBomStepLine();
    return;
  }

  if (step == 1) {
    string firstToChoose;
    for (const auto& purchase : bomPurchases_) {
      if (purchase.state != BomLineState::ToChoose && purchase.state != BomLineState::Chosen) continue;
      bomStepKeys_.push_back(purchase.lineKey);
      if (firstToChoose.empty() && purchase.state == BomLineState::ToChoose) firstToChoose = purchase.lineKey;
    }
    if (bomStepKeys_.empty()) {
      setMessage("Nothing to buy: every part is in stock or ordered", 3);
      return;
    }
    positionAt(atLineKey.empty() ? firstToChoose : atLineKey);
    // The store this project buys from is kept once the user starts choosing.
    if (!namedStore(bomPreferredVendor(project->enrichment))) {
      project->enrichment[kBomPreferredVendorKey] = bomProjectStore();
      saveBomProjects();
    }
    bomView_ = BomView::Choose;
    showBomStepLine();
    return;
  }

  const auto state = step == 2 ? BomLineState::Chosen : BomLineState::Ordered;
  const auto rows = bomPurchasesIn(state);
  if (rows.empty()) {
    setMessage(step == 2 ? "Nothing chosen to order. Choose parts to buy first." : "Nothing ordered yet", 3);
    return;
  }
  size_t selection = 0;
  for (size_t index = 0; index < rows.size(); ++index) {
    if (rows[index]->lineKey == atLineKey) selection = index;
  }
  if (step == 2) {
    bomOrderSelection_ = selection;
    bomOrderConfirm_ = false;
    bomView_ = BomView::Order;
  } else {
    bomReceiveSelection_ = selection;
    bomView_ = BomView::Receive;
  }
  dirty_ = true;
}

void App::openBomLine(size_t matchIndex) {
  const auto* project = activeBomProject();
  if (project == nullptr || matchIndex >= bomAnalysis_.matches.size()) return;
  const auto& match = bomAnalysis_.matches[matchIndex];
  const auto& line = bomAnalysis_.lines[match.lineIndex];
  const auto key = bomLineKey(line);
  switch (bomLineState(match, key, project->enrichment)) {
    case BomLineState::InStock: {
      const auto* item = store_.findById(match.chosenItemId());
      const auto place = item == nullptr ? string() : bomItemPlace(*item, store_.racks());
      setMessage(lineName(line) + " is in stock" + (place.empty() ? string(", with no location") : ": " + place), 3);
      return;
    }
    case BomLineState::ToConfirm: openBomStep(0, key); return;
    case BomLineState::ToChoose:
    case BomLineState::Chosen: openBomStep(1, key); return;
    case BomLineState::Ordered: openBomStep(3, key); return;
  }
}

void App::openFirstOpenBomStep() {
  const auto& progress = bomProgress_;
  if (progress.toConfirm > 0) return openBomStep(0);
  if (progress.toChoose > 0) return openBomStep(1);
  if (progress.chosen > 0) return openBomStep(2);
  if (progress.ordered > 0) return openBomStep(3);
  openBomOverview();
}

void App::requestBomFindInRacks() {
  if (!bomAnalysisValid_) return;
  refreshBomAnalysis();
  // Starting with gaps is allowed, but never without saying what is left out and what can get it.
  if (bomProgress_.notCovered() > 0 && bomView_ != BomView::StartNotice) {
    closeBomPartPicker();
    bomView_ = BomView::StartNotice;
    dirty_ = true;
    return;
  }
  beginBomBuild();
  if (bomView_ != BomView::Build) openBomOverview();
}

vector<const App::BomPurchase*> App::bomPurchasesIn(BomLineState state) const {
  vector<const BomPurchase*> rows;
  for (const auto& purchase : bomPurchases_) {
    if (purchase.state == state) rows.push_back(&purchase);
  }
  return rows;
}

void App::moveBomViewSelection(int delta) {
  const auto clampMove = [delta](size_t& selection, size_t count) {
    if (count == 0) {
      selection = 0;
      return;
    }
    selection = static_cast<size_t>(clamp(static_cast<int>(min(selection, count - 1)) + delta, 0, static_cast<int>(count) - 1));
  };
  switch (bomView_) {
    case BomView::Overview: clampMove(bomOverviewSelection_, kLastStep + 1); break;
    case BomView::Confirm: {
      const auto index = bomStepPosition_ < bomStepKeys_.size() ? bomMatchIndex(bomStepKeys_[bomStepPosition_]) : string::npos;
      clampMove(bomConfirmSelection_, index == string::npos ? 0 : bomAnalysis_.matches[index].candidates.size());
      break;
    }
    case BomView::Choose: moveBomPickerSelection(delta); return;
    case BomView::Order: clampMove(bomOrderSelection_, bomPurchasesIn(BomLineState::Chosen).size()); break;
    case BomView::Receive: clampMove(bomReceiveSelection_, bomPurchasesIn(BomLineState::Ordered).size()); break;
    default: moveBomSelection(delta); return;
  }
  dirty_ = true;
}

void App::showBomStepLine() {
  const auto* project = activeBomProject();
  if (project == nullptr) return;
  // Lines answered elsewhere since the step opened are passed over.
  while (bomStepPosition_ < bomStepKeys_.size()) {
    const auto index = bomMatchIndex(bomStepKeys_[bomStepPosition_]);
    if (index != string::npos) {
      const auto state = bomLineState(bomAnalysis_.matches[index], bomStepKeys_[bomStepPosition_], project->enrichment);
      const bool belongs = bomView_ == BomView::Confirm ? state == BomLineState::ToConfirm
                                                         : state == BomLineState::ToChoose || state == BomLineState::Chosen;
      if (belongs) break;
    }
    ++bomStepPosition_;
  }
  if (bomStepPosition_ >= bomStepKeys_.size()) {
    const bool confirming = bomView_ == BomView::Confirm;
    string summary = to_string(bomStepDone_) + (confirming ? " answered" : " chosen");
    if (bomStepSkipped_ > 0) summary += ", " + to_string(bomStepSkipped_) + " skipped";
    openBomOverview();
    bomOverviewSelection_ = confirming ? 1 : 2;
    setMessage(summary, 3, UiMessageSeverity::Success);
    return;
  }

  const auto key = bomStepKeys_[bomStepPosition_];
  const auto& match = bomAnalysis_.matches[bomMatchIndex(key)];
  const auto& line = bomAnalysis_.lines[match.lineIndex];
  if (bomView_ == BomView::Confirm) {
    bomConfirmSelection_ = match.chosen;
    dirty_ = true;
    return;
  }

  bomPicker_ = {};
  bomPicker_.open = true;
  bomPicker_.projectId = project->id;
  bomPicker_.lineKey = key;
  bomPicker_.designation = lineName(line);
  bomPicker_.quantity = max<uint64_t>(1, bomQuantityToBuy(match));
  bomPicker_.store = bomProjectStore();
  bomPicker_.query = bomPickerQuery(line);
  dirty_ = true;
  if (!bomPickerSearchable(bomPicker_.store)) {
    bomPicker_.status = "Search is not available for " + vendorDisplayName(bomPicker_.store) + " yet. Press v to change the store.";
    bomPicker_.statusWarns = true;
    return;
  }
  // A chosen part is kept without a search, so stepping through spends no requests.
  if (bomLineState(match, key, project->enrichment) == BomLineState::Chosen) {
    bomPicker_.status = "Chosen: " + bomEnrichmentPart(project->enrichment.at(key)) +
                        ". Press Enter to keep it, or / to search for another part.";
    return;
  }
  startBomPartPickerSearch(line, "");
}

void App::advanceBomStep(bool done) {
  ++(done ? bomStepDone_ : bomStepSkipped_);
  ++bomStepPosition_;
  showBomStepLine();
}

void App::useBomConfirmCandidate() {
  auto* project = activeBomProject();
  if (project == nullptr || bomStepPosition_ >= bomStepKeys_.size()) return;
  const auto key = bomStepKeys_[bomStepPosition_];
  const auto index = bomMatchIndex(key);
  if (index == string::npos) return;
  const auto& match = bomAnalysis_.matches[index];
  if (bomConfirmSelection_ >= match.candidates.size()) return;
  const auto* item = store_.findById(match.candidates[bomConfirmSelection_].itemId);
  if (item == nullptr) {
    setMessage("That stock part is gone. Choose again.", 3);
    refreshBomAnalysis();
    showBomStepLine();
    return;
  }
  const auto name = item->partName;
  const auto line = lineName(bomAnalysis_.lines[match.lineIndex]);
  project->overrides[key] = encodeBomConfirmation({item->id, bomItemFingerprint(*item)});
  bomSuggestions_.erase(key);
  refreshBomAnalysis();
  const bool saved = saveBomProjects();
  setMessage(saved ? name + " is used for " + line : "Confirmation changed in memory; press R to retry saving the project",
             saved ? 3 : 5, saved ? UiMessageSeverity::Success : UiMessageSeverity::Error);
  advanceBomStep(true);
}

void App::markBomLineNotInStock() {
  auto* project = activeBomProject();
  if (project == nullptr || bomStepPosition_ >= bomStepKeys_.size()) return;
  const auto key = bomStepKeys_[bomStepPosition_];
  const auto index = bomMatchIndex(key);
  if (index == string::npos) return;
  const auto line = lineName(bomAnalysis_.lines[bomAnalysis_.matches[index].lineIndex]);
  project->overrides[key] = kBomNotInStock;
  bomSuggestions_.erase(key);
  refreshBomAnalysis();
  saveBomProjects();
  setMessage(line + " goes to the parts to buy", 3);
  advanceBomStep(true);
}

string App::bomProjectStore() const {
  const auto* project = activeBomProject();
  if (project != nullptr) {
    const auto chosen = bomPreferredVendor(project->enrichment);
    if (namedStore(chosen)) return chosen;
  }
  // A project with no store yet starts with the one used last.
  const BomProject* last = nullptr;
  for (const auto& other : bomProjects_) {
    if (project != nullptr && other.id == project->id) continue;
    if (!namedStore(bomPreferredVendor(other.enrichment))) continue;
    if (last == nullptr || other.lastOpened > last->lastOpened) last = &other;
  }
  if (last != nullptr) return bomPreferredVendor(last->enrichment);
  const auto choices = bomVendorChoices();
  return choices.empty() ? string(kVendorProviderDigiKey) : choices.front();
}

void App::cycleBomStore() {
  auto* project = activeBomProject();
  if (project == nullptr) return;
  auto choices = bomVendorChoices();
  if (choices.empty()) choices = {kVendorProviderDigiKey, kVendorProviderTme, kVendorProviderMouser};
  const auto current = bomProjectStore();
  const auto found = find(choices.begin(), choices.end(), current);
  const auto next = found == choices.end() || found + 1 == choices.end() ? choices.front() : *(found + 1);
  project->enrichment[kBomPreferredVendorKey] = next;
  saveBomProjects();
  setMessage("Buying from " + vendorDisplayName(next), 2);
  if (bomView_ == BomView::Choose) showBomStepLine();
  dirty_ = true;
}

void App::saveBomCartFiles() {
  const auto* project = activeBomProject();
  if (project == nullptr) return;
  map<string, vector<const BomPurchase*>> byStore;
  for (const auto* row : bomPurchasesIn(BomLineState::Chosen)) byStore[row->provider].push_back(row);
  const string filter = string("CSV files (*.csv)") + '\0' + "*.csv" + '\0' + "All files (*.*)" + '\0' + "*.*" + '\0';
  vector<string> saved;
  int leftOut = 0;
  for (const auto& [provider, rows] : byStore) {
    // TME reads one "symbol;quantity" per line. DigiKey's upload asks which column is which, so it gets
    // named columns.
    string text = provider == kVendorProviderDigiKey ? "Quantity,Part Number,Customer Reference\n"
                  : provider == kVendorProviderMouser ? "Mouser Part Number,Quantity\n"
                                                      : string();
    int written = 0;
    for (const auto* row : rows) {
      if (!row->cost.has_value() || !row->offer.has_value()) {
        ++leftOut;
        continue;
      }
      const auto number = trim(row->offer->sku).empty() ? row->part : trim(row->offer->sku);
      const auto quantity = to_string(row->cost->orderQuantity);
      if (provider == kVendorProviderTme) {
        text += number + ";" + quantity + "\n";
      } else if (provider == kVendorProviderDigiKey) {
        const auto& line = bomAnalysis_.lines[bomAnalysis_.matches[row->matchIndex].lineIndex];
        text += quantity + "," + csvTextCell(number) + "," + csvTextCell(joinedDesignators(line)) + "\n";
      } else {
        text += csvTextCell(number) + "," + quantity + "\n";
      }
      ++written;
    }
    if (written == 0) continue;
    filesystem::path target = dataPath_ / (sanitizeIdPart(project->name) + "-" + provider + "-cart.csv");
    if (!saveFileDialog(target, "Save the " + vendorDisplayName(provider) + " cart file", filter, "csv")) {
      setMessage("Cart file not saved", 2);
      return;
    }
    string error;
    if (!writeFileAtomically(target, text, &error)) {
      setMessage("Unable to write " + target.filename().u8string() + (error.empty() ? string() : ": " + error), 5,
                 UiMessageSeverity::Error);
      return;
    }
    saved.push_back(target.filename().u8string());
  }
  if (saved.empty()) {
    setMessage("No chosen part can be bought now. Choose them again.", 4, UiMessageSeverity::Warning);
    return;
  }
  string message = "Saved ";
  for (size_t index = 0; index < saved.size(); ++index) message += (index == 0 ? "" : ", ") + saved[index];
  message += ". Upload it on the store's cart page.";
  if (leftOut > 0) message += " " + voiceCount(leftOut, "part", "parts") + " cannot be bought now and were left out.";
  setMessage(message, 6, leftOut > 0 ? UiMessageSeverity::Warning : UiMessageSeverity::Success);
}

void App::openBomOrderPage(bool next) {
  const auto rows = bomPurchasesIn(BomLineState::Chosen);
  if (rows.empty()) return;
  size_t index = min(bomOrderSelection_, rows.size() - 1);
  if (next) {
    const auto found = find_if(rows.begin(), rows.end(), [&](const BomPurchase* row) { return bomOrderOpened_.count(row->lineKey) == 0; });
    if (found == rows.end()) {
      setMessage("Every page is open. Press m once the order is placed.", 3);
      return;
    }
    index = static_cast<size_t>(distance(rows.begin(), found));
    bomOrderSelection_ = index;
  }
  const auto& row = *rows[index];
  const auto url = row.offer.has_value() ? vendorPurchaseUrl(*row.offer) : string();
  if (!openableUrl(url)) {
    setMessage("No " + vendorDisplayName(row.provider) + " page is saved for " + row.part, 3);
    return;
  }
  openCurrentUrl(url, vendorDisplayName(row.provider) + " " + row.part);
  bomOrderOpened_.insert(row.lineKey);
  dirty_ = true;
}

void App::markBomOrdered(bool confirmed) {
  auto* project = activeBomProject();
  const auto rows = bomPurchasesIn(BomLineState::Chosen);
  if (project == nullptr || rows.empty()) return;
  if (!confirmed) {
    bomOrderConfirm_ = true;
    dirty_ = true;
    return;
  }
  const auto now = time(nullptr);
  for (const auto* row : rows) {
    const auto quantity = row->cost.has_value() ? row->cost->orderQuantity : row->toBuy;
    project->enrichment[bomOrderKey(row->lineKey)] = encodeBomOrder({row->provider, row->part, quantity, now});
  }
  const auto count = static_cast<int>(rows.size());
  bomOrderConfirm_ = false;
  saveBomProjects();
  refreshBomProgress();
  openBomOverview();
  bomOverviewSelection_ = kLastStep;
  setMessage(voiceCount(count, "part is", "parts are") + " marked as ordered. Receive " + string(count == 1 ? "it" : "them") + " when " + string(count == 1 ? "it arrives." : "they arrive."), 4,
             UiMessageSeverity::Success);
}

string App::applyBomReceipt(InventoryStore& store, map<string, string>& overrides, const BomPurchase& purchase,
                            int quantity, string* itemId) const {
  const auto& match = bomAnalysis_.matches[purchase.matchIndex];
  const auto& line = bomAnalysis_.lines[match.lineIndex];
  const auto sku = purchase.offer.has_value() ? trim(purchase.offer->sku) : string();
  const auto same = [](const string& lhs, const string& rhs) { return !rhs.empty() && toLower(trim(lhs)) == toLower(rhs); };
  // The stock part the line already uses, else one with the same part number, else a new one.
  InventoryItem* item = match.confirmed ? store.findById(match.chosenItemId()) : nullptr;
  if (item == nullptr) {
    for (auto& candidate : store.items()) {
      if (same(candidate.vendorMetadata.manufacturerPartNumber, purchase.part) ||
          same(candidate.vendorMetadata.providerProductNumber, sku) || same(candidate.sku, sku)) {
        item = &candidate;
        break;
      }
    }
  }
  if (item == nullptr) {
    InventoryItem created;
    const auto* project = activeBomProject();
    created.id = sanitizeIdPart(purchase.part.empty() ? line.designation : purchase.part) + "-" + makeId().substr(0, 8);
    created.partName = trim(line.designation + " " + packageFromFootprint(line.footprint));
    created.manufacturer = purchase.offer.has_value() && !trim(purchase.offer->manufacturer).empty()
                               ? trim(purchase.offer->manufacturer)
                               : string("Unknown");
    created.category = bomComparisonCategory(line, match, store.items());
    if (const auto package = packageFromFootprint(line.footprint); !package.empty()) created.parameters = {{"Package", package}};
    created.sku = sku;
    created.productUrl = purchase.offer.has_value() ? purchase.offer->productUrl : string();
    if (purchase.provider == kVendorProviderDigiKey) created.digikeyPartNumber = sku;
    created.vendorMetadata.provider = purchase.provider;
    created.vendorMetadata.providerProductNumber = sku;
    created.vendorMetadata.manufacturerPartNumber = purchase.part;
    created.vendorMetadata.productUrl = created.productUrl;
    created.notes = "Received for the project " + (project == nullptr ? string() : project->name) + ".";
    created.lastUpdated = time(nullptr);
    created.createdAt = created.lastUpdated;
    store.items().push_back(move(created));
    item = &store.items().back();
  }
  item->quantity = item->quantity > numeric_limits<int>::max() - quantity ? numeric_limits<int>::max() : item->quantity + quantity;
  item->lastUpdated = time(nullptr);
  reconcileRackAssignment(store, *item);
  overrides[purchase.lineKey] = encodeBomConfirmation({item->id, bomItemFingerprint(*item)});
  if (itemId != nullptr) *itemId = item->id;
  return bomItemPlace(*item, store.racks());
}

void App::beginBomReceive() {
  const auto rows = bomPurchasesIn(BomLineState::Ordered);
  if (rows.empty()) return;
  const auto& row = *rows[min(bomReceiveSelection_, rows.size() - 1)];
  bomReceiveLineKey_ = row.lineKey;
  inputBuffer_ = to_string(row.order.has_value() ? row.order->quantity : row.toBuy);
  inputReplaceOnType_ = true;
  inputMode_ = InputMode::BomReceive;
  dirty_ = true;
}

void App::handleBomReceiveKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (numeric_prompt::applyDigit(inputBuffer_, key.ch, inputReplaceOnType_, 9U)) dirty_ = true;
    return;
  }
  if (key.type == KeyType::Backspace) {
    numeric_prompt::applyBackspace(inputBuffer_, inputReplaceOnType_);
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    bomReceiveLineKey_.clear();
    inputMode_ = InputMode::None;
    dirty_ = true;
    return;
  }
  if (key.type != KeyType::Enter) return;
  const auto text = inputBuffer_;
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  int received = 0;
  try {
    size_t parsed = 0;
    received = stoi(text, &parsed);
    if (parsed != text.size() || received <= 0) throw invalid_argument("quantity");
  } catch (...) {
    setMessage("Enter how many pieces arrived", 3);
    return;
  }
  auto* project = activeBomProject();
  const auto found = find_if(bomPurchases_.begin(), bomPurchases_.end(), [&](const BomPurchase& purchase) {
    return purchase.lineKey == bomReceiveLineKey_ && purchase.state == BomLineState::Ordered;
  });
  bomReceiveLineKey_.clear();
  if (project == nullptr || found == bomPurchases_.end()) {
    setMessage("That order is no longer waiting", 3);
    return;
  }
  const auto purchase = *found;
  captureUndoSnapshot();
  string itemId;
  const auto place = applyBomReceipt(store_, project->overrides, purchase, received, &itemId);
  project->enrichment.erase(bomOrderKey(purchase.lineKey));
  const auto* item = store_.findById(itemId);
  const auto name = item == nullptr ? purchase.part : item->partName;
  logActivity("receipt", name + " received " + to_string(received) + " for " + project->name);
  const bool saved = saveState("stock_receipt", activeBomProjectId_);
  saveBomProjects();
  refreshBomAnalysis();
  setMessage(saved ? "Received " + to_string(received) + " of " + name + (place.empty() ? "." : ". Put them in " + place + ".")
                   : "The receipt is in memory; press R to retry saving",
             saved ? 6 : 7, saved ? UiMessageSeverity::Success : UiMessageSeverity::Error);
  if (bomPurchasesIn(BomLineState::Ordered).empty()) openBomOverview();
}

void App::receiveAllBomOrders() {
  auto* project = activeBomProject();
  vector<BomPurchase> rows;
  for (const auto* row : bomPurchasesIn(BomLineState::Ordered)) rows.push_back(*row);
  if (project == nullptr || rows.empty()) return;
  // One undo step for the whole delivery.
  captureUndoSnapshot();
  string places;
  for (const auto& row : rows) {
    const auto quantity = static_cast<int>(min<uint64_t>(row.order->quantity, numeric_limits<int>::max()));
    string itemId;
    const auto place = applyBomReceipt(store_, project->overrides, row, quantity, &itemId);
    project->enrichment.erase(bomOrderKey(row.lineKey));
    const auto* item = store_.findById(itemId);
    const auto name = item == nullptr ? row.part : item->partName;
    logActivity("receipt", name + " received " + to_string(quantity) + " for " + project->name);
    if (!place.empty()) places += (places.empty() ? "" : ", ") + name + " in " + place;
  }
  const bool saved = saveState("stock_receipt", activeBomProjectId_);
  saveBomProjects();
  refreshBomAnalysis();
  openBomOverview();
  setMessage(saved ? "Received " + voiceCount(static_cast<int>(rows.size()), "part", "parts") + "." +
                         (places.empty() ? string() : " Put " + places + ".")
                   : "The receipt is in memory; press R to retry saving",
             saved ? 8 : 7, saved ? UiMessageSeverity::Success : UiMessageSeverity::Error);
}

}  // namespace inventatory
