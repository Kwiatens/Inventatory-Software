// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// KiCad BOM project rendering: pinned list, BOM comparison, Find in racks workflow.

#include "App.h"

#include "core/racks/StorageGridLayout.h"

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

    auto nameColumn = tableColumn(16), linesColumn = tableColumn(5), boardsColumn = tableColumn(6), builtColumn = tableColumn(7);
    nameColumn.fit("Project");
    linesColumn.fit("Lines");
    boardsColumn.fit("Boards");
    builtColumn.fit("Not yet");
    for (const auto& project : bomProjects_) {
      nameColumn.fit(project.name);
      linesColumn.fit(to_string(count(project.bomText.begin(), project.bomText.end(), '\n')));
      boardsColumn.fit(to_string(project.boards));
    }
    const auto widths = fitColumns({nameColumn, linesColumn, boardsColumn, builtColumn}, screenWidth - 5);
    const int nameWidth = widths[0];
    const int linesWidth = widths[1];
    const int boardsWidth = widths[2];
    const int builtWidth = widths[3];
    ftxui::Elements rows;
    rows.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Project", nameWidth, uiMutedText()),
                                tableCell("Lines", linesWidth, uiMutedText(), false, true),
                                tableCell("Boards", boardsWidth, uiMutedText(), false, true), tableCell("Built", builtWidth, uiMutedText()),
                                ftxui::filler()}));
    for (size_t index = 0; index < bomProjects_.size(); ++index) {
      const auto& project = bomProjects_[index];
      const bool selected = index == min(bomProjectSelection_, bomProjects_.size() - 1);
      const auto bg = selected ? uiSelectionBg() : surface;
      // Renders at 10 Hz, so count newlines rather than re-parsing the stored BOM.
      const auto lineCount = count(project.bomText.begin(), project.bomText.end(), '\n');
      auto row = ftxui::hbox({
                     styledText(selected ? " > " : "   ", uiFocusColor()),
                     tableCell(project.name, nameWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                     tableCell(to_string(max<long long>(0, lineCount - 1)), linesWidth, uiSecondaryText(), false, true),
                     tableCell(to_string(project.boards), boardsWidth, uiSecondaryText(), false, true),
                     tableCell(project.lastBuilt > 0 ? "Yes" : "Not yet", builtWidth,
                               project.lastBuilt > 0 ? uiSuccessColor() : uiMutedText()),
                     ftxui::filler(),
                 }) |
                 ftxui::bgcolor(bg);
      if (selected) row = row | ftxui::select;
      rows.push_back(target(row, "bom.project." + project.id, UiTargetKind::Row, [self, index] {
        self->bomProjectSelection_ = index;
        self->openSelectedBomProject();
      }));
    }

    return ftxui::vbox({
               uiPageHeader(uiHeaderText("Projects", uiPrimaryText()), ftxui::text(""), move(importButton)),
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
      auto partColumn = tableColumn(12), packageColumn = tableColumn(8), fromColumn = tableColumn(12), tookColumn = tableColumn(4),
           afterColumn = tableColumn(11), markColumn = tableColumn(3);
      partColumn.fit("Part");
      packageColumn.fit("Package");
      fromColumn.fit("From");
      tookColumn.fit("Took");
      afterColumn.fit("Stock after");
      markColumn.fit("Out");
      // "120 to 117" over "4 to 3": both numbers pad to their widest, so the "to" lines up down the column.
      size_t haveDigits = 1;
      size_t afterDigits = 1;
      for (const auto& stop : plan.stops) {
        for (const auto& pick : stop.picks) {
          const auto* item = store_.findById(pick.itemId);
          const int have = item == nullptr ? 0 : item->quantity;
          partColumn.fit(pick.designation);
          packageColumn.fit(pick.package);
          fromColumn.fit(stop.rackId.empty() ? pick.slot : stop.title + "  " + pick.slot);
          tookColumn.fit(to_string(bomPickTaken(pick)));
          haveDigits = max(haveDigits, to_string(have).size());
          afterDigits = max(afterDigits, to_string(max(0, have - bomPickTaken(pick))).size());
        }
      }
      afterColumn.fit(string(haveDigits + 4 + afterDigits, '0'));
      const auto widths =
          fitColumns({partColumn, packageColumn, fromColumn, tookColumn, afterColumn, markColumn}, screenWidth - 5);
      const int partWidth = widths[0];
      const int packageWidth = widths[1];
      const int fromWidth = widths[2];
      const int tookWidth = widths[3];
      const int afterWidth = widths[4];
      const int markWidth = widths[5];
      const auto padded = [](int number, size_t digits) {
        auto text = to_string(number);
        return string(digits > text.size() ? digits - text.size() : 0, ' ') + text;
      };
      rows.push_back(ftxui::hbox({ftxui::text("   "), tableCell("Part", partWidth, uiMutedText()),
                                  tableCell("Package", packageWidth, uiMutedText()), tableCell("From", fromWidth, uiMutedText()),
                                  tableCell("Took", tookWidth, uiMutedText(), false, true),
                                  tableCell("Stock after", afterWidth, uiMutedText(), false, true), ftxui::filler()}));
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
                         tableCell(pick.designation, partWidth, selected ? uiFocusColor() : uiPrimaryText(), true),
                         tableCell(pick.package, packageWidth, uiSecondaryText()),
                         tableCell(from, fromWidth, uiLinkColor()),
                         tableCell(to_string(took), tookWidth, took < pick.quantity ? uiWarnColor() : uiPrimaryText(), true, true),
                         ftxui::hbox({ftxui::filler(), styledText(padded(have, haveDigits) + " to ", uiMutedText()),
                                      uiHeaderText(padded(after, afterDigits), out ? uiDangerColor() : low ? uiWarnColor() : uiPrimaryText()),
                                      ftxui::text(string(kColumnGap, ' '))}) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, afterWidth),
                         tableCell(out ? "Out" : low ? "Low" : "", markWidth, out ? uiDangerColor() : uiWarnColor()),
                         ftxui::filler(),
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
    auto slotColumn = tableColumn(loose ? 12 : 5), partColumn = tableColumn(9), forColumn = tableColumn(8), takeColumn = tableColumn(4);
    slotColumn.fit(loose ? "Location" : "Slot");
    partColumn.fit("Part");
    forColumn.fit("For");
    takeColumn.fit("Take");
    for (const auto& pick : stop.picks) {
      slotColumn.fit(pick.slot);
      partColumn.fit(pick.designation);
      forColumn.fit(designatorText(pick.designators, numeric_limits<int>::max()));
      takeColumn.fit(to_string(pick.quantity));
    }
    const auto widths = fitColumns({slotColumn, partColumn, forColumn, takeColumn}, sideWidth);
    const int slotWidth = widths[0];
    const int partWidth = widths[1];
    const int forWidth = widths[2];
    const int takeWidth = widths[3];
    ftxui::Elements list;
    list.push_back(ftxui::hbox({tableCell(loose ? "Location" : "Slot", slotWidth, uiMutedText()),
                                tableCell("Part", partWidth, uiMutedText()), tableCell("For", forWidth, uiMutedText()),
                                tableCell("Take", takeWidth, uiMutedText(), false, true), ftxui::filler()}));
    for (const auto& pick : stop.picks) {
      // designatorText keeps one cell spare; the column gap needs all of its cells.
      list.push_back(ftxui::hbox({tableCell(pick.slot, slotWidth, uiFocusColor(), true),
                                  tableCell(pick.designation, partWidth, uiPrimaryText(), true),
                                  tableCell(designatorText(pick.designators, forWidth - kColumnGap + 1), forWidth, uiSecondaryText()),
                                  tableCell(to_string(pick.quantity), takeWidth,
                                            pick.quantity < pick.needed ? uiWarnColor() : uiPrimaryText(), true, true),
                                  ftxui::filler()}));
    }
    list.push_back(ftxui::text(""));
    list.push_back(ftxui::hbox({tableCell("Total", slotWidth + partWidth + forWidth, uiSecondaryText()),
                                tableCell(to_string(stop.pieces()), takeWidth, uiPrimaryText(), true, true), ftxui::filler()}));
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
    // The map shrinks its cells so every pick of the stop is on screen together.
    const InventatoryRack* stopRack = nullptr;
    for (const auto& candidate : store_.racks()) {
      if (candidate.id == stop.rackId) stopRack = &candidate;
    }
    StorageGridRequest request;
    request.letters = stopRack != nullptr ? storageGridRows(*stopRack) : kRackGridSize;
    request.numbers = stopRack != nullptr ? storageGridColumns(*stopRack) : kRackGridSize;
    request.scroll = bomMapScroll_;
    for (const auto& pick : stop.picks) {
      const auto slot = toUpper(trim(pick.slot));
      if (isValidRackSlot(slot)) request.focus.push_back({slot.front() - 'A', stoi(slot.substr(1)) - 1});
    }
    const int designatorWidth = request.numbers >= 10 ? 4 : 3;
    const int gridWidth = max(40, screenWidth - sideWidth - 4);
    request.width = gridWidth - designatorWidth;
    // The 11 rows outside the grid and its gaps: shell, page header, letter row and route line.
    request.height = max(3, screenHeight - 11);
    const auto fit = fitStorageGrid(request);
    // Up and Down scroll the map; the scroll is kept to what the map can actually move.
    {
      auto unscrolled = request;
      unscrolled.scroll = 0;
      bomMapScroll_ = fit.numberStart - fitStorageGrid(unscrolled).numberStart;
    }
    const int letterStart = fit.letterStart;
    const int numberStart = fit.numberStart;
    const int rows = fit.numberCount;
    const int columns = fit.letterCount;
    const int slotWidth2 = fit.cellWidth;
    const bool pulse = uiBlinkOn(700);

    map<string, const BomPick*> picksBySlot;
    for (const auto& pick : stop.picks) picksBySlot[toUpper(trim(pick.slot))] = &pick;

    ftxui::Elements grid;
    int gridExactWidth = designatorWidth + columns * slotWidth2 + (columns - 1);
    {
      ftxui::Elements letters{ftxui::text(string(static_cast<size_t>(designatorWidth), ' '))};
      for (int column = 0; column < columns; ++column) {
        if (column > 0) letters.push_back(ftxui::text(" "));
        letters.push_back(ftxui::hbox({ftxui::filler(), uiHeaderText(rack_page_detail::storageLetterHeader(fit, column), uiMutedText()),
                                       ftxui::filler()}) |
                          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, slotWidth2));
      }
      grid.push_back(ftxui::hbox(move(letters)));
      // While the rows scroll, a bar above and below says how many rows are off screen and which slots
      // to take from are among them. Up and Down scroll.
      const auto scrollBar = [&](bool above) {
        auto text = rack_page_detail::storageScrollText(fit, above);
        string slots;
        for (const auto& [letter, number] : fit.hiddenFocus) {
          if ((number < numberStart) != above) continue;
          slots += (slots.empty() ? "" : " ") + rackSlotLabel(letter, number);
        }
        if (!slots.empty()) text += ", take from " + slots;
        const bool more = (above ? fit.rowsAbove() : fit.rowsBelow()) > 0;
        const auto color = !slots.empty() ? uiFocusColor() : more ? uiMutedText() : uiDimColor();
        return ftxui::hbox({ftxui::text(string(static_cast<size_t>(designatorWidth), ' ')), ftxui::filler(),
                            uiHeaderText(text, color), ftxui::filler()});
      };
      if (fit.scrollBars) grid.push_back(scrollBar(true));
      const auto valueWidth = static_cast<size_t>(max(1, slotWidth2 - 2));
      for (int row = 0; row < rows; ++row) {
        if (row > 0) grid.push_back(ftxui::text(""));
        const int slotHeight = fit.rowHeight(row);
        ftxui::Elements cells;
        cells.push_back(ftxui::vbox({ftxui::filler(),
                                     ftxui::hbox({ftxui::filler(), uiHeaderText(to_string(numberStart + row + 1), uiMutedText()),
                                                  ftxui::filler()}),
                                     ftxui::filler()}) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, designatorWidth));
        for (int column = 0; column < columns; ++column) {
          if (column > 0) cells.push_back(ftxui::text(" "));
          const auto slot = rackSlotLabel(letterStart + column, numberStart + row);
          const auto found = picksBySlot.find(slot);
          // Every slot shows its number and stored value in the same place, lit or not.
          const auto* pick = found != picksBySlot.end() ? found->second : nullptr;
          const auto* item = pick != nullptr ? store_.findById(pick->itemId) : itemAtRackSlot(store_, stop.rackId, slot);
          string value = pick != nullptr ? pick->designation : string("-");
          if (item != nullptr) {
            const auto split = splitPartName(item->partName).value;
            value = split.empty() ? item->partName : split;
          }
          ftxui::Element body;
          if (pick != nullptr) {
            // Lit slots pulse between two fills; both stay lit.
            const auto bg = pulse ? uiInteractiveColor() : uiActiveBg();
            const auto fg = pulse ? uiCanvasBg() : uiPrimaryText();
            const auto slotFg = pulse ? uiCanvasBg() : uiFocusColor();
            const auto take = "Take " + to_string(pick->quantity);
            body = ftxui::vbox({
                       uiHeaderText(" " + slot, slotFg),
                       uiHeaderText(" " + ellipsize(value, valueWidth), fg),
                       ftxui::filler(),
                       uiHeaderText(" " + take, fg),
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
      if (fit.scrollBars) grid.push_back(scrollBar(false));
    }

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
