// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - asynchronous BOM metadata enrichment.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/bom/BomMouserSuggestion.h"
#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/mouser/MouserApi.h"
#include "platform/tme/TmeApi.h"
#include "platform/security/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace inventatory {

using namespace std;
using namespace app_actions;

namespace {

// TME's answer for a BOM line: the first fitting candidate, else the first orderable, unverified.
struct TmeSuggestion {
  BomLookupOutcome outcome = BomLookupOutcome::Failed;
  string suggestion;
  bool verified = false;
};

TmeSuggestion lookupTmeSuggestion(TmeApiClient& client, const string& keywords, const BomLine& line) {
  // Fit is only known once parameters and package are read, so read the first few in full.
  constexpr size_t kCandidatesRead = 3;
  TmeSuggestion answer;
  TmeLookupOutcome searched = TmeLookupOutcome::Failed;
  string error;
  auto candidates = client.searchProducts(keywords, searched, &error);
  if (searched == TmeLookupOutcome::NoMatch) {
    answer.outcome = BomLookupOutcome::NoMatch;
    return answer;
  }
  if (searched != TmeLookupOutcome::Found) return answer;  // a failure is retried later
  for (size_t index = 0; index < candidates.size() && index < kCandidatesRead; ++index) {
    auto& candidate = candidates[index];
    // A part TME cannot sell is never suggested for a line, however well it fits.
    if (!candidate.orderable) continue;
    if (!client.fetchProductDetails(candidate, &error)) return answer;  // a failure is retried later
    if (answer.suggestion.empty()) answer.suggestion = candidate.vendorMetadata.providerProductNumber;
    // Judge the record as a stock item is judged: by part number, or by value and package.
    InventoryItem probe;
    probe.partName = candidate.productDescription;
    probe.sku = candidate.vendorMetadata.manufacturerPartNumber;
    probe.parameters = candidate.parameters;
    if (bomSuggestionFitsLine(line, probe)) {
      answer.suggestion = candidate.vendorMetadata.providerProductNumber;
      answer.verified = true;
      answer.outcome = BomLookupOutcome::Found;
      return answer;
    }
  }
  answer.outcome = answer.suggestion.empty() ? BomLookupOutcome::NoMatch : BomLookupOutcome::Found;
  return answer;
}

// Mouser's sellable record for the line's part number; limitReached is set at the request limit.
BomMouserSuggestion lookupMouserSuggestion(MouserApiClient& client, const string& partNumber, const BomLine& line,
                                           bool& limitReached) {
  BomMouserSuggestion answer;
  MouserLookupOutcome lookup = MouserLookupOutcome::Failed;
  string error;
  // BOM lines name no manufacturer, so none is passed to disambiguate.
  const auto product = client.lookupByPartNumber(partNumber, lookup, &error);
  switch (lookup) {
    case MouserLookupOutcome::Found:
      if (product.has_value()) return bomMouserSuggestion(line, *product);
      break;
    case MouserLookupOutcome::NoMatch:
    case MouserLookupOutcome::Ambiguous:
      // Never guess between several products; asking again gets the same answer.
      answer.outcome = BomLookupOutcome::NoMatch;
      break;
    case MouserLookupOutcome::Failed:
      // Failure is retried later; a reached limit also ends Mouser for this run.
      limitReached = mouserLimitReached(error);
      break;
  }
  return answer;
}

}  // namespace

void App::clearBomEnrichmentRun() {
  bomEnrichmentQueue_.clear();
  bomEnrichmentTotal_ = 0;
  bomEnrichmentActiveKey_.clear();
  bomEnrichmentActiveProjectId_.clear();
  bomEnrichmentClient_.reset();
}

void App::queueBomEnrichment() {
  bomEnrichmentQueue_.clear();
  bomEnrichmentTotal_ = 0;
  const auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_) {
    bomEnrichmentProjectId_.clear();
    ++bomEnrichmentSequence_;
    if (!bomEnrichmentFuture_.valid()) {
      clearBomEnrichmentRun();
    }
    return;
  }
  bomEnrichmentProjectId_ = project->id;
  // A new run invalidates any result still in flight; it is checked before being applied.
  ++bomEnrichmentSequence_;
  // The worker reads credentials (the keyring can block); missing config ends the run quietly.
  if (const auto context = currentWorkspaceContext(); context != nullptr) {
    bomEnrichmentGeneration_ = context->generation;
  } else {
    return;
  }
  for (const auto& match : bomAnalysis_.matches) {
    if (match.sufficient) {
      continue;
    }
    const auto key = bomLineKey(bomAnalysis_.lines[match.lineIndex]);
    if (bomEnrichmentCached(project->enrichment, key)) {
      continue;  // already answered and cached with the project
    }
    if (find(bomEnrichmentQueue_.begin(), bomEnrichmentQueue_.end(), key) == bomEnrichmentQueue_.end()) {
      bomEnrichmentQueue_.push_back(key);
    }
  }
  bomEnrichmentTotal_ = bomEnrichmentQueue_.size();
}

void App::processBomEnrichment() {
  // Collect the finished lookup before starting the next; one request at a time keeps the token.
  if (bomEnrichmentFuture_.valid()) {
    if (bomEnrichmentFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
      return;
    }
    const auto result = bomEnrichmentFuture_.get();
    const auto context = currentWorkspaceContext();
    if (result.requestSequence != bomEnrichmentSequence_) {
      // Superseded by a reopen or re-import; release the old client so the next run uses new keys.
      bomEnrichmentClient_.reset();
    }
    if (bomEnrichmentScopeMatches(bomEnrichmentProjectId_, result.projectId,
                                  context == nullptr ? 0 : context->generation,
                                  result.workspaceGeneration, bomEnrichmentSequence_,
                                  result.requestSequence)) {
      const auto targetProject = find_if(bomProjects_.begin(), bomProjects_.end(), [&](BomProject& candidate) {
        return candidate.id == result.projectId;
      });
      if (targetProject != bomProjects_.end() && !result.key.empty()) {
        // A failed lookup stores nothing, so the line is retried next time.
        if (const auto entry = bomEnrichmentEntry(result.outcome, result.suggestion, result.verified, result.fromTme,
                                                  result.fromMouser)) {
          targetProject->enrichment[result.key] = *entry;
        } else {
          // Drop a stale "-" entry from earlier versions.
          targetProject->enrichment.erase(result.key);
        }
        dirty_ = true;
      }
    }
    if (result.credentialsMissing) {
      clearBomEnrichmentRun();
      return;
    }
    if (result.projectId == bomEnrichmentActiveProjectId_) {
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
    }
    if (context == nullptr || !workspaceGenerationMatches(context->generation, result.workspaceGeneration)) {
      clearBomEnrichmentRun();
      return;
    }
    if (bomEnrichmentQueue_.empty() && result.requestSequence == bomEnrichmentSequence_) {
      bomEnrichmentClient_.reset();
      // Save the project named by the result, not whichever is selected now.
      if (!result.projectId.empty()) saveBomProjects();
      return;
    }
  }

  auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_ || project->id != bomEnrichmentProjectId_) {
    if (!bomEnrichmentFuture_.valid()) {
      clearBomEnrichmentRun();
    }
    return;
  }

  if (bomEnrichmentQueue_.empty()) {
    return;
  }

  if (bomEnrichmentClient_ == nullptr) {
    bomEnrichmentClient_ = make_shared<BomEnrichmentSession>();
  }

  const auto key = bomEnrichmentQueue_.front();
  bomEnrichmentQueue_.erase(bomEnrichmentQueue_.begin());
  bomEnrichmentActiveKey_ = key;
  bomEnrichmentActiveProjectId_ = project->id;

  const auto line = find_if(bomAnalysis_.lines.begin(), bomAnalysis_.lines.end(),
                            [&](const BomLine& candidate) { return bomLineKey(candidate) == key; });
  if (line == bomAnalysis_.lines.end()) {
    return;
  }

  // A keyword search (no MPN) can return a near miss, so its hit is checked against the line.
  const BomLine lineSnapshot = *line;
  const auto keywords = trim(line->manufacturerPartNumber.empty()
                                 ? line->designation + " " + packageFromFootprint(line->footprint)
                                 : line->manufacturerPartNumber);
  const auto session = bomEnrichmentClient_;
  const auto generation = bomEnrichmentGeneration_;
  const auto projectId = project->id;
  const auto requestSequence = bomEnrichmentSequence_;
  bomEnrichmentFuture_ = async(launch::async, [session, key, keywords, lineSnapshot, projectId, generation,
                                                requestSequence] {
    BomEnrichmentResult result{key, {}, false, BomLookupOutcome::Failed, projectId, generation, requestSequence};
    // Read credentials here: the keyring can block. Unconfigured suppliers are skipped.
    if (session->client == nullptr) {
      const auto config = loadDigiKeyConfig();
      if (config.valid()) session->client = make_unique<DigiKeyApiClient>(config);
    }
    if (session->tmeClient == nullptr) {
      const auto config = loadTmeConfig();
      if (config.valid()) session->tmeClient = make_unique<TmeApiClient>(config);
    }
    // Read the Mouser key per lookup, so a key saved mid-run applies to the next line.
    const auto mouserConfig = loadMouserConfig();
    const bool mouserConfigured = mouserConfig.valid();
    if (session->client == nullptr && session->tmeClient == nullptr && !mouserConfigured) {
      result.credentialsMissing = true;
      return result;
    }
    // Order: DigiKey, TME, then Mouser; each is asked only when the earlier one has no part.
    if (session->client != nullptr) {
      string error;
      DigiKeyLookupOutcome lookup = DigiKeyLookupOutcome::Failed;
      if (const auto details = session->client->lookupProductDetails(keywords, lookup, &error)) {
        result.suggestion = details->manufacturerPartNumber.empty() ? details->lookupKey : details->manufacturerPartNumber;
        // Judge the record as a stock item is judged: by part number, or by value and package.
        InventoryItem probe;
        probe.partName = details->productDescription;
        probe.sku = details->manufacturerPartNumber;
        probe.parameters = details->parameters;
        if (!details->packageName.empty()) probe.parameters.push_back({"Package / Case", details->packageName});
        result.verified = bomSuggestionFitsLine(lineSnapshot, probe);
        result.outcome = BomLookupOutcome::Found;
        return result;
      }
      // Only a clear "no part" moves on to TME; failures are retried later.
      if (lookup != DigiKeyLookupOutcome::NoMatch) return result;
    }
    if (session->tmeClient != nullptr) {
      const auto answer = lookupTmeSuggestion(*session->tmeClient, keywords, lineSnapshot);
      // Only a clear "no part" moves on to Mouser; a failed request is retried later.
      if (answer.outcome != BomLookupOutcome::NoMatch) {
        result.suggestion = answer.suggestion;
        result.verified = answer.verified;
        result.outcome = answer.outcome;
        result.fromTme = true;
        return result;
      }
    }
    // Mouser is asked by part number only, so a line without one has no part there either.
    const auto mouserPartNumber = bomMouserQuery(lineSnapshot);
    if (!mouserConfigured || mouserPartNumber.empty()) {
      result.outcome = BomLookupOutcome::NoMatch;
      return result;
    }
    // At Mouser's request limit, leave these lines unanswered for a later run.
    if (session->mouserLimitReached) return result;
    BomMouserSuggestion answer;
    try {
      MouserApiClient mouserClient(mouserConfig);
      answer = lookupMouserSuggestion(mouserClient, mouserPartNumber, lineSnapshot, session->mouserLimitReached);
    } catch (...) {
      // Store nothing: the API key is in the URL, so the exception text is not kept.
      return result;
    }
    result.suggestion = answer.suggestion;
    result.verified = answer.verified;
    result.outcome = answer.outcome;
    result.fromMouser = true;
    return result;
  });
  dirty_ = true;
}

}  // namespace inventatory
