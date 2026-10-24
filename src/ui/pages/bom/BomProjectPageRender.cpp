// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// KiCad BOM project rendering: pinned list, BOM comparison, Find in racks workflow.

#include "App.h"

#include "core/inventory/VendorProvider.h"
#include "platform/system/Console.h"
#include "ui/pages/racks/RackManagementPagePrivate.h"
#include "ui/pages/bom/BomPageShared.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;
using namespace bom_page;

namespace {

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

  if (bomView_ == BomView::List || !bomAnalysisValid_) {
    auto importButton = target(uiButton("Import BOM", "i", bomProjects_.empty() ? UiButtonKind::Primary : UiButtonKind::Normal),
                               "bom.import", UiTargetKind::Button, [self] { self->beginCsvImport(); });
    if (bomProjects_.empty()) {
      return ftxui::vbox({
                 uiPageHeader(uiHeaderText("Projects", uiPrimaryText()),
                              uiVoiceLine({{"There are no projects yet. Import a BOM to start one."}}),
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
      // Renders at 10 Hz, so count newlines rather than re-parsing the stored BOM.
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

  if (bomView_ == BomView::Build) {
    const auto plan = bomPickPlan();
    if (plan.empty()) return ftxui::text("");
    const auto stopIndex = min(bomBuildStep_, plan.stops.size() - 1);

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
                              uiVoiceLine(bomPickFinishGuide(lowAfter, plan.shortMatches.size()), nullopt, screenWidth - 3), move(buttons)),
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
    auto header = uiPageHeader(voiceTitle(bomPickStopTitle(stop)), ftxui::text(""), move(buttons));

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

    // Lit slots carry the BOM value and the take; the other slots are a quiet raised block.
    constexpr int rows = 5;
    constexpr int columns = 5;
    constexpr int designatorWidth = 3;
    const int gridWidth = max(40, screenWidth - sideWidth - 4);
    const int slotWidth2 = rack_page_detail::equalRackSlotWidth(gridWidth - designatorWidth - (columns - 1), columns);
    // The 15 rows outside the grid: shell, page header, letter row, row gaps and route line.
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
        // Every slot shows its number and stored value in the same place, lit or not.
        const auto* pick = found != picksBySlot.end() ? found->second : nullptr;
        const auto* item = pick != nullptr ? store_.findById(pick->itemId) : itemAtRackSlot(store_, stop.rackId, slot);
        string value = pick != nullptr ? pick->designation : string("-");
        if (item != nullptr) {
          const auto split = splitPartName(item->partName).value;
          value = split.empty() ? item->partName : split;
        }
        const auto valueWidth = static_cast<size_t>(max(1, slotWidth2 - 2));
        ftxui::Element body;
        if (pick != nullptr) {
          // Lit slots pulse between two fills; both stay lit.
          const auto bg = pulse ? uiInteractiveColor() : uiActiveBg();
          const auto fg = pulse ? uiCanvasBg() : uiPrimaryText();
          const auto slotFg = pulse ? uiCanvasBg() : uiFocusColor();
          body = ftxui::vbox({
                     uiHeaderText(" " + slot, slotFg),
                     uiHeaderText(" " + ellipsize(value, valueWidth), fg),
                     ftxui::filler(),
                     uiHeaderText(" Take " + to_string(pick->quantity), fg),
                 }) |
                 ftxui::color(fg) | ftxui::bgcolor(bg);
        } else {
          body = ftxui::vbox({
                     styledText(" " + slot, uiMutedText()),
                     styledText(" " + ellipsize(value, valueWidth), uiDimColor()),
                     ftxui::filler(),
                 }) |
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

  // Every step screen uses the full width. Their columns grow with it, and the detail column takes the rest.
  ftxui::Element screen;
  switch (bomView_) {
    case BomView::Lines: screen = renderBomLines(screenWidth); break;
    case BomView::StartNotice: screen = renderBomStartNotice(screenWidth); break;
    case BomView::Confirm: screen = renderBomConfirm(screenWidth); break;
    case BomView::Choose: screen = renderBomChoose(screenWidth); break;
    case BomView::Order: screen = renderBomOrder(screenWidth); break;
    case BomView::Receive: screen = renderBomReceive(screenWidth); break;
    default: screen = renderBomOverview(screenWidth); break;
  }
  return ftxui::vbox({dirtyRow(), move(screen) | ftxui::flex}) | ftxui::bgcolor(surface) | ftxui::flex;
}

}  // namespace inventatory
