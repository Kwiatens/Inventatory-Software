// Inventatory - Hardware Inventory Management System
// BOM project, enrichment, and build actions.

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

// ---------------------------------------------------------------------------
// KiCad BOM projects
// ---------------------------------------------------------------------------

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
  // Silently skipped without credentials, so an offline user never sees an
  // error they cannot act on.
  if (!loadDigiKeyConfig().valid()) {
    return;
  }
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
    if (project->enrichment.count(key) != 0) {
      continue;  // cached with the pinned project
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
        targetProject->enrichment[result.key] = result.suggestion;
        dirty_ = true;
      }
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
    const auto config = loadDigiKeyConfig();
    if (!config.valid()) {
      bomEnrichmentQueue_.clear();
      bomEnrichmentTotal_ = 0;
      bomEnrichmentActiveKey_.clear();
      return;
    }
    bomEnrichmentClient_ = make_unique<DigiKeyApiClient>(config);
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
  auto* client = bomEnrichmentClient_.get();
  const auto generation = bomEnrichmentGeneration_;
  const auto projectId = project->id;
  const auto requestSequence = bomEnrichmentSequence_;
  bomEnrichmentFuture_ = async(launch::async, [client, key, keywords, projectId, generation, requestSequence] {
    string error;
    if (const auto details = client->fetchProductDetails(keywords, &error)) {
      const auto suggestion =
          details->manufacturerPartNumber.empty() ? details->lookupKey : details->manufacturerPartNumber;
      return BomEnrichmentResult{key, suggestion.empty() ? string("-") : suggestion, projectId, generation,
                                 requestSequence};
    }
    return BomEnrichmentResult{key, string("-"), projectId, generation, requestSequence};
  });
  dirty_ = true;
}

void App::openCurrentUrl(const string& url, const string& label) {
  if (trim(url).empty()) {
    setMessage("No " + label + " link stored for this item", 3);
    return;
  }
  if (openUrl(url)) {
    setMessage("Opened " + label + " link", 2);
  } else {
    setMessage("Unable to open " + label + " link", 3);
  }
}

string App::fieldLabel(EditField field) const {
  switch (field) {
    case EditField::PartName:
      return "Part name";
    case EditField::Manufacturer:
      return "Manufacturer";
    case EditField::Category:
      return "Category";
    case EditField::Quantity:
      return "Quantity";
    case EditField::ReorderThreshold:
      return "Reorder threshold";
    case EditField::Location:
      return "Location";
    case EditField::Tags:
      return "Tags";
    case EditField::Parameters:
      return "Parameters";
    case EditField::Notes:
      return "Notes";
    case EditField::LabelOverride:
      return "Label override";
    case EditField::DigiKeyPart:
      return "DigiKey part";
    case EditField::DatasheetUrl:
      return "Datasheet URL";
    case EditField::ProductUrl:
      return "Product URL";
    case EditField::Sku:
      return "SKU";
    case EditField::RackLocation:
      return "Rack location";
  }
  return "Field";
}

string App::currentFieldValue(EditField field) const {
  const auto* item = workingCopy_.item.id.empty() && !workingCopy_.isNew ? selectedItem() : &workingCopy_.item;
  if (item == nullptr) {
    return {};
  }

  switch (field) {
    case EditField::PartName:
      return item->partName;
    case EditField::Manufacturer:
      return item->manufacturer;
    case EditField::Category:
      return item->category;
    case EditField::Quantity:
      return to_string(item->quantity);
    case EditField::ReorderThreshold:
      return to_string(item->reorderThreshold);
    case EditField::Location:
      return item->location;
    case EditField::Tags:
      return join(item->tags, ',');
    case EditField::Parameters: {
      ostringstream out;
      for (size_t index = 0; index < item->parameters.size(); ++index) {
        if (index > 0) {
          out << "; ";
        }
        out << item->parameters[index].name << '=' << item->parameters[index].value;
      }
      return out.str();
    }
    case EditField::Notes:
      return item->notes;
    case EditField::LabelOverride:
      return item->labelOverride;
    case EditField::DigiKeyPart:
      return item->digikeyPartNumber;
    case EditField::DatasheetUrl:
      return item->datasheetUrl;
    case EditField::ProductUrl:
      return item->productUrl;
    case EditField::Sku:
      return item->sku;
    case EditField::RackLocation: {
      const auto location = rackLocation(*item, store_.racks());
      return location.empty() ? (item->rackAssignment == RackAssignmentMode::Automatic ? "AUTO" : "") : location;
    }
  }

  return {};
}

vector<App::FieldOption> App::fieldOptions() const {
  return {
      {"Part name", EditField::PartName},
      {"Manufacturer", EditField::Manufacturer},
      {"Category", EditField::Category},
      {"Quantity", EditField::Quantity},
      {"Reorder threshold", EditField::ReorderThreshold},
      {"Location", EditField::Location},
      {"Rack location", EditField::RackLocation},
      {"Tags", EditField::Tags},
      {"Parameters", EditField::Parameters},
      {"Notes", EditField::Notes},
      {"Label override", EditField::LabelOverride},
      {"DigiKey part", EditField::DigiKeyPart},
      {"Datasheet URL", EditField::DatasheetUrl},
      {"Product URL", EditField::ProductUrl},
      {"SKU", EditField::Sku},
  };
}

string App::softwareVersion() const {
#ifdef Inventatory_VERSION_STRING
  return Inventatory_VERSION_STRING;
#else
  return "dev";
#endif
}

string App::itemDetailText(const InventoryItem& item, int width) const {
  ostringstream out;
  const auto fields = stockPreviewFields(item, rackLocation(item, store_.racks()));
  for (const auto& field : fields) {
    const auto line = field.label + field.value;
    for (const auto& wrapped : wrapText(line, width)) {
      out << wrapped << '\n';
    }
  }

  return out.str();
}

string App::summaryLine() const {
  const auto summary = summarize(store_.items(), settings_.lowStockThreshold);
  ostringstream out;
  out << summary.itemCount << " items"
      << " | " << summary.totalUnits << " units"
      << " | " << summary.lowStockCount << " low"
      << " | " << summary.missingMetadataCount << " missing metadata"
      << " | " << summary.unsyncedCount << " unsynced";
  return out.str();
}

string App::activePrompt() const {
  if (inputMode_ == InputMode::EditValue && fieldMenuIndex_ >= 0 && fieldMenuIndex_ < static_cast<int>(menuOptions_.size())) {
    return fieldLabel(menuOptions_[fieldMenuIndex_].field) + ": ";
  }
  if (inputMode_ == InputMode::RackRename) return "Rename rack to: ";
  if (inputMode_ == InputMode::RackType) return "Rack type: ";
  if (inputMode_ == InputMode::RackCreate) return "New rack type: ";
  if (inputMode_ == InputMode::RackJump) return "Jump to rack: ";
  if (inputMode_ == InputMode::RackFilter) return "Rack filter: ";
  if (inputMode_ == InputMode::QuantityAdjust) return "Quantity on hand: ";
  if (inputMode_ == InputMode::StocktakeCount) return "Physical count: ";
  if (inputMode_ == InputMode::HistoryCheckpoint) return "Checkpoint name: ";
  if (inputMode_ == InputMode::HistoryConfirm) return historyConfirmationMessage_;
  if (inputMode_ == InputMode::ExitConfirmation) return "S save  ·  D discard  ·  Esc cancel";
  return "";
}


}  // namespace inventatory
