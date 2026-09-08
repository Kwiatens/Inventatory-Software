// Inventatory - BOM build walkthrough and shortage export actions.

#include "App.h"
#include "app/common/AppActionSupport.h"

#include "core/storage/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <utility>

namespace inventatory {

using namespace std;
using namespace app_actions;

vector<App::BuildStep> App::bomBuildSteps() const {
  vector<BuildStep> steps;
  if (!bomAnalysisValid_) {
    return steps;
  }

  // Rack stops come first in rack order so the shelf is walked once, then a
  // single stop collects everything that lives outside a rack.
  map<string, BuildStep> rackSteps;
  BuildStep loose;
  loose.title = "NOT IN A RACK";

  for (const auto& match : bomAnalysis_.matches) {
    const auto itemId = match.chosenItemId();
    if (itemId.empty()) {
      const auto& line = bomAnalysis_.lines[match.lineIndex];
      BuildPick pick;
      pick.label = line.designation + " (add in Stock)";
      pick.detail = packageFromFootprint(line.footprint);
      pick.slot = "Add in Stock";
      pick.quantity = match.needed;
      loose.picks.push_back(move(pick));
      continue;
    }
    const auto* item = store_.findById(itemId);
    if (item == nullptr) {
      continue;
    }

    const auto& line = bomAnalysis_.lines[match.lineIndex];
    BuildPick pick;
    pick.itemId = itemId;
    pick.slot = item->rackSlot;
    pick.label = line.designation;
    pick.detail = packageFromFootprint(line.footprint);
    pick.quantity = match.needed;

    if (item->rackId.empty() || item->rackSlot.empty()) {
      pick.slot = item->location.empty() ? "Unfiled" : item->location;
      loose.picks.push_back(move(pick));
      continue;
    }

    auto& step = rackSteps[item->rackId];
    step.rackId = item->rackId;
    step.picks.push_back(move(pick));
  }

  vector<const InventatoryRack*> orderedRacks;
  for (const auto& rack : store_.racks()) {
    if (rackSteps.count(rack.id) != 0) {
      orderedRacks.push_back(&rack);
    }
  }
  sort(orderedRacks.begin(), orderedRacks.end(), [](const InventatoryRack* lhs, const InventatoryRack* rhs) {
    const auto left = rackNumberFromCode(lhs->code);
    const auto right = rackNumberFromCode(rhs->code);
    return left != right ? left < right : lhs->code < rhs->code;
  });

  for (const auto* rack : orderedRacks) {
    auto step = rackSteps[rack->id];
    step.title = "RACK " + to_string(max(1, rackNumberFromCode(rack->code)));
    step.subtitle = rack->componentType;
    sort(step.picks.begin(), step.picks.end(),
         [](const BuildPick& lhs, const BuildPick& rhs) { return lhs.slot < rhs.slot; });
    steps.push_back(move(step));
  }

  if (!loose.picks.empty()) {
    sort(loose.picks.begin(), loose.picks.end(),
         [](const BuildPick& lhs, const BuildPick& rhs) { return lhs.slot < rhs.slot; });
    steps.push_back(move(loose));
  }
  return steps;
}

void App::beginBomBuild() {
  if (!bomAnalysisValid_) {
    return;
  }
  if (bomBuildSteps().empty()) {
    setMessage("Nothing to pick: no BOM line matched a part in stock", 4);
    return;
  }
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomView_ = BomView::Build;
  dirty_ = true;
}

void App::advanceBomBuild(int delta) {
  const auto steps = bomBuildSteps();
  if (steps.empty()) {
    bomView_ = BomView::Split;
    dirty_ = true;
    return;
  }

  const auto next = static_cast<int>(bomBuildStep_) + delta;
  if (next < 0) {
    bomView_ = BomView::Split;
    bomBuildStep_ = 0;
    dirty_ = true;
    return;
  }
  if (next >= static_cast<int>(steps.size())) {
    if (!bomBuildReady(bomAnalysis_)) {
      setMessage("Build walkthrough viewed; completion is disabled while BOM shortages remain", 6);
      bomBuildStep_ = steps.size() - 1;
      dirty_ = true;
      return;
    }
    // Past the last stop the walkthrough asks its one and only question.
    bomDeductPrompt_ = true;
    dirty_ = true;
    return;
  }

  bomBuildStep_ = static_cast<size_t>(next);
  dirty_ = true;
}

void App::finishBomBuild(bool subtractFromStock) {
  if (!bomBuildReady(bomAnalysis_)) {
    bomDeductPrompt_ = false;
    setMessage("Build cannot be completed until every BOM line has enough stock", 6);
    return;
  }
  const auto steps = bomBuildSteps();
  int parts = 0;
  int pieces = 0;

  if (subtractFromStock) {
    captureUndoSnapshot();
    const auto now = time(nullptr);
    for (const auto& step : steps) {
      for (const auto& pick : step.picks) {
        auto* item = store_.findById(pick.itemId);
        if (item == nullptr) {
          continue;
        }
        item->quantity = max(0, item->quantity - pick.quantity);
        item->lastUpdated = now;
        reconcileRackAssignment(store_, *item);
        ++parts;
        pieces += pick.quantity;
      }
    }
    if (!saveState("bom_build", activeBomProjectId_)) {
      setMessage("Build stock changes are in memory; press R to retry saving", 6);
      return;
    }
  }

  auto* project = activeBomProject();
  const auto previousLastBuilt = project == nullptr ? time_t(0) : project->lastBuilt;
  if (project != nullptr) {
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
    logActivity("build", name + " · " + to_string(parts) + " parts · " + to_string(pieces) + " pieces · " +
                             to_string(bomAnalysis_.boards) + " boards");
  }

  bomDeductPrompt_ = false;
  bomBuildStep_ = 0;
  bomView_ = BomView::Split;
  refreshBomAnalysis();
  setMessage(subtractFromStock ? "Build complete · stock updated · Ctrl+Z undoes it"
                               : "Build complete · stock unchanged",
             6);
}

bool App::exportBomShortages() {
  if (!bomAnalysisValid_) {
    return false;
  }
  const auto* project = activeBomProject();
  if (project == nullptr) {
    return false;
  }

  filesystem::path target = project->sourcePath.empty()
                                ? dataPath_ / (project->name + " shortage.csv")
                                : filesystem::path(project->sourcePath).parent_path() /
                                      (filesystem::path(project->sourcePath).stem().string() + "-shortage.csv");

  ofstream output(target, ios::binary);
  if (!output) {
    setMessage("Unable to write " + target.filename().string(), 5);
    return false;
  }

  const auto quote = [](const string& value) {
    string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
      if (ch == '"') {
        escaped.push_back('"');
      }
      escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
  };

  output << "Designation,Footprint,Package,Designators,Needed,On hand,Suggested DigiKey part\r\n";
  size_t rows = 0;
  for (const auto& match : bomAnalysis_.matches) {
    if (match.sufficient) {
      continue;
    }
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const auto suggestion = project->enrichment.find(bomLineKey(line));
    output << quote(line.designation) << ',' << quote(line.footprint) << ','
           << quote(packageFromFootprint(line.footprint)) << ',' << quote(join(line.designators, ' ')) << ','
           << match.needed << ',' << match.available << ','
           << quote(suggestion == project->enrichment.end() ? string() : suggestion->second) << "\r\n";
    ++rows;
  }

  setMessage("Wrote " + to_string(rows) + " shortages to " + target.filename().string(), 6);
  return true;
}

}  // namespace inventatory
