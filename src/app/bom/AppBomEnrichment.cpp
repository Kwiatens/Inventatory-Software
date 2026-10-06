// Inventatory - asynchronous BOM metadata enrichment.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "import/csv/CsvFormat.h"
#include "core/storage/InventorySqlite.h"
#include "platform/digikey/DigiKeyApi.h"
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

void App::queueBomEnrichment() {
  bomEnrichmentQueue_.clear();
  bomEnrichmentTotal_ = 0;
  const auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_) {
    bomEnrichmentProjectId_.clear();
    ++bomEnrichmentSequence_;
    if (!bomEnrichmentFuture_.valid()) {
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
      bomEnrichmentClient_.reset();
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
        if (const auto entry = bomEnrichmentEntry(result.outcome, result.suggestion)) {
          targetProject->enrichment[result.key] = *entry;
        } else {
          // Drop a stale "-" left behind by earlier versions rather than keep showing it.
          targetProject->enrichment.erase(result.key);
        }
        dirty_ = true;
      }
    }
    if (result.credentialsMissing) {
      bomEnrichmentQueue_.clear();
      bomEnrichmentTotal_ = 0;
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
      bomEnrichmentClient_.reset();
      return;
    }
    if (result.projectId == bomEnrichmentActiveProjectId_) {
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
    }
    if (context == nullptr || !workspaceGenerationMatches(context->generation, result.workspaceGeneration)) {
      bomEnrichmentQueue_.clear();
      bomEnrichmentTotal_ = 0;
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
      bomEnrichmentClient_.reset();
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
      bomEnrichmentQueue_.clear();
      bomEnrichmentTotal_ = 0;
      bomEnrichmentActiveKey_.clear();
      bomEnrichmentActiveProjectId_.clear();
      bomEnrichmentClient_.reset();
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

  // fetchProductDetails falls back to a keyword search, so a free-form value
  // such as "470uF Radial 8.0mm" resolves as well as a real part number.
  const auto keywords = trim(line->designation + " " + packageFromFootprint(line->footprint));
  const auto session = bomEnrichmentClient_;
  const auto generation = bomEnrichmentGeneration_;
  const auto projectId = project->id;
  const auto requestSequence = bomEnrichmentSequence_;
  bomEnrichmentFuture_ = async(launch::async, [session, key, keywords, projectId, generation, requestSequence] {
    string error;
    DigiKeyLookupOutcome lookup = DigiKeyLookupOutcome::Failed;
    BomEnrichmentResult result{key, {}, BomLookupOutcome::Failed, projectId, generation, requestSequence};
    if (session->client == nullptr) {
      const auto config = loadDigiKeyConfig();
      if (!config.valid()) {
        result.credentialsMissing = true;
        return result;
      }
      session->client = make_unique<DigiKeyApiClient>(config);
    }
    if (const auto details = session->client->lookupProductDetails(keywords, lookup, &error)) {
      result.suggestion = details->manufacturerPartNumber.empty() ? details->lookupKey : details->manufacturerPartNumber;
      result.outcome = BomLookupOutcome::Found;
    } else if (lookup == DigiKeyLookupOutcome::NoMatch) {
      result.outcome = BomLookupOutcome::NoMatch;
    }
    return result;
  });
  dirty_ = true;
}

}  // namespace inventatory
