// Inventatory - Hardware Inventory Management System
// CSV import and DigiKey synchronization actions.

#include "App.h"
#include "app/AppActionSupport.h"

#include "import/CsvFormat.h"
#include "core/InventorySqlite.h"
#include "platform/DigiKeyApi.h"
#include "platform/CredentialStore.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <memory>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace inventatory {

using namespace std;
using namespace app_actions;

void App::beginCsvImport() {
  filesystem::path selectedPath;
  if (!openCsvFileDialog(selectedPath)) {
    setMessage("CSV import cancelled", 2);
    return;
  }

  ifstream input(selectedPath, ios::binary);
  if (!input) {
    setMessage("Unable to open " + selectedPath.filename().string(), 6);
    return;
  }
  error_code fileError;
  if (const auto size = filesystem::file_size(selectedPath, fileError); fileError || size > kMaximumImportBytes) {
    setMessage("CSV import exceeds the 25 MiB safety limit", 6);
    return;
  }
  ostringstream buffer;
  buffer << input.rdbuf();
  const auto text = buffer.str();

  // The file picker is the only way in, so detection replaces asking the user
  // which dialect they just chose.
  if (detectCsvFormat(text) == CsvFormat::KicadBom) {
    beginBomProject(text, projectNameFromPath(selectedPath), selectedPath);
    return;
  }

  const auto result = parseDigiKeyCsvText(text, store_.items());
  if (!result.ok) {
    setMessage("CSV import failed: " + result.error, 6);
    return;
  }

  importCandidates_ = result.candidates;
  importOriginalStore_ = store_;
  importStagedStore_ = store_;
  importStageActive_ = true;
  importCommitPending_ = false;
  importAcceptedItemIds_.clear();
  importSourcePath_ = selectedPath;
  importSelection_ = 0;
  importSyncPrompt_ = false;
  importCreatedCount_ = 0;
  importMergedCount_ = 0;
  importSkippedCount_ = 0;
  importSyncedCount_ = 0;
  importSyncFailedCount_ = 0;
  importSyncFailedItemIds_.clear();
  importSyncTotal_ = 0;
  importSyncCompleted_ = 0;
  importSyncRunning_ = false;
  importSyncHasRun_ = false;
  importSyncCancelRequested_ = false;
  editingImportCandidate_ = false;
  inputMode_ = InputMode::None;
  page_ = Page::Import;

  string summary = "Loaded " + to_string(importCandidates_.size()) + " CSV rows for review";
  if (!result.warnings.empty()) {
    summary += " · " + to_string(result.warnings.size()) + " rows skipped";
  }
  setMessage(summary, 4);
}

void App::cancelImportSession() {
  if (!importStageActive_ && !importCommitPending_) return;
  if (importCommitPending_) {
    // The staged copy was assigned to the live view only to let the existing
    // save-retry path retry the exact same operation. Restore the pre-import
    // view when the user explicitly cancels that retry.
    store_ = importOriginalStore_;
  }
  importCandidates_.clear();
  importAcceptedItemIds_.clear();
  importSourcePath_.clear();
  importOriginalStore_ = {};
  importStagedStore_ = {};
  importStageActive_ = false;
  importCommitPending_ = false;
  importSelection_ = 0;
  importSyncPrompt_ = false;
  importSyncHasRun_ = false;
  importSyncFailedItemIds_.clear();
  editingImportCandidate_ = false;
  inputMode_ = InputMode::None;
  dirty_ = true;
}

bool App::commitImportStage() {
  if (!importStageActive_) return true;
  store_ = importStagedStore_;
  if (!saveState("import", importSourcePath_.filename().string())) {
    importCommitPending_ = true;
    setMessage("Import is staged but not saved. Press R to retry or Q to cancel.", 7);
    dirty_ = true;
    return false;
  }
  importStageActive_ = false;
  importCommitPending_ = false;
  return true;
}

CsvImportCandidate* App::currentImportCandidate() {
  if (importCandidates_.empty()) {
    return nullptr;
  }
  importSelection_ = min(importSelection_, importCandidates_.size() - 1);
  return &importCandidates_[importSelection_];
}

const CsvImportCandidate* App::currentImportCandidate() const {
  if (importCandidates_.empty()) {
    return nullptr;
  }
  const auto index = min(importSelection_, importCandidates_.size() - 1);
  return &importCandidates_[index];
}

void App::moveImportSelection(int delta) {
  if (importCandidates_.empty()) {
    importSelection_ = 0;
    dirty_ = true;
    return;
  }

  const auto current = static_cast<int>(min(importSelection_, importCandidates_.size() - 1));
  importSelection_ = static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(importCandidates_.size() - 1)));
  dirty_ = true;
}

void App::acceptImportCandidate() {
  auto* candidate = currentImportCandidate();
  if (candidate == nullptr) {
    finishImportReview();
    return;
  }

  string acceptedId;
  if (candidate->hasConflict) {
      auto* existing = importStagedStore_.findById(candidate->existingItemId);
    if (existing != nullptr) {
      captureUndoSnapshot();
      existing->quantity = candidate->item.quantity > 0 &&
                                  existing->quantity > numeric_limits<int>::max() - candidate->item.quantity
                              ? numeric_limits<int>::max()
                              : max(0, existing->quantity + candidate->item.quantity);
      existing->lastUpdated = time(nullptr);
      mergeImportedMetadata(*existing, candidate->item);
      if (candidate->item.rackAssignment != RackAssignmentMode::Automatic) {
        existing->rackId = candidate->item.rackId;
        existing->rackSlot = candidate->item.rackSlot;
        existing->rackAssignment = candidate->item.rackAssignment;
      }
      reconcileRackAssignment(importStagedStore_, *existing);
      acceptedId = existing->id;
      ++importMergedCount_;
    }
  }

  if (acceptedId.empty()) {
    captureUndoSnapshot();
    importStagedStore_.items().push_back(candidate->item);
    reconcileRackAssignment(importStagedStore_, importStagedStore_.items().back());
    acceptedId = importStagedStore_.items().back().id;
    ++importCreatedCount_;
  }

  importAcceptedItemIds_.push_back(acceptedId);
  importCandidates_.erase(importCandidates_.begin() + static_cast<ptrdiff_t>(importSelection_));
  if (importSelection_ >= importCandidates_.size() && !importCandidates_.empty()) {
    importSelection_ = importCandidates_.size() - 1;
  }

  if (importCandidates_.empty()) {
    finishImportReview();
  } else {
    setMessage("Accepted import row", 2);
  }
  dirty_ = true;
}

void App::skipImportCandidate() {
  if (importCandidates_.empty()) {
    finishImportReview();
    return;
  }

  importCandidates_.erase(importCandidates_.begin() + static_cast<ptrdiff_t>(importSelection_));
  ++importSkippedCount_;
  if (importSelection_ >= importCandidates_.size() && !importCandidates_.empty()) {
    importSelection_ = importCandidates_.size() - 1;
  }

  if (importCandidates_.empty()) {
    finishImportReview();
  } else {
    setMessage("Skipped import row", 2);
  }
  dirty_ = true;
}

void App::finishImportReview() {
  if (importStageActive_ && !commitImportStage()) return;
  if (importCommitPending_) return;
  if (importAcceptedItemIds_.empty()) {
    finishCsvImport(false);
    return;
  }
  importSyncPrompt_ = !importAcceptedItemIds_.empty();
  page_ = Page::Import;
  inputMode_ = InputMode::None;
  dirty_ = true;
}

void App::beginImportSync(bool retryFailed) {
  if (importSyncRunning_) {
    setMessage("DigiKey import sync is already running", 3);
    return;
  }

  const auto itemIds = retryFailed ? importSyncFailedItemIds_ : importAcceptedItemIds_;
  if (itemIds.empty()) {
    setMessage(retryFailed ? "There are no failed DigiKey rows to retry" : "No accepted rows need DigiKey sync", 3);
    return;
  }

  const auto api = createDigiKeyApi();
  importSyncTotal_ = itemIds.size();
  importSyncCompleted_ = 0;
  importSyncFailedCount_ = 0;
  importSyncFailedItemIds_.clear();
  importSyncCancelRequested_ = false;
  importSyncHasRun_ = false;

  vector<pair<string, string>> requests;
  for (const auto& itemId : itemIds) {
    const auto* item = store_.findById(itemId);
    if (item == nullptr) {
      importSyncFailedItemIds_.push_back(itemId);
      ++importSyncFailedCount_;
      continue;
    }
    const auto lookup = !trim(item->digikeyPartNumber).empty() ? item->digikeyPartNumber : item->sku;
    if (trim(lookup).empty()) {
      importSyncFailedItemIds_.push_back(itemId);
      ++importSyncFailedCount_;
      continue;
    }
    requests.emplace_back(itemId, lookup);
  }

  if (api.client == nullptr) {
    importSyncCompleted_ = importSyncTotal_;
    importSyncHasRun_ = true;
    importSyncPrompt_ = true;
    setMessage("DigiKey sync unavailable: " + api.error + "; press R to retry after configuring it", 6);
    return;
  }

  if (requests.empty()) {
    importSyncCompleted_ = importSyncTotal_;
    importSyncHasRun_ = true;
    importSyncPrompt_ = true;
    setMessage("No accepted rows had a DigiKey identifier; press Enter to finish", 5);
    return;
  }

  const auto config = loadDigiKeyConfig();
  const auto context = currentWorkspaceContext();
  if (context == nullptr) {
    setMessage("DigiKey sync unavailable while the workspace is changing", 5);
    return;
  }
  importSyncGeneration_ = context->generation;
  importSyncCancelFlag_ = make_shared<atomic<bool>>(false);
  const auto cancelFlag = importSyncCancelFlag_;
  const auto generation = importSyncGeneration_;
  importSyncFuture_ = async(launch::async, [requests = move(requests), config, cancelFlag, generation] {
    ImportSyncBatchResult batch;
    batch.workspaceGeneration = generation;
    DigiKeyApiClient client(config);
    for (size_t index = 0; index < requests.size(); ++index) {
      if (cancelFlag->load()) {
        for (size_t remaining = index; remaining < requests.size(); ++remaining) {
          batch.failedItemIds.push_back(requests[remaining].first);
        }
        break;
      }
      string error;
      auto details = client.fetchProductDetails(requests[index].second, &error);
      if (details) {
        batch.results.emplace_back(requests[index].first, move(details));
      } else {
        batch.failedItemIds.push_back(requests[index].first);
      }
    }
    return batch;
  });
  importSyncRunning_ = true;
  importSyncPrompt_ = false;
  setMessage("DigiKey sync is running in the background; the terminal remains available", 5);
  dirty_ = true;
}

void App::processImportSync() {
  if (!importSyncFuture_.valid() || importSyncFuture_.wait_for(chrono::seconds(0)) != future_status::ready) return;

  const auto batch = importSyncFuture_.get();
  if (!workspaceIsCurrent(batch.workspaceGeneration)) {
    importSyncRunning_ = false;
    importSyncHasRun_ = false;
    importSyncCancelFlag_.reset();
    importSyncPrompt_ = false;
    importSyncFailedItemIds_.clear();
    setMessage("DigiKey sync result discarded because the workspace changed", 5);
    dirty_ = true;
    return;
  }
  const bool cancelled = importSyncCancelRequested_;
  importSyncCompleted_ = min(importSyncTotal_, batch.results.size() + batch.failedItemIds.size() +
                                             importSyncFailedItemIds_.size());
  if (cancelled) {
    for (const auto& result : batch.results) importSyncFailedItemIds_.push_back(result.first);
    importSyncFailedItemIds_.insert(importSyncFailedItemIds_.end(), batch.failedItemIds.begin(), batch.failedItemIds.end());
    importSyncFailedCount_ += static_cast<int>(batch.results.size() + batch.failedItemIds.size());
  } else {
    for (const auto& result : batch.results) {
      if (auto* item = store_.findById(result.first); item != nullptr && result.second.has_value()) {
        mergeDigiKeyMetadata(*item, *result.second);
        reconcileRackAssignment(store_, *item);
        ++importSyncedCount_;
      } else {
        importSyncFailedItemIds_.push_back(result.first);
        ++importSyncFailedCount_;
      }
    }
    importSyncFailedItemIds_.insert(importSyncFailedItemIds_.end(), batch.failedItemIds.begin(), batch.failedItemIds.end());
    importSyncFailedCount_ += static_cast<int>(batch.failedItemIds.size());
  }

  importSyncRunning_ = false;
  importSyncHasRun_ = true;
  importSyncCancelFlag_.reset();
  if (!cancelled && !batch.results.empty() && !saveState("digikey", "import sync", "DigiKey enrichment batch")) {
    setMessage("DigiKey metadata is in memory; press R to retry saving", 6);
  } else {
    setMessage(cancelled ? "DigiKey sync cancelled; press R to retry failed rows"
                         : "DigiKey sync finished; press R to retry failed rows or Enter to finish",
               6);
  }
  importSyncPrompt_ = true;
  dirty_ = true;
}

void App::retryImportSync() {
  if (importSyncFailedItemIds_.empty()) {
    setMessage("There are no failed DigiKey rows to retry", 3);
    return;
  }
  beginImportSync(true);
}

void App::finishCsvImport(bool syncWithDigiKey) {
  if (syncWithDigiKey) {
    beginImportSync();
    return;
  }
  if (importSyncRunning_) {
    setMessage("Wait for DigiKey sync to finish or cancel it first", 4);
    return;
  }

  const auto summary = importCompletionMessage();
  logActivity("import", summary);
  importCandidates_.clear();
  importAcceptedItemIds_.clear();
  importSourcePath_.clear();
  importSelection_ = 0;
  importSyncPrompt_ = false;
  importSyncHasRun_ = false;
  importSyncFailedItemIds_.clear();
  importSyncTotal_ = 0;
  importSyncCompleted_ = 0;
  importSyncCancelRequested_ = false;
  editingImportCandidate_ = false;
  importStageActive_ = false;
  importCommitPending_ = false;
  importOriginalStore_ = {};
  importStagedStore_ = {};
  changePage(Page::Home);
  setMessage(summary, 8);
}

string App::importCompletionMessage() const {
  return "CSV import complete: " + to_string(importCreatedCount_) + " new, " +
         to_string(importMergedCount_) + " merged, " + to_string(importSkippedCount_) + " skipped, " +
         to_string(importSyncedCount_) + " synced, " + to_string(importSyncFailedCount_) + " sync failed";
}

}  // namespace inventatory
