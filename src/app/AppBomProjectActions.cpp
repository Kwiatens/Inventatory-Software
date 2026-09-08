// Inventatory - BOM project lifecycle, analysis, build, and export actions.

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
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace inventatory {

using namespace std;
using namespace app_actions;

BomProject* App::activeBomProject() {
  if (activeBomProjectId_.empty()) {
    return nullptr;
  }
  const auto it = find_if(bomProjects_.begin(), bomProjects_.end(),
                          [&](const BomProject& project) { return project.id == activeBomProjectId_; });
  return it == bomProjects_.end() ? nullptr : &*it;
}

const BomProject* App::activeBomProject() const {
  return const_cast<App*>(this)->activeBomProject();
}

bool App::saveBomProjects() {
  if (inventoryRecoveryRequired_) {
    persistenceError_ = inventoryRecoveryDetail_.empty()
                            ? "Inventory recovery is required before BOM projects can be saved."
                            : inventoryRecoveryDetail_ + ". BOM projects were not changed.";
    return false;
  }
  const bool saved = inventatory::saveBomProjects(inventoryPath_, bomProjects_);
  if (!saved) {
    bomProjectsDirty_ = true;
    persistenceError_ = "Could not save BOM projects; changes remain in memory.";
    setMessage(persistenceError_ + " Press R to retry.", 6);
  } else {
    bomProjectsDirty_ = false;
  }
  return saved;
}

void App::openBomProjects() {
  bomView_ = BomView::List;
  bomDeductPrompt_ = false;
  bomProjectSelection_ = bomProjects_.empty() ? 0 : min(bomProjectSelection_, bomProjects_.size() - 1);
  changePage(Page::Projects);
}

void App::beginBomProject(const string& bomText, const string& name, const filesystem::path& sourcePath) {
  bomFile_ = parseKicadBomText(bomText, name);
  if (!bomFile_.ok) {
    setMessage("BOM import failed: " + bomFile_.error, 6);
    return;
  }

  // A freshly imported project is held in memory until the user pins it, so a
  // one-off analysis never clutters the list.
  BomProject project;
  project.id = "bom-" + makeId().substr(0, 12);
  project.name = name;
  project.sourcePath = sourcePath.string();
  project.boards = 1;
  project.createdAt = time(nullptr);
  project.lastOpened = project.createdAt;
  project.bomText = bomText;

  // Replace an existing project built from the same file rather than stacking
  // duplicates every time the user re-imports after a schematic change.
  const auto existing = find_if(bomProjects_.begin(), bomProjects_.end(), [&](const BomProject& candidate) {
    return !candidate.sourcePath.empty() && candidate.sourcePath == project.sourcePath;
  });
  if (existing != bomProjects_.end()) {
    project.id = existing->id;
    project.boards = existing->boards;
    project.createdAt = existing->createdAt;
    project.lastBuilt = existing->lastBuilt;
    project.overrides = existing->overrides;
    project.enrichment = existing->enrichment;
    *existing = project;
  } else {
    bomProjects_.insert(bomProjects_.begin(), project);
  }

  activeBomProjectId_ = project.id;
  bomProjectSelection_ = 0;
  bomSplitSelection_ = 0;
  bomSplitShortFocused_ = false;
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomView_ = BomView::Split;
  inputMode_ = InputMode::None;
  refreshBomAnalysis();
  queueBomEnrichment();
  // Persist straight away: an imported BOM should survive a restart without the
  // user having to ask for it.
  const bool projectSaved = saveBomProjects();
  changePage(Page::Projects);

  string summary = to_string(bomAnalysis_.readyCount) + " ready · " + to_string(bomAnalysis_.shortCount) + " short";
  if (!projectSaved) summary += " · project changes unsaved; press R to retry";
  if (!bomFile_.warnings.empty()) {
    summary += " · " + to_string(bomFile_.warnings.size()) + " rows not orderable";
  }
  setMessage(summary, 6);
}

void App::refreshBomAnalysis() {
  auto* project = activeBomProject();
  if (project == nullptr || !bomFile_.ok) {
    bomAnalysisValid_ = false;
    return;
  }

  bomAnalysis_ = analyzeBom(bomFile_, store_.items(), project->boards, project->overrides);
  bomAnalysisValid_ = true;
  if (!bomAnalysis_.matches.empty()) {
    bomSplitSelection_ = min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
  } else {
    bomSplitSelection_ = 0;
  }
  dirty_ = true;
}

void App::openSelectedBomProject() {
  if (bomProjects_.empty()) {
    return;
  }
  auto& project = bomProjects_[min(bomProjectSelection_, bomProjects_.size() - 1)];
  bomFile_ = parseKicadBomText(project.bomText, project.name);
  if (!bomFile_.ok) {
    setMessage("Stored BOM could not be read: " + bomFile_.error, 6);
    return;
  }

  project.lastOpened = time(nullptr);
  activeBomProjectId_ = project.id;
  bomSplitSelection_ = 0;
  bomSplitShortFocused_ = false;
  bomBuildStep_ = 0;
  bomDeductPrompt_ = false;
  bomView_ = BomView::Split;
  refreshBomAnalysis();
  queueBomEnrichment();
  saveBomProjects();
}

void App::moveBomSelection(int delta) {
  if (bomView_ == BomView::List) {
    if (bomProjects_.empty()) {
      bomProjectSelection_ = 0;
      dirty_ = true;
      return;
    }
    const auto current = static_cast<int>(min(bomProjectSelection_, bomProjects_.size() - 1));
    bomProjectSelection_ =
        static_cast<size_t>(clamp(current + delta, 0, static_cast<int>(bomProjects_.size() - 1)));
  } else if (bomView_ == BomView::Split) {
    if (!bomAnalysisValid_ || bomAnalysis_.matches.empty()) {
      bomSplitSelection_ = 0;
      dirty_ = true;
      return;
    }
    vector<size_t> visibleIndices;
    for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
      if (bomAnalysis_.matches[index].sufficient != bomSplitShortFocused_) {
        visibleIndices.push_back(index);
      }
    }
    if (visibleIndices.empty()) {
      dirty_ = true;
      return;
    }
    const auto selected = find(visibleIndices.begin(), visibleIndices.end(), bomSplitSelection_);
    const int current = selected == visibleIndices.end() ? 0 : static_cast<int>(distance(visibleIndices.begin(), selected));
    const auto next = clamp(current + delta, 0, static_cast<int>(visibleIndices.size() - 1));
    bomSplitSelection_ = visibleIndices[static_cast<size_t>(next)];
  }
  dirty_ = true;
}

void App::adjustBomBoards(int delta) {
  auto* project = activeBomProject();
  if (project == nullptr) {
    return;
  }
  const auto boards = clamp(project->boards + delta, 1, 999);
  if (boards == project->boards) {
    return;
  }
  project->boards = boards;
  refreshBomAnalysis();
  const bool saved = saveBomProjects();
  setMessage(saved ? to_string(boards) + (boards == 1 ? " board" : " boards")
                  : "Board count changed in memory; press R to retry saving the project",
             saved ? 2 : 5);
}

void App::cycleBomAlternate() {
  auto* project = activeBomProject();
  if (project == nullptr || !bomAnalysisValid_ || bomAnalysis_.matches.empty()) {
    return;
  }

  auto& match = bomAnalysis_.matches[min(bomSplitSelection_, bomAnalysis_.matches.size() - 1)];
  if (match.candidates.size() < 2) {
    setMessage("No alternate match for this line", 2);
    return;
  }

  match.chosen = (match.chosen + 1) % match.candidates.size();
  project->overrides[bomLineKey(bomAnalysis_.lines[match.lineIndex])] = match.chosenItemId();
  recomputeBomTotals(bomAnalysis_, store_.items());
  const bool saved = saveBomProjects();

  const auto* item = store_.findById(match.chosenItemId());
  setMessage(saved ? "Matched to " + (item == nullptr ? string("unknown part") : item->partName) + "  (" +
                         to_string(match.chosen + 1) + "/" + to_string(match.candidates.size()) + ")"
                  : "Alternate match changed in memory; press R to retry saving the project",
             5);
}

void App::deleteSelectedBomProject() {
  if (bomProjects_.empty()) {
    return;
  }
  const auto index = min(bomProjectSelection_, bomProjects_.size() - 1);
  const auto selectedId = bomProjects_[index].id;
  const auto now = time(nullptr);
  if (bomDeleteConfirmationProjectId_ != selectedId || now > bomDeleteConfirmationUntil_) {
    bomDeleteConfirmationProjectId_ = selectedId;
    bomDeleteConfirmationUntil_ = now + 5;
    setMessage("Press d again within 5 seconds to forget " + bomProjects_[index].name, 5);
    dirty_ = true;
    return;
  }
  bomDeleteConfirmationProjectId_.clear();
  bomDeleteConfirmationUntil_ = 0;
  const auto name = bomProjects_[index].name;
  if (bomProjects_[index].id == activeBomProjectId_) {
    activeBomProjectId_.clear();
    bomAnalysisValid_ = false;
  }
  bomProjects_.erase(bomProjects_.begin() + static_cast<long>(index));
  bomProjectSelection_ = bomProjects_.empty() ? 0 : min(index, bomProjects_.size() - 1);
  if (!saveBomProjects()) {
    setMessage("Could not save project deletion; press R to retry", 5);
    return;
  }
  setMessage(name + " forgotten", 3);
}

void App::beginBomRestock() {
  if (!bomAnalysisValid_ || bomAnalysis_.matches.empty()) return;
  const auto index = min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
  const auto& match = bomAnalysis_.matches[index];
  if (match.sufficient) {
    setMessage("That BOM line is already covered", 2);
    return;
  }
  const auto itemId = match.chosenItemId();
  if (itemId.empty() || store_.findById(itemId) == nullptr) {
    setMessage("This line has no matched stock item; add it manually from Stock", 5);
    changePage(Page::Stock);
    beginEditCurrentItem(true);
    return;
  }
  bomRestockItemId_ = itemId;
  inputBuffer_ = to_string(max(1, match.needed - match.available));
  inputMode_ = InputMode::BomRestock;
  setMessage("Enter received quantity; default is the shortage", 4);
  dirty_ = true;
}

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
