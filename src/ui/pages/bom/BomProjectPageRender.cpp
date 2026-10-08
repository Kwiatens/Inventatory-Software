// Inventatory - Hardware Inventory Management System
// KiCad BOM project rendering: pinned list, BOM comparison, Find in racks workflow.

#include "App.h"

#include "ui/pages/racks/RackManagementPagePrivate.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

// A text cell of exact width; bold for the figures a row is read by.
ftxui::Element cell(const string& text, int width, ftxui::Color color, bool bold = false, bool rightAlign = false) {
  const auto clipped = ellipsize(text, static_cast<size_t>(max(0, width - 1)));
  auto content = bold ? uiHeaderText(clipped, color) : styledText(clipped, color);
  auto row = rightAlign ? ftxui::hbox({ftxui::filler(), move(content), ftxui::text(" ")})
                        : ftxui::hbox({move(content), ftxui::filler()});
  return row | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

// A voice line used as a page title: every word is bold, slots keep their focus colour.
ftxui::Element voiceTitle(const VoiceLine& line) {
  ftxui::Elements spans;
  for (const auto& span : line) {
    spans.push_back(uiHeaderText(span.text, span.tone == VoiceTone::Slot ? uiFocusColor() : uiPrimaryText()));
  }
  return ftxui::hbox(move(spans));
}

ftxui::Element buttonGroup(ftxui::Elements buttons) {
  ftxui::Elements spaced;
  for (auto& button : buttons) {
    if (!spaced.empty()) spaced.push_back(ftxui::text(" "));
    spaced.push_back(move(button));
  }
  return ftxui::hbox(move(spaced));
}

// "Rack 1 > Rack 2 > Outside racks > Finish", with the current stop in bold focus text.
ftxui::Element routeLine(const BomPickPlan& plan, size_t current, bool finishing) {
  ftxui::Elements parts{ftxui::text(" ")};
  for (size_t index = 0; index <= plan.stops.size(); ++index) {
    if (index > 0) parts.push_back(styledText(" > ", uiMutedText()));
    const bool atFinish = index == plan.stops.size();
    const auto& label = atFinish ? string("Finish") : plan.stops[index].title;
    const bool active = finishing ? atFinish : index == current;
    parts.push_back(active ? uiHeaderText(label, uiFocusColor()) : styledText(label, uiMutedText()));
  }
  parts.push_back(ftxui::filler());
  return ftxui::hbox(move(parts));
}

string designatorText(const vector<string>& designators, int width) {
  string text;
  for (size_t index = 0; index < designators.size(); ++index) {
    const auto next = (text.empty() ? string() : string(" ")) + designators[index];
    const auto remaining = designators.size() - index;
    const auto more = " +" + to_string(remaining);
    if (static_cast<int>(text.size() + next.size()) > width - 1 ||
        (remaining > 1 && static_cast<int>(text.size() + next.size() + more.size()) > width - 1)) {
      return text + (text.empty() ? "+" + to_string(remaining) : more);
    }
    text += next;
  }
  return text;
}

}  // namespace

ftxui::Element App::renderBomProjectUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 40;
  auto self = const_cast<App*>(this);
  const auto surface = uiSurfaceBg();

  const auto dirtyRow = [&]() -> ftxui::Element {
    if (!bomProjectsDirty_) return ftxui::emptyElement();
    return fullLine(" Project changes are not saved yet. Press R to retry.", uiDangerColor(), uiDangerBg());
  };

  // ---------------------------------------------------------------- list ---
  if (bomView_ == BomView::List || !bomAnalysisValid_) {
    auto importButton = target(uiButton("Import BOM", "i", bomProjects_.empty() ? UiButtonKind::Primary : UiButtonKind::Normal),
                               "bom.import", UiTargetKind::Button, [self] { self->beginCsvImport(); });
    if (bomProjects_.empty()) {
      return ftxui::vbox({
                 uiPageHeader(uiHeaderText("Projects", uiPrimaryText()),
                              uiVoiceLine({{"There are no projects yet. Import a KiCad BOM to start one."}}),
                              move(importButton)),
                 ftxui::filler(),
             }) |
             ftxui::bgcolor(surface) | ftxui::flex;
    }

    const int nameWidth = clamp(screenWidth / 3, 24, 48);
    ftxui::Elements rows;
    rows.push_back(ftxui::hbox({ftxui::text("   "), cell("Project", nameWidth, uiMutedText()),
                                cell("Lines", 8, uiMutedText(), false, true), cell("Boards", 9, uiMutedText(), false, true),
                                ftxui::text("   "), cell("Built", 12, uiMutedText())}));
    for (size_t index = 0; index < bomProjects_.size(); ++index) {
      const auto& project = bomProjects_[index];
      const bool selected = index == min(bomProjectSelection_, bomProjects_.size() - 1);
      const auto bg = selected ? uiSelectionBg() : surface;
      // Render runs at 10 Hz, so the row count comes from a cheap newline count
      // rather than a full re-parse of the stored BOM.
      const auto lineCount = count(project.bomText.begin(), project.bomText.end(), '\n');
      auto row = ftxui::hbox({
                     styledText(selected ? " > " : "   ", uiFocusColor()),
                     cell(project.name, nameWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                     cell(to_string(max<long long>(0, lineCount - 1)), 8, uiSecondaryText(), false, true),
                     cell(to_string(project.boards), 9, uiSecondaryText(), false, true),
                     ftxui::text("   "),
                     cell(project.lastBuilt > 0 ? "Yes" : "Not yet", 12, project.lastBuilt > 0 ? uiSuccessColor() : uiMutedText()),
                     ftxui::filler(),
                 }) |
                 ftxui::bgcolor(bg);
      if (selected) row = row | ftxui::select;
      rows.push_back(target(row, "bom.project." + project.id, UiTargetKind::Row, [self, index] {
        self->bomProjectSelection_ = index;
        self->openSelectedBomProject();
      }));
    }

    const auto count = static_cast<int>(bomProjects_.size());
    return ftxui::vbox({
               uiPageHeader(uiHeaderText("Projects", uiPrimaryText()),
                            uiVoiceLine({{voiceCount(count, "project", "projects"), VoiceTone::Strong},
                                         {count == 1 ? " is pinned. Open it to compare its BOM with stock."
                                                     : " are pinned. Open one to compare its BOM with stock."}}),
                            move(importButton)),
               dirtyRow(),
               ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::flex,
           }) |
           ftxui::bgcolor(surface) | ftxui::flex;
  }

  const auto* project = activeBomProject();
  const auto projectName = project == nullptr ? string("Project") : project->name;

  // --------------------------------------------------------------- build ---
  if (bomView_ == BomView::Build) {
    const auto plan = bomPickPlan();
    if (plan.empty()) return ftxui::text("");
    const auto stopIndex = min(bomBuildStep_, plan.stops.size() - 1);

    // ---- finish: one table, one question ----
    if (bomDeductPrompt_) {
      int planned = 0;
      int taken = 0;
      vector<string> lowAfter;
      ftxui::Elements rows;
      const int partWidth = clamp(screenWidth / 5, 16, 26);
      const int packageWidth = 12;
      const int fromWidth = 16;
      const int tookWidth = 7;
      const int afterWidth = 16;
      rows.push_back(ftxui::hbox({ftxui::text("   "), cell("Part", partWidth, uiMutedText()),
                                  cell("Package", packageWidth, uiMutedText()), cell("From", fromWidth, uiMutedText()),
                                  ftxui::filler(), cell("Took", tookWidth, uiMutedText(), false, true),
                                  cell("Stock after", afterWidth, uiMutedText(), false, true), ftxui::text("      ")}));
      size_t rowIndex = 0;
      size_t pickCount = 0;
      for (const auto& stop : plan.stops) pickCount += stop.picks.size();
      const auto selection = pickCount == 0 ? size_t(0) : min(bomFinishSelection_, pickCount - 1);
      for (const auto& stop : plan.stops) {
        for (const auto& pick : stop.picks) {
          const auto* item = store_.findById(pick.itemId);
          const int took = bomPickTaken(pick);
          const int have = item == nullptr ? 0 : item->quantity;
          const int after = max(0, have - took);
          planned += pick.quantity;
          taken += took;
          InventoryItem afterItem = item == nullptr ? InventoryItem{} : *item;
          afterItem.quantity = after;
          const bool out = after == 0;
          const bool low = !out && isLowStock(afterItem, settings_.lowStockThreshold);
          if (out || low) lowAfter.push_back(pick.designation);
          const bool selected = rowIndex == selection;
          const auto bg = selected ? uiSelectionBg() : surface;
          const string from = stop.rackId.empty() ? pick.slot : stop.title + "  " + pick.slot;
          auto row = ftxui::hbox({
                         styledText(selected ? " > " : "   ", uiFocusColor()),
                         cell(pick.designation, partWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                         cell(pick.package, packageWidth, uiSecondaryText()),
                         cell(from, fromWidth, uiLinkColor()),
                         ftxui::filler(),
                         cell(to_string(took), tookWidth, took < pick.quantity ? uiWarnColor() : uiPrimaryText(), true, true),
                         ftxui::hbox({ftxui::filler(), styledText(to_string(have) + " to ", uiMutedText()),
                                      uiHeaderText(to_string(after), out ? uiDangerColor() : low ? uiWarnColor() : uiPrimaryText()),
                                      ftxui::text(" ")}) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, afterWidth),
                         cell(out ? "Out" : low ? "Low" : "", 6, out ? uiDangerColor() : uiWarnColor()),
                     }) |
                     ftxui::bgcolor(bg);
          if (selected) row = row | ftxui::select;
          rows.push_back(target(row, "bom.finish.row." + to_string(rowIndex), UiTargetKind::Row, [self, rowIndex] {
            self->bomFinishSelection_ = rowIndex;
            self->dirty_ = true;
          }));
          ++rowIndex;
        }
      }

      auto buttons = buttonGroup({
          target(uiButton("Deduct from stock", "y", UiButtonKind::Primary), "bom.deduct.yes", UiTargetKind::Button,
                 [self] { self->finishBomBuild(true); }),
          target(uiButton("Keep stock", "n"), "bom.deduct.no", UiTargetKind::Button, [self] { self->finishBomBuild(false); }),
      });
      return ftxui::vbox({
                 uiPageHeader(voiceTitle(bomPickFinishTitle(taken, planned)),
                              uiVoiceLine(bomPickFinishGuide(lowAfter, plan.shortMatches.size())), move(buttons)),
                 ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::flex,
                 ftxui::text(""),
                 routeLine(plan, stopIndex, true),
             }) |
             ftxui::bgcolor(surface) | ftxui::flex;
    }

    const auto& stop = plan.stops[stopIndex];
    auto buttons = buttonGroup({
        target(uiButton(stopIndex + 1 < plan.stops.size() ? "Rack done" : "Done", "Enter", UiButtonKind::Primary),
               "bom.build.next", UiTargetKind::Button, [self] { self->advanceBomBuild(1); }),
        target(uiButton("Back", "Bksp"), "bom.build.back", UiTargetKind::Button, [self] { self->advanceBomBuild(-1); }),
    });
    auto header = uiPageHeader(voiceTitle(bomPickStopTitle(stop)), uiVoiceLine(bomPickStopGuide(stop)), move(buttons));

    // The pick list: slot, part, the board references it is for, and how many to take.
    const bool loose = stop.rackId.empty();
    const int sideWidth = loose ? screenWidth - 2 : clamp(screenWidth * 2 / 5, 44, 64);
    const int slotWidth = loose ? clamp(sideWidth / 5, 14, 24) : 6;
    const int partWidth = clamp(sideWidth / 5, 9, 18);
    const int takeWidth = 6;
    const int forWidth = max(8, sideWidth - slotWidth - partWidth - takeWidth);
    ftxui::Elements list;
    list.push_back(ftxui::hbox({cell(loose ? "Location" : "Slot", slotWidth, uiMutedText()), cell("Part", partWidth, uiMutedText()),
                                cell("For", forWidth, uiMutedText()), cell("Take", takeWidth, uiMutedText(), false, true)}));
    for (const auto& pick : stop.picks) {
      list.push_back(ftxui::hbox({cell(pick.slot, slotWidth, uiFocusColor(), true),
                                  cell(pick.designation, partWidth, uiPrimaryText(), true),
                                  cell(designatorText(pick.designators, forWidth), forWidth, uiSecondaryText()),
                                  cell(to_string(pick.quantity), takeWidth,
                                       pick.quantity < pick.needed ? uiWarnColor() : uiPrimaryText(), true, true)}));
    }
    list.push_back(ftxui::text(""));
    list.push_back(ftxui::hbox({cell("Total", slotWidth + partWidth + forWidth, uiSecondaryText()),
                                cell(to_string(stop.pieces()), takeWidth, uiPrimaryText(), true, true)}));
    list.push_back(ftxui::text(""));
    list.push_back(uiVoiceLine(bomPickNextStop(plan, stopIndex)));
    auto sideList = ftxui::vbox(move(list)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, sideWidth);

    if (loose) {
      return ftxui::vbox({
                 move(header),
                 ftxui::hbox({ftxui::text(" "), move(sideList), ftxui::filler()}) | ftxui::yframe | ftxui::flex,
                 routeLine(plan, stopIndex, false),
             }) |
             ftxui::bgcolor(surface) | ftxui::flex;
    }

    // The rack grid: lettered columns, numbered rows, one-cell gaps on the workspace surface. Slots to
    // open carry the BOM value and how many to take; every other slot is a quiet raised block.
    constexpr int rows = 5;
    constexpr int columns = 5;
    constexpr int designatorWidth = 3;
    const int gridWidth = max(40, screenWidth - sideWidth - 4);
    const int slotWidth2 = rack_page_detail::equalRackSlotWidth(gridWidth - designatorWidth - (columns - 1), columns);
    // Shell (5 rows), page header (3), letter row (1), row gaps (4) and the route line with its gap (2).
    const int slotHeight = rack_page_detail::equalRackSlotHeight(max(rows * 3, screenHeight - 15), rows);
    const bool pulse = uiBlinkOn(700);

    map<string, const BomPick*> picksBySlot;
    for (const auto& pick : stop.picks) picksBySlot[pick.slot] = &pick;

    ftxui::Elements grid;
    ftxui::Elements letters{ftxui::text(string(designatorWidth, ' '))};
    for (int column = 0; column < columns; ++column) {
      if (column > 0) letters.push_back(ftxui::text(" "));
      letters.push_back(ftxui::hbox({ftxui::filler(), uiHeaderText(string(1, static_cast<char>('A' + column)), uiMutedText()),
                                     ftxui::filler()}) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, slotWidth2));
    }
    grid.push_back(ftxui::hbox(move(letters)));
    for (int row = 0; row < rows; ++row) {
      if (row > 0) grid.push_back(ftxui::text(""));
      ftxui::Elements cells;
      cells.push_back(ftxui::vbox({ftxui::filler(), uiHeaderText(" " + to_string(row + 1) + " ", uiMutedText()), ftxui::filler()}) |
                      ftxui::size(ftxui::WIDTH, ftxui::EQUAL, designatorWidth));
      for (int column = 0; column < columns; ++column) {
        if (column > 0) cells.push_back(ftxui::text(" "));
        const auto slot = rackSlotLabel(column, row);
        const auto found = picksBySlot.find(slot);
        ftxui::Element body;
        if (found != picksBySlot.end()) {
          // Lit slots pulse between the interactive fill and the active surface; both states stay lit.
          const auto bg = pulse ? uiInteractiveColor() : uiActiveBg();
          const auto fg = pulse ? uiCanvasBg() : uiPrimaryText();
          const auto slotFg = pulse ? uiCanvasBg() : uiFocusColor();
          body = ftxui::vbox({
                     uiHeaderText(" " + slot, slotFg),
                     uiHeaderText(" " + ellipsize(found->second->designation, static_cast<size_t>(max(1, slotWidth2 - 1))), fg),
                     ftxui::filler(),
                     uiHeaderText(" Take " + to_string(found->second->quantity), fg),
                 }) |
                 ftxui::color(fg) | ftxui::bgcolor(bg);
        } else {
          const auto* item = itemAtRackSlot(store_, stop.rackId, slot);
          const auto value = item == nullptr ? string("-") : splitPartName(item->partName).value;
          body = ftxui::vbox({ftxui::filler(),
                              styledText(" " + ellipsize(value.empty() ? item->partName : value,
                                                         static_cast<size_t>(max(1, slotWidth2 - 1))),
                                         uiDimColor()),
                              ftxui::filler()}) |
                 ftxui::bgcolor(uiRaisedSurfaceBg());
        }
        cells.push_back(move(body) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, slotWidth2) |
                        ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, slotHeight));
      }
      grid.push_back(ftxui::hbox(move(cells)));
    }
    const int gridExactWidth = designatorWidth + columns * slotWidth2 + (columns - 1);

    return ftxui::vbox({
               move(header),
               ftxui::hbox({ftxui::vbox(move(grid)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, gridExactWidth),
                            ftxui::text("   "), move(sideList), ftxui::filler()}) |
                   ftxui::flex,
               ftxui::text(""),
               routeLine(plan, stopIndex, false),
           }) |
           ftxui::bgcolor(surface) | ftxui::flex;
  }

  // ------------------------------------------------------------ compare ---
  int suggested = 0;
  if (project != nullptr) {
    for (const auto& match : bomAnalysis_.matches) {
      if (match.sufficient) continue;
      const auto key = bomLineKey(bomAnalysis_.lines[match.lineIndex]);
      const auto found = project->enrichment.find(key);
      if (bomEnrichmentCached(project->enrichment, key) && found->second != kBomEnrichmentNoMatch) ++suggested;
    }
  }

  auto title = ftxui::hbox({
      uiHeaderText(projectName, uiPrimaryText()),
      ftxui::text("   "),
      target(uiButton("-"), "bom.boards.less", UiTargetKind::Button, [self] { self->adjustBomBoards(-1); }),
      uiHeaderText(" " + voiceCount(bomAnalysis_.boards, "board", "boards") + " ", uiPrimaryText()),
      target(uiButton("+"), "bom.boards.more", UiTargetKind::Button, [self] { self->adjustBomBoards(1); }),
  });
  auto buttons = buttonGroup({
      target(uiButton("Pick parts", "f", UiButtonKind::Primary), "bom.find-in-racks", UiTargetKind::Button,
             [self] { self->beginBomBuild(); }),
      target(uiButton("Shopping list", "o"), "bom.shopping-list", UiTargetKind::Button,
             [self] { self->exportBomShortages(); }),
  });

  const int indent = 3;
  const int partWidth = clamp(screenWidth / 4, 20, 32);
  const int packageWidth = clamp(screenWidth / 9, 11, 18);
  const int needWidth = 7;
  const int haveWidth = 7;
  const int statusWidth = 10;
  const int whereWidth = max(16, screenWidth - indent - partWidth - packageWidth - needWidth - haveWidth - statusWidth - 2);

  ftxui::Elements tableRows;
  tableRows.push_back(ftxui::hbox({ftxui::text(string(indent, ' ')), cell("Part", partWidth, uiMutedText()),
                                   cell("Package", packageWidth, uiMutedText()), cell("Where", whereWidth, uiMutedText()),
                                   cell("Need", needWidth, uiMutedText(), false, true),
                                   cell("Have", haveWidth, uiMutedText(), false, true),
                                   cell("Status", statusWidth, uiMutedText(), false, true)}));

  map<string, int> groupCounts;
  for (const auto& match : bomAnalysis_.matches) {
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    ++groupCounts[(match.sufficient ? "s|" : "m|") + toLower(bomComparisonCategory(line, match, store_.items()))];
    ++groupCounts[match.sufficient ? "s" : "m"];
  }

  string previousAvailability;
  string previousCategory;
  for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
    const auto& match = bomAnalysis_.matches[index];
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const bool selected = index == min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
    const auto* item = store_.findById(match.chosenItemId());
    const auto availability = match.sufficient ? string("In stock") : string("Missing");
    const string groupKey = match.sufficient ? "s" : "m";
    const auto category = bomComparisonCategory(line, match, store_.items());
    if (availability != previousAvailability) {
      tableRows.push_back(ftxui::hbox({ftxui::text(" "),
                                       uiHeaderText(availability, match.sufficient ? uiSuccessColor() : uiDangerColor()),
                                       styledText("  " + to_string(groupCounts[groupKey]), uiMutedText()), ftxui::filler()}) |
                          ftxui::bgcolor(uiRaisedSurfaceBg()));
      previousAvailability = availability;
      previousCategory.clear();
    }
    if (toLower(category) != toLower(previousCategory)) {
      tableRows.push_back(ftxui::hbox({ftxui::text("  "), styledText(category, uiSecondaryText()),
                                       styledText("  " + to_string(groupCounts[groupKey + "|" + toLower(category)]),
                                                  uiMutedText()),
                                       ftxui::filler()}));
      previousCategory = category;
    }

    string where = "-";
    auto whereColor = uiMutedText();
    if (match.sufficient) {
      const auto rack = item == nullptr ? string() : rackLocation(*item, store_.racks());
      if (!rack.empty()) {
        // rackLocation reads "R1-E3"; spell the rack out the way the pick route does.
        const auto dash = rack.find('-');
        where = dash == string::npos ? rack : "Rack " + to_string(max(1, rackNumberFromCode(rack.substr(0, dash)))) + "  " + rack.substr(dash + 1);
      } else if (item != nullptr && !item->location.empty()) {
        where = item->location;
      }
      whereColor = uiLinkColor();
    } else if (project != nullptr) {
      const auto lineKey = bomLineKey(line);
      const auto found = project->enrichment.find(lineKey);
      if (bomEnrichmentCached(project->enrichment, lineKey)) {
        where = found->second == kBomEnrichmentNoMatch ? string("No DigiKey match") : "DigiKey  " + found->second;
        whereColor = found->second == kBomEnrichmentNoMatch ? uiMutedText() : uiSecondaryText();
      } else if (bomEnrichmentFuture_.valid() && lineKey == bomEnrichmentActiveKey_) {
        where = "Looking up on DigiKey";
      } else if (find(bomEnrichmentQueue_.begin(), bomEnrichmentQueue_.end(), lineKey) != bomEnrichmentQueue_.end()) {
        where = "Waiting for DigiKey lookup";
      }
    }

    const auto haveColor = match.sufficient ? uiPrimaryText() : match.available <= 0 ? uiDangerColor() : uiWarnColor();
    const int shortBy = max(0, match.needed - match.available);
    const auto bg = selected ? uiSelectionBg() : surface;
    auto row = ftxui::hbox({
                   styledText(selected ? " > " : "   ", uiFocusColor()),
                   cell(line.designation, partWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                   cell(packageFromFootprint(line.footprint), packageWidth, uiSecondaryText()),
                   cell(where, whereWidth, whereColor),
                   cell(to_string(match.needed), needWidth, uiSecondaryText(), false, true),
                   cell(to_string(match.available), haveWidth, haveColor, true, true),
                   cell(match.sufficient ? "Ready" : match.available > 0 ? "Short " + to_string(shortBy) : "Missing",
                        statusWidth, match.sufficient ? uiSuccessColor() : haveColor, false, true),
               }) |
               ftxui::bgcolor(bg);
    if (selected) row = row | ftxui::select;
    tableRows.push_back(target(row, "bom.line." + to_string(index), UiTargetKind::Row, [self, index] {
      self->bomSplitSelection_ = index;
      self->dirty_ = true;
    }));
  }

  return ftxui::vbox({
             uiPageHeader(move(title), uiVoiceLine(bomProjectSummary(bomAnalysis_, suggested)), move(buttons)),
             dirtyRow(),
             ftxui::vbox(move(tableRows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::flex |
                 ftxui::reflect(bomTableBounds_),
         }) |
         ftxui::bgcolor(surface) | ftxui::flex;
}

}  // namespace inventatory
