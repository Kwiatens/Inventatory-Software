// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// The project page: its Overview, the notice before Find in racks, the four steps and the full table.

#include "App.h"

#include "core/inventory/VendorProvider.h"
#include "core/vendor/VendorPurchase.h"
#include "ui/pages/bom/BomPageShared.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;
using namespace bom_page;

namespace {

constexpr int kGutter = 15;

string lineName(const BomLine& line) { return trim(line.designation + "  " + packageFromFootprint(line.footprint)); }

// "C12 10uF 0805", "R1 +3 10k 0603": the first designator says where on the board.
string lineLabel(const BomLine& line) {
  string label;
  if (!line.designators.empty()) {
    label = line.designators.front();
    if (line.designators.size() > 1) label += " +" + to_string(line.designators.size() - 1);
    label += "  ";
  }
  return label + lineName(line);
}

string itemPackage(const InventoryItem& item) {
  for (const auto& parameter : item.parameters) {
    if (toLower(parameter.name).find("package") != string::npos) return trim(parameter.value);
  }
  return {};
}

string checkReasonText(BomCheckReason reason) {
  switch (reason) {
    case BomCheckReason::Ambiguous: return "Several stock parts fit this line.";
    case BomCheckReason::IdentityNotInStock: return "The BOM names a part number that no stock part carries.";
    case BomCheckReason::QuantityConflict: return "The BOM's quantity and its designators disagree.";
    case BomCheckReason::Unverified: return "The package is unknown, or differs from the BOM's.";
    case BomCheckReason::OverrideLost: return "The part confirmed before has changed or is gone.";
    case BomCheckReason::None: break;
  }
  return {};
}

// A muted name in the gutter column, then the row's content.
ftxui::Element gutterRow(const string& label, ftxui::Element content) {
  return ftxui::hbox({ftxui::text(" "), cell(label, kGutter - 1, uiMutedText()), move(content), ftxui::filler()});
}

// A full-width group row: the name in its state colour, then a muted count.
ftxui::Element groupRow(const string& name, ftxui::Color color, int count) {
  return ftxui::hbox({ftxui::text(" "), uiHeaderText(name, color), styledText("  " + to_string(count), uiMutedText()),
                      ftxui::filler()}) |
         ftxui::bgcolor(uiRaisedSurfaceBg());
}

ftxui::Element selectableRow(ftxui::Elements cells, bool selected) {
  cells.insert(cells.begin(), styledText(selected ? " > " : "   ", uiFocusColor()));
  cells.push_back(ftxui::filler());
  auto row = ftxui::hbox(move(cells)) | ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
  return selected ? row | ftxui::select : row;
}

string partCount(int count) { return voiceCount(count, "part", "parts"); }

}  // namespace

ftxui::Element App::renderBomOverview(int screenWidth) const {
  auto self = const_cast<App*>(this);
  const auto* project = activeBomProject();
  const auto& progress = bomProgress_;
  auto buttons = buttonGroup({
      target(uiButton("Find in racks", "f", UiButtonKind::Primary), "bom.find-in-racks", UiTargetKind::Button,
             [self] { self->requestBomFindInRacks(); }),
      target(uiButton("All lines", "t"), "bom.lines", UiTargetKind::Button, [self] {
        self->bomView_ = BomView::Lines;
        self->dirty_ = true;
      }),
  });
  auto header = uiPageHeader(uiHeaderText(project == nullptr ? string("Project") : project->name, uiPrimaryText()),
                             ftxui::text(""), move(buttons));

  ftxui::Elements body;
  body.push_back(ftxui::hbox({ftxui::text(" "), cell("Boards", kGutter - 1, uiMutedText()),
                              uiHeaderText(to_string(bomAnalysis_.boards), uiPrimaryText()), ftxui::filler(),
                              target(uiButton("-"), "bom.boards.less", UiTargetKind::Button, [self] { self->adjustBomBoards(-1); }),
                              ftxui::text(" "),
                              target(uiButton("+"), "bom.boards.more", UiTargetKind::Button, [self] { self->adjustBomBoards(1); }),
                              ftxui::text(" ")}));
  body.push_back(ftxui::text(" "));

  // In stock: how many, then where, so the user knows what Find in racks will lead to.
  const int countWidth = 21;
  const int detailWidth = max(10, screenWidth - kGutter - countWidth - 2);
  const auto joinedNames = [](const vector<string>& names) {
    string text;
    for (const auto& name : names) text += (text.empty() ? "" : ", ") + name;
    return text;
  };
  body.push_back(gutterRow("In stock", uiHeaderText(progress.inStock == progress.lines && progress.lines > 0
                                                         ? "All " + partCount(progress.lines)
                                                         : to_string(progress.inStock) + " of " + partCount(progress.lines),
                                                     progress.inStock == progress.lines ? uiSuccessColor() : uiPrimaryText())));
  const auto placeRow = [&](int count, const string& what, const string& detail) {
    if (count == 0) return;
    body.push_back(gutterRow("", ftxui::hbox({cell(to_string(count) + " " + what, countWidth, uiSecondaryText()),
                                              cell(detail, detailWidth, uiMutedText())})));
  };
  placeRow(progress.inRacks, "in racks", "");
  placeRow(progress.otherPlaces, progress.otherPlaces == 1 ? "in another place" : "in other places", joinedNames(progress.places));
  placeRow(progress.noLocation, "with no location", joinedNames(progress.noLocationParts));
  body.push_back(ftxui::text(" "));

  body.push_back(ftxui::hbox({ftxui::text(" "), cell("To sort out", kGutter - 1, uiMutedText()), ftxui::filler()}));
  if (progress.lines > 0 && progress.notCovered() == 0) {
    body.push_back(ftxui::hbox({ftxui::text("   "), styledText("Nothing to sort out. Every part is in stock.", uiSuccessColor()),
                                ftxui::filler()}));
  } else {
    const auto steps = bomProjectSteps(progress, bomOrderCost_, bomOrderStores_);
    // The count sits right after the longest sentence, not at the far edge of the screen.
    auto nameColumn = tableColumn(12), sentenceColumn = tableColumn(10), countColumnFit = tableColumn(5);
    for (const auto& step : steps) {
      nameColumn.fit(step.name);
      sentenceColumn.fit(step.sentence);
      countColumnFit.fit(step.count);
    }
    const auto widths = fitColumns({nameColumn, sentenceColumn, countColumnFit}, screenWidth - 5);
    const int nameWidth = widths[0];
    const int sentenceWidth = widths[1];
    const int countColumn = widths[2];
    for (size_t index = 0; index < steps.size(); ++index) {
      const auto& step = steps[index];
      const bool selected = index == min<size_t>(bomOverviewSelection_, steps.size() - 1);
      const auto countColor = step.open ? uiWarnColor() : step.count == "Done" ? uiSuccessColor() : uiMutedText();
      auto row = selectableRow({tableCell(step.name, nameWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                                tableCell(step.sentence, sentenceWidth, step.open ? uiSecondaryText() : uiMutedText()),
                                tableCell(step.count, countColumn, countColor)},
                               selected);
      body.push_back(target(move(row), "bom.step." + to_string(index), UiTargetKind::Row, [self, index] {
        self->bomOverviewSelection_ = index;
        self->openBomStep(index);
      }));
    }
  }
  return ftxui::vbox({move(header), ftxui::vbox(move(body)) | ftxui::yframe | ftxui::flex});
}

ftxui::Element App::renderBomStartNotice(int screenWidth) const {
  auto self = const_cast<App*>(this);
  const auto* project = activeBomProject();
  const auto& progress = bomProgress_;
  auto buttons = buttonGroup({
      target(uiButton("Get them first", "s"), "bom.start.sort", UiTargetKind::Button, [self] { self->openFirstOpenBomStep(); }),
      target(uiButton("Start anyway", "Enter", UiButtonKind::Primary), "bom.start.anyway", UiTargetKind::Button, [self] {
        self->beginBomBuild();
        if (self->bomView_ != BomView::Build) self->openBomOverview();
      }),
  });
  const auto missing = progress.notCovered();
  auto header = uiPageHeader(
      uiHeaderText((project == nullptr ? string("Project") : project->name) + ": find in racks", uiPrimaryText()),
      styledText(to_string(missing) + " of " + partCount(progress.lines) + (missing == 1 ? " is" : " are") +
                     " not sorted out yet. Find in racks will skip " + (missing == 1 ? "it." : "them."),
                 uiWarnColor()),
      move(buttons));

  // One row per part left out, with where it is on the way to stock.
  struct Row {
    string where;
    string value;
    string package;
    string state;
    ftxui::Color color;
  };
  vector<Row> rows;
  for (const auto& match : bomAnalysis_.matches) {
    if (match.sufficient) continue;
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    string where;
    for (const auto& designator : line.designators) where += (where.empty() ? "" : " ") + designator;
    Row row{where, line.designation, packageFromFootprint(line.footprint), "Not in stock", uiWarnColor()};
    switch (project == nullptr ? BomLineState::ToChoose : bomLineState(match, bomLineKey(line), project->enrichment)) {
      case BomLineState::ToConfirm: row.state = "To confirm"; row.color = uiLinkColor(); break;
      case BomLineState::Chosen: row.state = "Chosen, not ordered"; row.color = uiSecondaryText(); break;
      case BomLineState::Ordered: row.state = "Ordered"; row.color = uiSuccessColor(); break;
      case BomLineState::InStock:
      case BomLineState::ToChoose: break;
    }
    rows.push_back(move(row));
  }

  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 40;
  // Tab bar, header, column names, the closing sentence and the status bar take the rest.
  const size_t shown = min(rows.size(), static_cast<size_t>(max(4, screenHeight - 15)));
  auto valueColumn = tableColumn(14), packageColumn = tableColumn(8), stateColumn = tableColumn(21), whereColumn = tableColumn(12);
  valueColumn.fit("Part");
  packageColumn.fit("Package");
  stateColumn.fit("Now");
  whereColumn.fit("On the board");
  for (size_t index = 0; index < shown; ++index) {
    valueColumn.fit(asciiVendorText(rows[index].value));
    packageColumn.fit(rows[index].package);
    stateColumn.fit(rows[index].state);
    whereColumn.fit(rows[index].where);
  }
  const auto widths = fitColumns({valueColumn, packageColumn, stateColumn, whereColumn}, screenWidth - 5);
  const int valueWidth = widths[0];
  const int packageWidth = widths[1];
  const int stateWidth = widths[2];
  const int whereWidth = widths[3];
  ftxui::Elements body;
  body.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Part", valueWidth, uiMutedText()),
                              tableCell("Package", packageWidth, uiMutedText()), tableCell("Now", stateWidth, uiMutedText()),
                              tableCell("On the board", whereWidth, uiMutedText()), ftxui::filler()}));
  for (size_t index = 0; index < shown; ++index) {
    const auto& row = rows[index];
    body.push_back(ftxui::hbox({ftxui::text("   "), tableCell(asciiVendorText(row.value), valueWidth, uiPrimaryText()),
                                tableCell(row.package, packageWidth, uiSecondaryText()), tableCell(row.state, stateWidth, row.color),
                                tableCell(row.where, whereWidth, uiSecondaryText()), ftxui::filler()}));
  }
  if (shown < rows.size()) {
    body.push_back(ftxui::hbox({ftxui::text("   "),
                                styledText("and " + to_string(rows.size() - shown) + " more. All lines shows every part.", uiMutedText()),
                                ftxui::filler()}));
  }
  body.push_back(ftxui::text(" "));

  vector<string> help;
  if (progress.toConfirm > 0) help.push_back("confirm " + to_string(progress.toConfirm) + " that may be in stock");
  if (progress.toChoose + progress.chosen > 0) help.push_back("choose and order " + to_string(progress.toChoose + progress.chosen));
  if (progress.ordered > 0) help.push_back("receive " + to_string(progress.ordered) + " ordered");
  string sentence = "The system can help: ";
  for (size_t index = 0; index < help.size(); ++index) {
    sentence += (index == 0 ? "" : index + 1 == help.size() ? ", and " : ", ") + help[index];
  }
  if (!help.empty()) {
    body.push_back(ftxui::hbox({ftxui::text(" "), styledText(sentence + ". Press s to start.", uiSecondaryText()), ftxui::filler()}));
  }
  return ftxui::vbox({move(header), ftxui::vbox(move(body)) | ftxui::flex});
}

ftxui::Element App::renderBomConfirm(int screenWidth) const {
  auto self = const_cast<App*>(this);
  const auto index = bomStepPosition_ < bomStepKeys_.size() ? bomMatchIndex(bomStepKeys_[bomStepPosition_]) : string::npos;
  if (index == string::npos) return ftxui::text("");
  const auto& match = bomAnalysis_.matches[index];
  const auto& line = bomAnalysis_.lines[match.lineIndex];
  auto buttons = buttonGroup({
      target(uiButton("Use this part", "Enter", UiButtonKind::Primary), "bom.confirm.use", UiTargetKind::Button,
             [self] { self->useBomConfirmCandidate(); }),
      target(uiButton("None of these", "n"), "bom.confirm.none", UiTargetKind::Button, [self] { self->markBomLineNotInStock(); }),
      target(uiButton("Skip", "s"), "bom.confirm.skip", UiTargetKind::Button, [self] { self->advanceBomStep(false); }),
  });
  const string who = line.designators.empty() ? lineName(line) : line.designators.front();
  auto header = uiPageHeader(uiHeaderText("Confirm matches", uiPrimaryText()),
                             styledText("Part " + to_string(bomStepPosition_ + 1) + " of " + to_string(bomStepKeys_.size()) +
                                            ". Is one of these in stock the right part for " + who + "?",
                                        uiMutedText()),
                             move(buttons));

  auto nameColumn = tableColumn(16), packageColumn = tableColumn(8), whereColumn = tableColumn(10), haveColumn = tableColumn(5);
  nameColumn.fit("Stock part");
  packageColumn.fit("Package");
  whereColumn.fit("Where");
  haveColumn.fit("Have");
  for (const auto& candidate : match.candidates) {
    const auto* item = store_.findById(candidate.itemId);
    if (item == nullptr) continue;
    const auto place = bomItemPlace(*item, store_.racks());
    nameColumn.fit(item->partName);
    packageColumn.fit(itemPackage(*item));
    whereColumn.fit(place.empty() ? string("No location") : place);
    haveColumn.fit(to_string(item->quantity));
  }
  const auto widths = fitColumns({nameColumn, packageColumn, whereColumn, haveColumn}, screenWidth - 5);
  const int nameWidth = widths[0];
  const int packageWidth = widths[1];
  const int whereWidth = widths[2];
  const int haveWidth = widths[3];
  ftxui::Elements body;
  body.push_back(gutterRow("BOM line", ftxui::hbox({uiHeaderText(lineLabel(line), uiPrimaryText()),
                                                    styledText("   need " + to_string(match.needed), uiSecondaryText())})));
  body.push_back(ftxui::text(" "));
  body.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Stock part", nameWidth, uiMutedText()),
                              tableCell("Package", packageWidth, uiMutedText()), tableCell("Where", whereWidth, uiMutedText()),
                              tableCell("Have", haveWidth, uiMutedText(), false, true), ftxui::filler()}));
  for (size_t candidateIndex = 0; candidateIndex < match.candidates.size(); ++candidateIndex) {
    const auto* item = store_.findById(match.candidates[candidateIndex].itemId);
    if (item == nullptr) continue;
    const bool selected = candidateIndex == bomConfirmSelection_;
    const auto place = bomItemPlace(*item, store_.racks());
    auto row = selectableRow({tableCell(item->partName, nameWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                              tableCell(itemPackage(*item), packageWidth, uiSecondaryText()),
                              tableCell(place.empty() ? string("No location") : place, whereWidth,
                                        place.empty() ? uiMutedText() : uiLinkColor()),
                              tableCell(to_string(item->quantity), haveWidth, uiPrimaryText(), false, true)},
                             selected);
    body.push_back(target(move(row), "bom.confirm.row." + to_string(candidateIndex), UiTargetKind::Row, [self, candidateIndex] {
      self->bomConfirmSelection_ = candidateIndex;
      self->dirty_ = true;
    }));
  }
  if (const auto reason = checkReasonText(match.check); !reason.empty()) {
    body.push_back(ftxui::text(" "));
    body.push_back(ftxui::hbox({ftxui::text(" "), styledText(reason, uiWarnColor()), ftxui::filler()}));
  }
  return ftxui::vbox({move(header), ftxui::vbox(move(body)) | ftxui::yframe | ftxui::flex | ftxui::reflect(bomTableBounds_)});
}

ftxui::Element App::renderBomChoose(int screenWidth) const {
  auto self = const_cast<App*>(this);
  const auto index = bomStepPosition_ < bomStepKeys_.size() ? bomMatchIndex(bomStepKeys_[bomStepPosition_]) : string::npos;
  if (index == string::npos) return ftxui::text("");
  const auto& match = bomAnalysis_.matches[index];
  const auto& line = bomAnalysis_.lines[match.lineIndex];
  const auto* project = activeBomProject();
  const auto found = project == nullptr ? map<string, string>::const_iterator{} : project->enrichment.find(bomPicker_.lineKey);
  const string chosenPart = project != nullptr && found != project->enrichment.end() && bomEnrichmentChosen(found->second)
                                ? bomEnrichmentPart(found->second)
                                : string();
  const bool keepOnly = bomPicker_.entries.empty() && !chosenPart.empty() && !bomPickerFuture_.valid();
  const string storeName = vendorDisplayName(bomPicker_.store);

  ftxui::Elements buttonList;
  if (!bomPicker_.entries.empty()) {
    buttonList.push_back(target(uiButton("Use this part", "Enter", UiButtonKind::Primary), "bom.choose.use", UiTargetKind::Button,
                                [self] { self->useBomPickerSelection(); }));
  } else if (keepOnly) {
    buttonList.push_back(target(uiButton("Keep it", "Enter", UiButtonKind::Primary), "bom.choose.keep", UiTargetKind::Button,
                                [self] { self->advanceBomStep(true); }));
  }
  buttonList.push_back(target(uiButton("Open in store", "l"), "bom.choose.page", UiTargetKind::Button, [self] { self->openBomPartPage(); }));
  buttonList.push_back(target(uiButton("Skip", "s"), "bom.choose.skip", UiTargetKind::Button, [self] { self->advanceBomStep(false); }));
  auto header = uiPageHeader(uiHeaderText("Choose parts to buy", uiPrimaryText()),
                             styledText("Part " + to_string(bomStepPosition_ + 1) + " of " + to_string(bomStepKeys_.size()) + ". " +
                                            lineLabel(line) + ", need " + to_string(bomPicker_.quantity) + ".",
                                        uiMutedText()),
                             buttonGroup(move(buttonList)));

  ftxui::Elements body;
  // Each button sits beside the value it changes.
  const int valueWidth = max(20, min(60, screenWidth - kGutter - 30));
  body.push_back(ftxui::hbox({ftxui::text(" "), cell("Store", kGutter - 1, uiMutedText()), cell(storeName, valueWidth, uiPrimaryText(), true),
                              ftxui::text("  "),
                              target(uiButton("Change store", "v"), "bom.choose.store", UiTargetKind::Button, [self] { self->cycleBomStore(); }),
                              ftxui::filler()}));
  const bool searchable = bomPickerSearchable(bomPicker_.store);
  const string searchText = searchable ? asciiVendorText(bomPicker_.query) +
                                             (bomPicker_.reading.empty() ? string() : ", read as " + asciiVendorText(bomPicker_.reading))
                                       : "Not available for " + storeName + " yet";
  body.push_back(ftxui::hbox({ftxui::text(" "), cell("Searched for", kGutter - 1, uiMutedText()),
                              cell(searchText, valueWidth, searchable ? uiPrimaryText() : uiMutedText()), ftxui::text("  "),
                              searchable ? target(uiButton("Search again", "/"), "bom.choose.search", UiTargetKind::Button,
                                                  [self] { self->beginBomPartSearchEdit(); })
                                         : ftxui::text(""),
                              ftxui::filler()}));
  // A store whose answers are less reliable says so, wrapped to the screen, for as long as it is the store.
  if (const auto note = bomPickerStoreNote(bomPicker_.store); !note.empty()) {
    body.push_back(ftxui::hbox({ftxui::text(" "), ftxui::paragraphAlignLeft(note) | ftxui::color(uiWarnColor()) | ftxui::flex}));
  }
  body.push_back(ftxui::text(" "));

  const auto statusRow = [&](const string& text, ftxui::Color color) {
    return ftxui::hbox({ftxui::text(" "), styledText(text, color), ftxui::filler()});
  };
  if (bomPickerFuture_.valid()) {
    const auto done = bomPicker_.progress ? bomPicker_.progress->done.load() : size_t{0};
    const auto total = bomPicker_.progress ? bomPicker_.progress->total.load() : size_t{0};
    const auto seconds = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - bomPicker_.searchStarted).count();
    body.push_back(statusRow("Searching " + storeName + ", " + to_string(done) + " of " + to_string(total) + " done, " +
                                 to_string(seconds) + " s",
                             uiLinkColor()));
    return ftxui::vbox({move(header), ftxui::vbox(move(body)) | ftxui::flex});
  }
  if (bomPicker_.entries.empty()) {
    body.push_back(statusRow(bomPicker_.status, bomPicker_.statusWarns ? uiWarnColor() : uiSecondaryText()));
    return ftxui::vbox({move(header), ftxui::vbox(move(body)) | ftxui::flex});
  }

  const bool generic = bomPartSpec(line).kind == BomPartKind::Generic;
  if (generic) {
    body.push_back(statusRow("Choose one: many parts fit this line.", uiWarnColor()));
    body.push_back(ftxui::text(" "));
  }
  // Each column is as wide as the longest text it holds, so a name is cut only when the screen has no room left
  // for it, and the numbers stay next to the details they belong to.
  const bool showMaker = screenWidth >= 110;
  auto partColumn = tableColumn(16), makerColumn = tableColumn(10), detailColumn = tableColumn(12), stockColumn = tableColumn(9),
       orderColumn = tableColumn(8), costColumn = tableColumn(10), tagColumn = tableColumn(12);
  partColumn.fit("Part number");
  makerColumn.fit("Maker");
  stockColumn.fit("In stock");
  orderColumn.fit("Order");
  costColumn.fit("Cost");
  tagColumn.fit("End of life");  // the longest tag
  for (const auto& entry : bomPicker_.entries) {
    const auto& candidate = entry.candidate;
    const auto cost = vendorCandidateCost(candidate, bomPicker_.quantity);
    const string currency = candidate.offers.empty() ? string() : " " + candidate.offers.front().currency;
    partColumn.fit(asciiVendorText(candidate.manufacturerPartNumber));
    makerColumn.fit(asciiVendorText(candidate.manufacturer));
    detailColumn.fit(pickerDetails(candidate));
    stockColumn.fit(to_string(vendorCandidateStock(candidate)));
    if (cost.has_value()) {
      orderColumn.fit(to_string(cost->orderQuantity) + " pcs");
      costColumn.fit(formatMicros(cost->totalMicros) + currency);
    }
  }
  if (!showMaker) makerColumn = TableColumn{};
  const auto widths = fitColumns({partColumn, makerColumn, detailColumn, stockColumn, orderColumn, costColumn, tagColumn},
                                 screenWidth - 6);
  const int partWidth = widths[0];
  const int makerWidth = widths[1];
  const int detailWidth = widths[2];
  const int stockWidth = widths[3];
  const int orderWidth = widths[4];
  const int costWidth = widths[5];
  const int tagWidth = widths[6];
  ftxui::Elements headerCells{tableCell("Part number", partWidth, uiMutedText())};
  if (showMaker) headerCells.push_back(tableCell("Maker", makerWidth, uiMutedText()));
  for (auto& next : {tableCell("Details", detailWidth, uiMutedText()), tableCell("In stock", stockWidth, uiMutedText(), false, true),
                     tableCell("Order", orderWidth, uiMutedText(), false, true),
                     tableCell("Cost", costWidth, uiMutedText(), false, true)}) {
    headerCells.push_back(next);
  }
  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({ftxui::text("   "), ftxui::hbox(move(headerCells)), ftxui::filler()}));
  const auto mayNotFit = count_if(bomPicker_.entries.begin(), bomPicker_.entries.end(), [](const BomPickerEntry& entry) { return !entry.fits; });
  bool bestShown = false;
  for (size_t entryIndex = 0; entryIndex < bomPicker_.entries.size(); ++entryIndex) {
    const auto& entry = bomPicker_.entries[entryIndex];
    const auto& candidate = entry.candidate;
    // Parts the checks cannot confirm come last, under their own heading.
    if (!entry.fits && (entryIndex == 0 || bomPicker_.entries[entryIndex - 1].fits)) {
      rows.push_back(groupRow("Some parameters might not match the original. Please verify manually.", uiWarnColor(), static_cast<int>(mayNotFit)));
    }
    const bool selected = entryIndex == bomPicker_.selection;
    const auto cost = vendorCandidateCost(candidate, bomPicker_.quantity);
    const string currency = candidate.offers.empty() ? string() : " " + candidate.offers.front().currency;
    // A part the maker has stopped making is never marked Best, so the picker does not recommend it.
    const bool endOfLife = candidateEndOfLife(candidate);
    string tag;
    if (!chosenPart.empty() && trim(candidate.manufacturerPartNumber) == chosenPart) {
      tag = "Chosen";
    } else if (endOfLife) {
      tag = "End of life";
    } else if (entry.fits && !generic && !bestShown) {
      tag = "Best";
    }
    if (entry.fits) bestShown = true;
    ftxui::Elements cells{tableCell(asciiVendorText(candidate.manufacturerPartNumber), partWidth,
                                    selected ? uiFocusColor() : uiPrimaryText(), true)};
    if (showMaker) cells.push_back(tableCell(asciiVendorText(candidate.manufacturer), makerWidth, uiSecondaryText()));
    cells.push_back(tableCell(pickerDetails(candidate), detailWidth, uiSecondaryText()));
    cells.push_back(tableCell(to_string(vendorCandidateStock(candidate)), stockWidth, uiPrimaryText(), false, true));
    cells.push_back(tableCell(cost.has_value() ? to_string(cost->orderQuantity) + " pcs" : string(), orderWidth, uiPrimaryText(),
                              false, true));
    cells.push_back(tableCell(cost.has_value() ? formatMicros(cost->totalMicros) + currency : string(), costWidth, uiPrimaryText(),
                              true, true));
    const auto tagColor = tag == "Chosen" ? uiFocusColor() : endOfLife ? uiWarnColor() : uiSuccessColor();
    cells.push_back(tableCell(tag, tagWidth, tagColor));
    rows.push_back(target(selectableRow(move(cells), selected), "bom.choose.row." + to_string(entryIndex), UiTargetKind::Row,
                          [self, entryIndex] {
                            self->bomPicker_.selection = entryIndex;
                            self->dirty_ = true;
                          }));
  }

  // The selected part in full, since the table cuts long names to fit its columns.
  ftxui::Elements panel;
  if (bomPicker_.selection < bomPicker_.entries.size()) {
    const auto& entry = bomPicker_.entries[bomPicker_.selection];
    const auto& candidate = entry.candidate;
    const int textWidth = max(20, screenWidth - kGutter - 2);
    const auto panelRow = [&](const string& label, const string& text, ftxui::Color color) {
      if (trim(text).empty()) return;
      panel.push_back(gutterRow(label, cell(text, textWidth, color)));
    };
    panel.push_back(ftxui::text(" "));
    panelRow("Selected", asciiVendorText(candidate.manufacturerPartNumber) + "  by " + asciiVendorText(candidate.manufacturer),
             uiPrimaryText());
    panelRow("", asciiVendorText(candidate.description), uiSecondaryText());
    if (const auto details = pickerDetails(candidate); details != asciiVendorText(candidate.description)) {
      panelRow("", details, uiSecondaryText());
    }
    const auto cost = vendorCandidateCost(candidate, bomPicker_.quantity);
    const string currency = candidate.offers.empty() ? string() : " " + candidate.offers.front().currency;
    string buying = to_string(vendorCandidateStock(candidate)) + " in stock at " + storeName + ".";
    if (cost.has_value()) {
      buying += " " + to_string(cost->orderQuantity) + " pcs cost " + formatMicros(cost->totalMicros) + currency + ".";
    }
    panelRow("", buying, uiSecondaryText());
    if (candidateEndOfLife(candidate)) {
      panelRow("", "End of life: the manufacturer has stopped making this part.", uiWarnColor());
      panelRow("", storeName + " still sells it while stock lasts. Find another part if you can.", uiWarnColor());
    }
    if (!entry.fits) panelRow("", "The checks cannot confirm this part fits the BOM line. Check it in the store first.", uiWarnColor());
  }
  panel.push_back(ftxui::text(" "));
  panel.push_back(statusRow(bomPicker_.status, uiMutedText()));
  return ftxui::vbox({move(header), ftxui::vbox(move(body)),
                      ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::flex | ftxui::reflect(bomTableBounds_),
                      ftxui::vbox(move(panel))});
}

ftxui::Element App::renderBomOrder(int screenWidth) const {
  auto self = const_cast<App*>(this);
  const auto rows = bomPurchasesIn(BomLineState::Chosen);
  auto buttons = buttonGroup({
      target(uiButton("Save cart file", "c", UiButtonKind::Primary), "bom.order.cart", UiTargetKind::Button,
             [self] { self->saveBomCartFiles(); }),
      target(uiButton("Open next page", "l"), "bom.order.page", UiTargetKind::Button, [self] { self->openBomOrderPage(true); }),
      target(uiButton("Mark ordered", "m"), "bom.order.mark", UiTargetKind::Button, [self] { self->markBomOrdered(false); }),
  });
  const auto count = static_cast<int>(rows.size());
  string line2 = partCount(count) + " to order" + (bomOrderStores_.empty() ? string() : " at " + bomOrderStores_) +
                 (bomOrderCost_.empty() ? string() : ", about " + bomOrderCost_) + ".";
  auto sub = bomOrderConfirm_ ? styledText("Mark " + partCount(count) + " as ordered? Press y to confirm or n to cancel.", uiWarnColor())
                              : styledText(line2, uiMutedText());
  auto header = uiPageHeader(uiHeaderText("Order", uiPrimaryText()), move(sub), move(buttons));

  auto partColumn = tableColumn(14), numberColumn = tableColumn(14), storeColumn = tableColumn(8), orderColumn = tableColumn(8),
       costColumn = tableColumn(12), pageColumn = tableColumn(8);
  partColumn.fit("Part");
  numberColumn.fit("Part number");
  storeColumn.fit("Store");
  orderColumn.fit("Order");
  costColumn.fit("Not buyable now");
  pageColumn.fit("Opened");
  for (const auto* row : rows) {
    partColumn.fit(lineName(bomAnalysis_.lines[bomAnalysis_.matches[row->matchIndex].lineIndex]));
    numberColumn.fit(row->part);
    storeColumn.fit(vendorDisplayName(row->provider));
    if (row->cost.has_value() && row->offer.has_value()) {
      orderColumn.fit(to_string(row->cost->orderQuantity) + " pcs");
      costColumn.fit(formatMicros(row->cost->totalMicros) + " " + row->offer->currency);
    }
  }
  costColumn.fit(bomOrderCost_);
  const auto widths = fitColumns({partColumn, numberColumn, storeColumn, orderColumn, costColumn, pageColumn}, screenWidth - 5);
  const int partWidth = widths[0];
  const int numberWidth = widths[1];
  const int storeWidth = widths[2];
  const int orderWidth = widths[3];
  const int costWidth = widths[4];
  const int pageWidth = widths[5];
  ftxui::Elements table;
  table.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Part", partWidth, uiMutedText()),
                               tableCell("Part number", numberWidth, uiMutedText()), tableCell("Store", storeWidth, uiMutedText()),
                               tableCell("Order", orderWidth, uiMutedText(), false, true),
                               tableCell("Cost", costWidth, uiMutedText(), false, true), tableCell("Page", pageWidth, uiMutedText()),
                               ftxui::filler()}));
  for (size_t index = 0; index < rows.size(); ++index) {
    const auto& row = *rows[index];
    const auto& line = bomAnalysis_.lines[bomAnalysis_.matches[row.matchIndex].lineIndex];
    const bool selected = index == min(bomOrderSelection_, rows.size() - 1);
    const bool buyable = row.cost.has_value() && row.offer.has_value();
    auto tableRow = selectableRow(
        {tableCell(lineName(line), partWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
         tableCell(row.part, numberWidth, uiSecondaryText()), tableCell(vendorDisplayName(row.provider), storeWidth, uiSecondaryText()),
         tableCell(buyable ? to_string(row.cost->orderQuantity) + " pcs" : string(), orderWidth, uiPrimaryText(), false, true),
         tableCell(buyable ? formatMicros(row.cost->totalMicros) + " " + row.offer->currency : string("Not buyable now"), costWidth,
                   buyable ? uiPrimaryText() : uiWarnColor(), buyable, true),
         tableCell(bomOrderOpened_.count(row.lineKey) > 0 ? "Opened" : "", pageWidth, uiSuccessColor())},
                                  selected);
    table.push_back(target(move(tableRow), "bom.order.row." + to_string(index), UiTargetKind::Row, [self, index] {
      self->bomOrderSelection_ = index;
      self->dirty_ = true;
    }));
  }
  table.push_back(ftxui::text(" "));
  table.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Total", partWidth + numberWidth + storeWidth + orderWidth, uiSecondaryText()),
                               tableCell(bomOrderCost_.empty() ? string("-") : bomOrderCost_, costWidth, uiPrimaryText(), true, true),
                               ftxui::filler()}));
  return ftxui::vbox({move(header), ftxui::vbox(move(table)) | ftxui::yframe | ftxui::flex | ftxui::reflect(bomTableBounds_)});
}

ftxui::Element App::renderBomReceive(int screenWidth) const {
  auto self = const_cast<App*>(this);
  const auto rows = bomPurchasesIn(BomLineState::Ordered);
  auto buttons = buttonGroup({
      target(uiButton("Receive", "Enter", UiButtonKind::Primary), "bom.receive.one", UiTargetKind::Button,
             [self] { self->beginBomReceive(); }),
      target(uiButton("Receive all", "a"), "bom.receive.all", UiTargetKind::Button, [self] { self->receiveAllBomOrders(); }),
  });
  set<string> providers;
  for (const auto* row : rows) providers.insert(vendorDisplayName(row->provider));
  string stores;
  for (const auto& name : providers) stores += (stores.empty() ? "" : " and ") + name;
  const auto count = static_cast<int>(rows.size());
  auto header = uiPageHeader(uiHeaderText("Receive", uiPrimaryText()),
                             styledText(voiceCount(count, "part", "parts") + " ordered" + (stores.empty() ? string() : " from " + stores) +
                                            ". Receive each one when it arrives.",
                                        uiMutedText()),
                             move(buttons));

  auto partColumn = tableColumn(12), numberColumn = tableColumn(14), orderedColumn = tableColumn(8), stockColumn = tableColumn(14),
       whereColumn = tableColumn(10);
  partColumn.fit("Part");
  numberColumn.fit("Part number");
  orderedColumn.fit("Ordered");
  stockColumn.fit("Stock part");
  whereColumn.fit("Goes to");
  for (const auto* row : rows) {
    partColumn.fit(lineName(bomAnalysis_.lines[bomAnalysis_.matches[row->matchIndex].lineIndex]));
    numberColumn.fit(row->part);
    orderedColumn.fit(to_string(row->order.has_value() ? row->order->quantity : row->toBuy) + " pcs");
    stockColumn.fit(row->stockPart);
    whereColumn.fit(row->goesTo);
  }
  const auto widths = fitColumns({partColumn, numberColumn, orderedColumn, stockColumn, whereColumn}, screenWidth - 5);
  const int partWidth = widths[0];
  const int numberWidth = widths[1];
  const int orderedWidth = widths[2];
  const int stockWidth = widths[3];
  const int whereWidth = widths[4];
  ftxui::Elements table;
  table.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Part", partWidth, uiMutedText()),
                               tableCell("Part number", numberWidth, uiMutedText()),
                               tableCell("Ordered", orderedWidth, uiMutedText(), false, true),
                               tableCell("Stock part", stockWidth, uiMutedText()), tableCell("Goes to", whereWidth, uiMutedText()),
                               ftxui::filler()}));
  for (size_t index = 0; index < rows.size(); ++index) {
    const auto& row = *rows[index];
    const auto& line = bomAnalysis_.lines[bomAnalysis_.matches[row.matchIndex].lineIndex];
    const bool selected = index == min(bomReceiveSelection_, rows.size() - 1);
    auto tableRow = selectableRow({tableCell(lineName(line), partWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                                   tableCell(row.part, numberWidth, uiSecondaryText()),
                                   tableCell(to_string(row.order.has_value() ? row.order->quantity : row.toBuy) + " pcs", orderedWidth,
                                             uiPrimaryText(), false, true),
                                   tableCell(row.stockPart, stockWidth, uiSecondaryText()),
                                   tableCell(row.goesTo, whereWidth, row.goesTo == "No location" ? uiMutedText() : uiLinkColor())},
                                  selected);
    table.push_back(target(move(tableRow), "bom.receive.row." + to_string(index), UiTargetKind::Row, [self, index] {
      self->bomReceiveSelection_ = index;
      self->dirty_ = true;
    }));
  }
  return ftxui::vbox({move(header), ftxui::vbox(move(table)) | ftxui::yframe | ftxui::flex | ftxui::reflect(bomTableBounds_)});
}

ftxui::Element App::renderBomLines(int screenWidth) const {
  auto self = const_cast<App*>(this);
  const auto* project = activeBomProject();
  auto buttons = buttonGroup({target(uiButton("Find in racks", "f", UiButtonKind::Primary), "bom.find-in-racks",
                                     UiTargetKind::Button, [self] { self->requestBomFindInRacks(); })});
  auto header = uiPageHeader(uiHeaderText((project == nullptr ? string("Project") : project->name) + ": all lines", uiPrimaryText()),
                             styledText(partCount(bomProgress_.lines) + " for " + voiceCount(bomAnalysis_.boards, "board", "boards") + ".",
                                        uiMutedText()),
                             move(buttons));

  // Rows keep the analysis order, so the arrow keys and the groups agree.
  const auto groupOf = [](BomLineState state) {
    return state == BomLineState::InStock ? 0 : state == BomLineState::ToConfirm ? 1 : 2;
  };
  struct LineCells {
    BomLineState state;
    string where;
    string stateText;
    ftxui::Color stateColor;
  };
  vector<int> groupCounts(3, 0);
  vector<LineCells> lineCells;
  auto partColumn = tableColumn(14), packageColumn = tableColumn(8), whereColumn = tableColumn(12), needColumn = tableColumn(5),
       haveColumn = tableColumn(5), stateColumn = tableColumn(12);
  partColumn.fit("Part");
  packageColumn.fit("Package");
  whereColumn.fit("Where");
  needColumn.fit("Need");
  haveColumn.fit("Have");
  stateColumn.fit("State");
  for (const auto& match : bomAnalysis_.matches) {
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const auto state = project == nullptr ? BomLineState::ToChoose : bomLineState(match, bomLineKey(line), project->enrichment);
    ++groupCounts[groupOf(state)];
    const auto* item = store_.findById(match.chosenItemId());
    const auto place = item == nullptr ? string() : bomItemPlace(*item, store_.racks());
    LineCells cells{state, item == nullptr ? string("-") : place.empty() ? string("No location") : place, "In stock", uiSuccessColor()};
    const int shortBy = max(0, match.needed - match.available);
    switch (state) {
      case BomLineState::InStock: break;
      case BomLineState::ToConfirm: cells.stateText = "To confirm"; cells.stateColor = uiWarnColor(); break;
      case BomLineState::ToChoose:
        cells.stateText = match.confirmed && match.available > 0 ? "Short " + to_string(shortBy) : string("To choose");
        cells.stateColor = uiDangerColor();
        break;
      case BomLineState::Chosen: {
        const auto chosen = project->enrichment.find(bomLineKey(line));
        cells.stateText = "Chosen at " + bomEnrichmentSupplier(chosen->second);
        cells.stateColor = uiSecondaryText();
        break;
      }
      case BomLineState::Ordered: cells.stateText = "Ordered"; cells.stateColor = uiLinkColor(); break;
    }
    if (state != BomLineState::InStock && state != BomLineState::ToConfirm && !match.confirmed) cells.where = "-";
    partColumn.fit(line.designation);
    packageColumn.fit(packageFromFootprint(line.footprint));
    whereColumn.fit(cells.where);
    needColumn.fit(to_string(match.needed));
    haveColumn.fit(to_string(match.available));
    stateColumn.fit(cells.stateText);
    lineCells.push_back(move(cells));
  }
  const auto widths = fitColumns({partColumn, packageColumn, whereColumn, needColumn, haveColumn, stateColumn}, screenWidth - 5);
  const int partWidth = widths[0];
  const int packageWidth = widths[1];
  const int whereWidth = widths[2];
  const int needWidth = widths[3];
  const int haveWidth = widths[4];
  const int stateWidth = widths[5];
  ftxui::Elements table;
  table.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Part", partWidth, uiMutedText()),
                               tableCell("Package", packageWidth, uiMutedText()), tableCell("Where", whereWidth, uiMutedText()),
                               tableCell("Need", needWidth, uiMutedText(), false, true),
                               tableCell("Have", haveWidth, uiMutedText(), false, true), tableCell("State", stateWidth, uiMutedText()),
                               ftxui::filler()}));

  int previousGroup = -1;
  for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
    const auto& match = bomAnalysis_.matches[index];
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const auto& [state, where, stateText, stateColor] = lineCells[index];
    const int group = groupOf(state);
    if (group != previousGroup) {
      table.push_back(groupRow(group == 0 ? "In stock" : group == 1 ? "To confirm" : "Not in stock",
                               group == 0 ? uiSuccessColor() : group == 1 ? uiWarnColor() : uiDangerColor(), groupCounts[group]));
      previousGroup = group;
    }
    const bool selected = index == min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
    auto row = selectableRow({tableCell(line.designation, partWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                              tableCell(packageFromFootprint(line.footprint), packageWidth, uiSecondaryText()),
                              tableCell(where, whereWidth, where == "-" || where == "No location" ? uiMutedText() : uiLinkColor()),
                              tableCell(to_string(match.needed), needWidth, uiSecondaryText(), false, true),
                              tableCell(to_string(match.available), haveWidth, match.sufficient ? uiPrimaryText() : uiDangerColor(),
                                        true, true),
                              tableCell(stateText, stateWidth, stateColor)},
                             selected);
    table.push_back(target(move(row), "bom.line." + to_string(index), UiTargetKind::Row, [self, index] {
      self->bomSplitSelection_ = index;
      self->dirty_ = true;
    }));
  }
  return ftxui::vbox({move(header), ftxui::vbox(move(table)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::flex |
                                        ftxui::reflect(bomTableBounds_)});
}

}  // namespace inventatory
