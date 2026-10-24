// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - picking a vendor part for a BOM line by hand.

#include "App.h"

#include "core/inventory/VendorProvider.h"
#include "core/vendor/VendorOfferStore.h"
#include "core/vendor/VendorPurchase.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/system/Console.h"
#include "platform/tme/TmeApi.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace inventatory {

using namespace std;

namespace {

// The most TME products one search reads in full. Each read is three requests.
constexpr size_t kTmePartsRead = 5;

// A product is listed once: the same maker's part number from two wordings is one part.
void addCandidate(vector<VendorCandidate>& candidates, VendorCandidate candidate) {
  const bool known = any_of(candidates.begin(), candidates.end(), [&](const VendorCandidate& seen) {
    return seen.manufacturer == candidate.manufacturer && seen.manufacturerPartNumber == candidate.manufacturerPartNumber;
  });
  if (!known) candidates.push_back(move(candidate));
}

// A part that fits is enough; another wording would only add more of the same kind of answer.
bool anyFits(const BomLine& line, const vector<VendorCandidate>& candidates) {
  return any_of(candidates.begin(), candidates.end(),
                [&](const VendorCandidate& candidate) { return bomPickerFits(line, candidate); });
}

bool limitReached(const string& error) { return error.find("request limit reached") != string::npos; }

// "DigiKey search failed: DigiKey did not answer in time. ..." A reached limit is said as it is.
string pickerSearchError(const string& store, const string& error) {
  if (limitReached(error)) return error + ".";
  return store + " search failed" + (error.empty() ? string() : ": " + error) + ". Press / and Enter to try again.";
}

// Runs on the worker thread. The error names the request, never a credential.
BomPartPickerSearch searchDigiKeyForPicker(const BomLine& line, const vector<string>& queries, BomPickerProgress& progress) {
  BomPartPickerSearch result;
  const auto config = loadDigiKeyConfig();
  if (!config.valid()) {
    result.error = "DigiKey is not set up. Add its keys in Settings, Vendors.";
    return result;
  }
  DigiKeyApiClient client(config);
  for (const auto& query : queries) {
    DigiKeyLookupOutcome outcome = DigiKeyLookupOutcome::Failed;
    string error;
    auto products = client.searchProducts(query, outcome, &error);
    // A search only reads, so one that times out or meets a passing server error is sent once more.
    if (outcome == DigiKeyLookupOutcome::Failed && !limitReached(error)) {
      error.clear();
      products = client.searchProducts(query, outcome, &error);
    }
    progress.done.fetch_add(1);
    if (outcome == DigiKeyLookupOutcome::Failed) {
      // A later search that fails keeps the answers before it.
      if (result.candidates.empty()) {
        result.error = pickerSearchError("DigiKey", error);
        return result;
      }
      break;
    }
    for (const auto& product : products) addCandidate(result.candidates, digiKeyCandidate(product));
    if (anyFits(line, result.candidates)) break;
  }
  result.ok = true;
  return result;
}

// Runs on the worker thread. TME's search lists products without price or stock, so the first few are read
// in full. A product that cannot be read is skipped unless nothing else was found.
BomPartPickerSearch searchTmeForPicker(const BomLine& line, const vector<string>& queries, BomPickerProgress& progress) {
  BomPartPickerSearch result;
  const auto config = loadTmeConfig();
  if (!config.valid()) {
    result.error = "TME is not set up. Add its keys in Settings, Vendors.";
    return result;
  }
  TmeApiClient client(config);
  for (const auto& query : queries) {
    TmeLookupOutcome outcome = TmeLookupOutcome::Failed;
    string error;
    auto products = client.searchProducts(query, outcome, &error);
    if (outcome == TmeLookupOutcome::Failed) {
      if (result.candidates.empty()) {
        result.error = pickerSearchError("TME", error);
        return result;
      }
      break;
    }
    progress.done.fetch_add(1);
    const auto reads = min(products.size(), kTmePartsRead);
    progress.total.store(progress.done.load() + reads);
    for (size_t index = 0; index < reads; ++index) {
      auto& product = products[index];
      if (product.orderable && client.fetchProductDetails(product, &error)) {
        addCandidate(result.candidates, tmeCandidate(product));
      } else if (product.orderable && result.candidates.empty()) {
        result.error = pickerSearchError("TME", error);
        return result;
      }
      progress.done.fetch_add(1);
    }
    if (anyFits(line, result.candidates)) break;
  }
  result.ok = true;
  return result;
}

}  // namespace

void App::startBomPartPickerSearch(const BomLine& line, const string& typed) {
  const auto queries = bomPickerQueries(line, typed);
  if (queries.empty()) return;
  // One search runs at a time. A search asked for meanwhile starts once the running one ends.
  if (bomPickerFuture_.valid()) {
    bomPicker_.pendingSearch = typed;
    bomPicker_.entries.clear();
    bomPicker_.status = "Waiting for the last search to finish.";
    dirty_ = true;
    return;
  }
  bomPicker_.pendingSearch.reset();
  // The query shown is the one sent, so a typed "0,33uF" shows as "0.33 uF 0603 (1608 Metric)".
  bomPicker_.query = queries.front();
  bomPicker_.reading = bomPickerReading(line, typed);
  bomPicker_.entries.clear();
  bomPicker_.selection = 0;
  bomPicker_.status.clear();
  bomPicker_.statusWarns = false;
  bomPicker_.progress = make_shared<BomPickerProgress>();
  // DigiKey has one step per search. TME has one step for its search, then one per product it reads.
  bomPicker_.progress->total.store(bomPicker_.store == kVendorProviderTme ? size_t{1} : queries.size());
  bomPicker_.searchStarted = chrono::steady_clock::now();
  dirty_ = true;
  // The keyring can block, so credentials are read on the worker. The worker shares only the counters.
  const auto store = bomPicker_.store;
  bomPickerFuture_ = async(launch::async, [line, queries, store, key = bomPicker_.lineKey, progress = bomPicker_.progress] {
    auto result = store == kVendorProviderTme ? searchTmeForPicker(line, queries, *progress)
                                              : searchDigiKeyForPicker(line, queries, *progress);
    result.lineKey = key;
    return result;
  });
}

void App::searchBomPartPicker(const string& words) {
  const auto query = trim(words);
  if (!bomPicker_.open || query.empty() || !bomPickerSearchable(bomPicker_.store)) return;
  const auto line = find_if(bomAnalysis_.lines.begin(), bomAnalysis_.lines.end(),
                            [&](const BomLine& candidate) { return bomLineKey(candidate) == bomPicker_.lineKey; });
  if (line == bomAnalysis_.lines.end()) return;
  startBomPartPickerSearch(*line, query);
}

void App::openBomPartPage() {
  if (!bomPicker_.open) return;
  if (bomPicker_.selection >= bomPicker_.entries.size()) {
    setMessage("No part to open yet", 2);
    return;
  }
  const auto store = vendorDisplayName(bomPicker_.store);
  const auto& candidate = bomPicker_.entries[bomPicker_.selection].candidate;
  const auto part = trim(candidate.manufacturerPartNumber);
  auto url = trim(candidate.productUrl);
  if (url.empty() && !candidate.offers.empty()) url = vendorPurchaseUrl(candidate.offers.front());
  // DigiKey sometimes stores no product page for a part. Its search for the part number still finds it.
  const bool searching = url.empty() && bomPicker_.store == kVendorProviderDigiKey && !part.empty();
  if (searching) url = digiKeySearchUrl(part);
  if (url.empty()) {
    setMessage("No " + store + " page is stored for " + part, 3);
    return;
  }
  if (openUrl(url)) {
    setMessage(searching ? "No DigiKey page for " + part + ". Opened its search for the part."
                         : "Opened the " + store + " page for " + part,
               2);
  } else {
    setMessage("Unable to open the " + store + " page for " + part, 3);
  }
}

void App::processBomPartPicker() {
  if (!bomPickerFuture_.valid() || bomPickerFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;
  const auto result = bomPickerFuture_.get();
  dirty_ = true;
  if (!bomPicker_.open) return;
  // The user moved to another line or store while this ran: its answer is dropped, and the search
  // asked for meanwhile starts.
  if (bomPicker_.pendingSearch.has_value()) {
    const auto typed = *bomPicker_.pendingSearch;
    const auto index = bomMatchIndex(bomPicker_.lineKey);
    if (index != string::npos) startBomPartPickerSearch(bomAnalysis_.lines[bomAnalysis_.matches[index].lineIndex], typed);
    return;
  }
  if (result.lineKey != bomPicker_.lineKey) return;
  if (!result.ok) {
    bomPicker_.status = result.error;
    bomPicker_.statusWarns = true;
    return;
  }
  const auto line = find_if(bomAnalysis_.lines.begin(), bomAnalysis_.lines.end(),
                            [&](const BomLine& candidate) { return bomLineKey(candidate) == bomPicker_.lineKey; });
  if (line == bomAnalysis_.lines.end()) {
    closeBomPartPicker();
    return;
  }
  auto ranking = rankBomPickerCandidates(*line, bomPicker_.quantity, result.candidates);
  bomPicker_.entries = move(ranking.entries);
  bomPicker_.selection = 0;
  // A part chosen before starts selected, so Enter keeps it.
  if (const auto* project = activeBomProject()) {
    const auto found = project->enrichment.find(bomPicker_.lineKey);
    const auto chosen = found == project->enrichment.end() ? string() : bomEnrichmentPart(found->second);
    for (size_t index = 0; index < bomPicker_.entries.size(); ++index) {
      if (!chosen.empty() && trim(bomPicker_.entries[index].candidate.manufacturerPartNumber) == chosen) bomPicker_.selection = index;
    }
  }
  bomPicker_.status = bomPickerSummary(vendorDisplayName(bomPicker_.store), bomPicker_.entries.size(), ranking.leftOut,
                                       ranking.otherValue);
  bomPicker_.statusWarns = bomPicker_.entries.empty();
}

void App::moveBomPickerSelection(int delta) {
  if (bomPicker_.entries.empty()) return;
  const auto last = static_cast<int>(bomPicker_.entries.size()) - 1;
  bomPicker_.selection = static_cast<size_t>(clamp(static_cast<int>(bomPicker_.selection) + delta, 0, last));
  dirty_ = true;
}

void App::useBomPickerSelection() {
  if (!bomPicker_.open || bomPicker_.selection >= bomPicker_.entries.size()) return;
  const auto project = find_if(bomProjects_.begin(), bomProjects_.end(),
                               [&](const BomProject& candidate) { return candidate.id == bomPicker_.projectId; });
  if (project == bomProjects_.end()) {
    closeBomPartPicker();
    return;
  }
  const auto& entry = bomPicker_.entries[bomPicker_.selection];
  const auto part = trim(entry.candidate.manufacturerPartNumber);
  BomEnrichmentFlags flags;
  flags.verified = entry.fits;
  flags.chosen = true;
  const auto stored = bomEnrichmentEntry(BomLookupOutcome::Found, part, flags, bomPicker_.store);
  if (!stored.has_value() || bomEnrichmentPart(*stored).empty()) return;
  project->enrichment[bomPicker_.lineKey] = *stored;
  // The search already carried the part's prices, so they are saved without another request.
  if (saveVendorOffers(vendorOfferDatabasePath(dataPath_), bomPicker_.store, part, entry.candidate.offers)) {
    offerCacheValid_ = false;
  }
  saveBomProjects();
  refreshBomProgress();
  setMessage("Chose " + part + " for " + bomPicker_.designation, 3, UiMessageSeverity::Success);
  advanceBomStep(true);
}

void App::closeBomPartPicker() {
  bomPicker_ = {};
  if (inputMode_ == InputMode::BomPartSearch) {
    inputMode_ = InputMode::None;
    inputBuffer_.clear();
  }
  dirty_ = true;
}

void App::beginBomPartSearchEdit() {
  if (!bomPicker_.open) return;
  if (!bomPickerSearchable(bomPicker_.store)) {
    setMessage("Search is not available for " + vendorDisplayName(bomPicker_.store) + " yet. Press v to change the store.", 4);
    return;
  }
  inputBuffer_ = bomPicker_.query;
  inputReplaceOnType_ = true;
  inputMode_ = InputMode::BomPartSearch;
  dirty_ = true;
}

void App::handleBomPartSearchKey(const KeyEvent& key) {
  constexpr size_t kMaximumQueryBytes = 200;
  // The first key replaces the words the prompt starts with, so a new search does not add to the last one.
  if (inputReplaceOnType_ && (key.type == KeyType::Character || key.type == KeyType::Backspace)) {
    inputBuffer_.clear();
    inputReplaceOnType_ = false;
    dirty_ = true;
  }
  if (key.type == KeyType::Character) {
    if (key.ch >= 32 && key.ch < 127 && inputBuffer_.size() < kMaximumQueryBytes) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
    }
    return;
  }
  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) inputBuffer_.pop_back();
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    dirty_ = true;
    return;
  }
  if (key.type != KeyType::Enter) return;
  const auto words = inputBuffer_;
  inputBuffer_.clear();
  inputMode_ = InputMode::None;
  searchBomPartPicker(words);
}

}  // namespace inventatory
