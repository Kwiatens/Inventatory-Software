// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - BOM build walkthrough and shortage export actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "core/storage/AtomicFile.h"
#include "core/transfer/CsvExport.h"
#include "core/storage/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <map>
#include <utility>

namespace inventatory {

using namespace std;
using namespace app_actions;

BomPickPlan App::bomPickPlan() const {
  if (!bomAnalysisValid_) return {};
  return planBomPicks(bomAnalysis_, store_);
}

int App::bomPickTaken(const BomPick& pick) const {
  if (pick.commitIndex >= bomCommitTaken_.size()) return pick.quantity;
  return clamp(bomCommitTaken_[pick.commitIndex], 0, pick.quantity);
}

void App::adjustBomPickTaken(int delta) {
  if (!bomDeductPrompt_) return;
  const auto plan = bomPickPlan();
  vector<const BomPick*> picks;
  for (const auto& stop : plan.stops) {
    for (const auto& pick : stop.picks) picks.push_back(&pick);
  }
  if (picks.empty()) return;
  const auto& pick = *picks[min(bomFinishSelection_, picks.size() - 1)];
  if (pick.commitIndex >= bomCommitTaken_.size()) return;
  bomCommitTaken_[pick.commitIndex] = clamp(bomPickTaken(pick) + delta, 0, pick.quantity);
  dirty_ = true;
}

void App::beginBomBuild() {
  if (!bomAnalysisValid_) {
    return;
  }
  // Picks are frozen here; finishing deducts only these, and only if stock still matches.
  refreshBomAnalysis();
  const auto plan = bomPickPlan();
  if (plan.empty()) {
    setMessage("Nothing to pick yet. Every part of this project still needs ordering.", 4);
    return;
  }
  bomCommits_ = bomPickCommits(plan, bomAnalysis_, store_);
  bomCommitTaken_.clear();
  for (const auto& commit : bomCommits_) bomCommitTaken_.push_back(commit.quantity);
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomFinishSelection_ = 0;
  bomView_ = BomView::Build;
  dirty_ = true;
}

void App::advanceBomBuild(int delta) {
  const auto plan = bomPickPlan();
  if (plan.empty()) {
    bomView_ = BomView::Split;
    bomDeductPrompt_ = false;
    dirty_ = true;
    return;
  }

  if (bomDeductPrompt_) {
    if (delta < 0) {
      bomDeductPrompt_ = false;
      bomBuildStep_ = plan.stops.size() - 1;
      dirty_ = true;
    }
    return;
  }

  const auto next = static_cast<int>(bomBuildStep_) + delta;
  if (next < 0) {
    bomView_ = BomView::Split;
    bomBuildStep_ = 0;
    dirty_ = true;
    return;
  }
  if (next >= static_cast<int>(plan.stops.size())) {
    // Shortages do not block the finish; missing lines stay on the shopping list.
    bomDeductPrompt_ = true;
    bomFinishSelection_ = 0;
    dirty_ = true;
    return;
  }

  bomBuildStep_ = static_cast<size_t>(next);
  dirty_ = true;
}

void App::finishBomBuild(bool subtractFromStock) {
  // Refresh first: the check below must compare against live stock, not the last analysis.
  if (subtractFromStock) refreshBomAnalysis();
  const auto plan = bomPickPlan();
  int parts = 0;
  int pieces = 0;
  bool stockSaved = true;

  if (subtractFromStock) {
    // Deduct nothing unless every pick still matches: same item, unchanged, enough units.
    const auto abandon = [this](const string& reason) {
      bomDeductPrompt_ = false;
      bomBuildStep_ = 0;
      bomCommits_.clear();
      bomCommitTaken_.clear();
      bomView_ = BomView::Split;
      refreshBomAnalysis();
      setMessage(reason, 7, UiMessageSeverity::Error);
    };
    if (!sameBomPickCommits(bomPickCommits(plan, bomAnalysis_, store_), bomCommits_)) {
      abandon("Stock or matches changed since the walkthrough began. Nothing was taken out. Start the build again.");
      return;
    }
    const auto problems = bomPickProblems(bomCommits_, bomCommitTaken_, store_);
    if (!problems.empty()) {
      string reason = "Nothing was taken out. " + problems.front();
      if (problems.size() > 1) reason += " " + to_string(problems.size() - 1) + " more to check.";
      abandon(reason + " Start the build again.");
      return;
    }

    captureUndoSnapshot();
    const auto now = time(nullptr);
    for (size_t index = 0; index < bomCommits_.size(); ++index) {
      const int taken = bomCommitTaken_[index];
      auto* item = store_.findById(bomCommits_[index].itemId);
      if (item == nullptr || taken <= 0) {
        continue;
      }
      item->quantity -= taken;  // checked above: on hand covers the draw
      item->lastUpdated = now;
      reconcileRackAssignment(store_, *item);
      ++parts;
      pieces += taken;
    }
    // A failed save keeps the deduction in memory for retry; the walkthrough still closes below.
    stockSaved = saveState("bom_build", activeBomProjectId_);
  }

  // Only a build with every line covered counts as built; a partial kit leaves the project open.
  auto* project = activeBomProject();
  const bool complete = bomBuildReady(bomAnalysis_);
  bool projectSaved = true;
  if (project != nullptr && complete) {
    const auto previousLastBuilt = project->lastBuilt;
    project->lastBuilt = time(nullptr);
    if (!saveBomProjects()) {
      project->lastBuilt = previousLastBuilt;
      projectSaved = false;
    }
  }

  const auto name = project == nullptr ? string("Project") : project->name;
  if (subtractFromStock) {
    logActivity("build", name + (complete ? "" : " (partial)") + " · " + to_string(parts) + " parts · " +
                             to_string(pieces) + " pieces · " + to_string(bomAnalysis_.boards) + " boards");
  }

  // Always close, saved or not: an open walkthrough could deduct the same pieces again.
  const auto shortLines = plan.shortMatches.size();
  const auto unconfirmedLines = plan.unconfirmedMatches.size();
  bomDeductPrompt_ = false;
  bomBuildStep_ = 0;
  bomCommits_.clear();
  bomCommitTaken_.clear();
  bomView_ = BomView::Split;
  refreshBomAnalysis();
  if (!stockSaved) {
    setMessage("Build stock changes are in memory; press R to retry saving", 7, UiMessageSeverity::Error);
    return;
  }
  if (!projectSaved) {
    setMessage("Build stock was saved, but the project timestamp was not; press R to retry", 7);
    return;
  }
  string message = subtractFromStock ? voiceCount(pieces, "piece", "pieces") + " taken out of stock. Ctrl+Z puts them back."
                                     : string("Stock was left as it was.");
  if (shortLines > 0) {
    message += " " + voiceCount(static_cast<int>(shortLines), "line is", "lines are") + " still on the shopping list.";
  }
  if (unconfirmedLines > 0) {
    message += " " + voiceCount(static_cast<int>(unconfirmedLines), "line", "lines") +
               " waiting for a confirmed part; they were not taken.";
  }
  setMessage(message, 6);
}

bool App::exportBomShortages() {
  if (!bomAnalysisValid_) {
    return false;
  }
  const auto* project = activeBomProject();
  if (project == nullptr) {
    return false;
  }

  filesystem::path target = dataPath_ / "Inventatory-BOM-shortages.csv";
  const string filter = string("CSV files (*.csv)") + '\0' + "*.csv" + '\0' +
                        "All files (*.*)" + '\0' + "*.*" + '\0';
  if (!saveFileDialog(target, "Export BOM shortages", filter, "csv")) {
    setMessage("Shortage export cancelled", 2);
    return false;
  }

  // Write atomically so a full disk never leaves a truncated export or reports success.
  const auto exported = buildBomShortageCsv(bomAnalysis_, project->enrichment);
  string writeError;
  if (!writeFileAtomically(target, exported.text, &writeError)) {
    setMessage("Unable to write " + target.filename().u8string() + (writeError.empty() ? string() : ": " + writeError), 5,
               UiMessageSeverity::Error);
    return false;
  }

  setMessage("Wrote " + to_string(exported.rows) + " shortages to " + target.filename().u8string(), 6);
  return true;
}

}  // namespace inventatory
