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

// TME's answer for one BOM line: the symbol of the first candidate that fits the line, or of the first candidate
// when none fits, which is then shown as needing a check.
struct TmeSuggestion {
  BomLookupOutcome outcome = BomLookupOutcome::Failed;
  string suggestion;
  bool verified = false;
};

TmeSuggestion lookupTmeSuggestion(TmeApiClient& client, const string& keywords, const BomLine& line) {
  // A candidate is only known to fit once its parameters and package are read, so the first few are read in full.
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
    // The record is judged the same way a stock item is: by its part number, or by its value and package.
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

// Mouser's answer for one BOM line: the Mouser part number of the one record the line's part number names, when Mouser
// can sell it. limitReached is set when Mouser refused the request because its request limit was reached.
BomMouserSuggestion lookupMouserSuggestion(MouserApiClient& client, const string& partNumber, const BomLine& line,
                                           bool& limitReached) {
  BomMouserSuggestion answer;
  MouserLookupOutcome lookup = MouserLookupOutcome::Failed;
  string error;
  // A BOM line names no manufacturer, so none is passed to tell apart products that share the part number.
  const auto product = client.lookupByPartNumber(partNumber, lookup, &error);
  switch (lookup) {
    case MouserLookupOutcome::Found:
      if (product.has_value()) return bomMouserSuggestion(line, *product);
      break;
    case MouserLookupOutcome::NoMatch:
    case MouserLookupOutcome::Ambiguous:
      // Several products with the part number are never guessed between, and asking again would get the same answer.
      answer.outcome = BomLookupOutcome::NoMatch;
      break;
    case MouserLookupOutcome::Failed:
      // A failure is retried later. A reached request limit also ends Mouser's part of this run.
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
  // A new queue run invalidates any result still in flight.  The old future is
  // allowed to finish against its captured client, then its project/sequence
  // pair is checked before anything is applied or saved.
  ++bomEnrichmentSequence_;
  // Credentials are looked up by the worker (the credential store can block on a locked keyring) and a
  // missing configuration ends the run quietly, so an offline user never sees an error they cannot act on.
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
      continue;  // answered earlier and cached with the pinned project
    }
    if (find(bomEnrichmentQueue_.begin(), bomEnrichmentQueue_.end(), key) == bomEnrichmentQueue_.end()) {
      bomEnrichmentQueue_.push_back(key);
    }
  }
  bomEnrichmentTotal_ = bomEnrichmentQueue_.size();
}

void App::processBomEnrichment() {
  // Collect a finished lookup first, then start the next one. Only ever one
  // request is outstanding, so the shared client's cached token is safe.
  if (bomEnrichmentFuture_.valid()) {
    if (bomEnrichmentFuture_.wait_for(chrono::seconds(0)) != future_status::ready) {
      return;
    }
    const auto result = bomEnrichmentFuture_.get();
    const auto context = currentWorkspaceContext();
    if (result.requestSequence != bomEnrichmentSequence_) {
      // A project reopen/re-import superseded this lookup.  It is safe to
      // release the old client now that its future has been joined, ensuring
      // a subsequent run uses the current credentials.
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
        // A failed lookup stores nothing so the line is retried the next time the project opens.
        if (const auto entry = bomEnrichmentEntry(result.outcome, result.suggestion, result.verified, result.fromTme,
                                                  result.fromMouser)) {
          targetProject->enrichment[result.key] = *entry;
        } else {
          // Drop a stale "-" left behind by earlier versions rather than keep showing it.
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
      // Persist the project identified by the result, never whichever project
      // happens to be selected when the future completes.
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

  // A manufacturer part number is the precise query. Without one, the keyword search uses the value and package,
  // which can return a near miss, so its answer is checked against the line before it is shown as verified.
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
    // Credentials are read by the worker, because the credential store can block. A supplier that is not configured
    // is skipped; a run with no supplier configured ends quietly.
    if (session->client == nullptr) {
      const auto config = loadDigiKeyConfig();
      if (config.valid()) session->client = make_unique<DigiKeyApiClient>(config);
    }
    if (session->tmeClient == nullptr) {
      const auto config = loadTmeConfig();
      if (config.valid()) session->tmeClient = make_unique<TmeApiClient>(config);
    }
    // Mouser's key is read for every lookup and a fresh client is built from it, so a key saved during the run applies
    // from the next line. That is safe because the request spacing is shared by every Mouser client in the process.
    const auto mouserConfig = loadMouserConfig();
    const bool mouserConfigured = mouserConfig.valid();
    if (session->client == nullptr && session->tmeClient == nullptr && !mouserConfigured) {
      result.credentialsMissing = true;
      return result;
    }
    // DigiKey is asked first. TME is asked when DigiKey has no part for the line, or when DigiKey is not configured.
    // Mouser is asked last, when neither DigiKey nor TME has a part for the line or is configured.
    if (session->client != nullptr) {
      string error;
      DigiKeyLookupOutcome lookup = DigiKeyLookupOutcome::Failed;
      if (const auto details = session->client->lookupProductDetails(keywords, lookup, &error)) {
        result.suggestion = details->manufacturerPartNumber.empty() ? details->lookupKey : details->manufacturerPartNumber;
        // The supplier's record is judged the same way a stock item is: by its part number, or by its value and package.
        InventoryItem probe;
        probe.partName = details->productDescription;
        probe.sku = details->manufacturerPartNumber;
        probe.parameters = details->parameters;
        if (!details->packageName.empty()) probe.parameters.push_back({"Package / Case", details->packageName});
        result.verified = bomSuggestionFitsLine(lineSnapshot, probe);
        result.outcome = BomLookupOutcome::Found;
        return result;
      }
      // Only a clear "no part" moves on to TME; a failed request is retried later, as it always was.
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
    // Once Mouser's request limit is reached, the lines only Mouser could answer stay unanswered (a failure, which
    // stores nothing) and are asked again in a later run, rather than spending more of the quota now.
    if (session->mouserLimitReached) return result;
    MouserApiClient mouserClient(mouserConfig);
    const auto answer =
        lookupMouserSuggestion(mouserClient, mouserPartNumber, lineSnapshot, session->mouserLimitReached);
    result.suggestion = answer.suggestion;
    result.verified = answer.verified;
    result.outcome = answer.outcome;
    result.fromMouser = true;
    return result;
  });
  dirty_ = true;
}

}  // namespace inventatory
