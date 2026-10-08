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
  const auto found = bomPickTaken_.find(pick.matchIndex);
  return found == bomPickTaken_.end() ? pick.quantity : clamp(found->second, 0, pick.quantity);
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
  bomPickTaken_[pick.matchIndex] = clamp(bomPickTaken(pick) + delta, 0, pick.quantity);
  dirty_ = true;
}

void App::beginBomBuild() {
  if (!bomAnalysisValid_) {
    return;
  }
  if (bomPickPlan().empty()) {
    setMessage("Nothing to pick yet. Every part of this project still needs ordering.", 4);
    return;
  }
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomPickTaken_.clear();
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
    // Back from the finish screen returns to the last stop.
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
    // Past the last stop the walkthrough asks its one question. Shortages do not block it: whatever
    // was taken can still be deducted, and the missing lines stay on the shopping list.
    bomDeductPrompt_ = true;
    bomFinishSelection_ = 0;
    dirty_ = true;
    return;
  }

  bomBuildStep_ = static_cast<size_t>(next);
  dirty_ = true;
}

void App::finishBomBuild(bool subtractFromStock) {
  const auto plan = bomPickPlan();
  int parts = 0;
  int pieces = 0;

  if (subtractFromStock) {
    captureUndoSnapshot();
    const auto now = time(nullptr);
    for (const auto& stop : plan.stops) {
      for (const auto& pick : stop.picks) {
        auto* item = store_.findById(pick.itemId);
        const int taken = bomPickTaken(pick);
        if (item == nullptr || taken <= 0) {
          continue;
        }
        item->quantity = max(0, item->quantity - taken);
        item->lastUpdated = now;
        reconcileRackAssignment(store_, *item);
        ++parts;
        pieces += taken;
      }
    }
    if (!saveState("bom_build", activeBomProjectId_)) {
      setMessage("Build stock changes are in memory; press R to retry saving", 6);
      return;
    }
  }

  // Only a build with every line covered counts as built; a partial kit leaves the project open.
  auto* project = activeBomProject();
  const bool complete = bomBuildReady(bomAnalysis_);
  if (project != nullptr && complete) {
    const auto previousLastBuilt = project->lastBuilt;
    project->lastBuilt = time(nullptr);
    if (!saveBomProjects()) {
      project->lastBuilt = previousLastBuilt;
      bomDeductPrompt_ = false;
      setMessage("Build stock was saved, but the project timestamp was not; press R to retry", 7);
      return;
    }
  }

  const auto name = project == nullptr ? string("Project") : project->name;
  if (subtractFromStock) {
    logActivity("build", name + (complete ? "" : " (partial)") + " · " + to_string(parts) + " parts · " +
                             to_string(pieces) + " pieces · " + to_string(bomAnalysis_.boards) + " boards");
  }

  const auto shortLines = plan.shortMatches.size();
  bomDeductPrompt_ = false;
  bomBuildStep_ = 0;
  bomPickTaken_.clear();
  bomView_ = BomView::Split;
  refreshBomAnalysis();
  string message = subtractFromStock ? voiceCount(pieces, "part", "parts") + " taken out of stock. Ctrl+Z puts them back."
                                     : string("Stock was left as it was.");
  if (shortLines > 0) {
    message += " " + voiceCount(static_cast<int>(shortLines), "line is", "lines are") + " still on the shopping list.";
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

  // Build the whole file first and replace the target atomically: a full disk or a removed drive must
  // neither leave a truncated export behind nor report success.
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
