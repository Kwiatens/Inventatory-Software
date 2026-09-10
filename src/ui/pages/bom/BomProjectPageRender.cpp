// Inventatory - Hardware Inventory Management System
// KiCad BOM project rendering: pinned list, BOM comparison, Find in racks workflow.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;


namespace {

ftxui::Element bomCell(const string& text, int width, ftxui::Color color, bool rightAlign = false) {
  const auto clipped = ellipsize(text, static_cast<size_t>(max(0, rightAlign ? width - 1 : width)));
  auto content = rightAlign
                     ? ftxui::hbox({ftxui::filler(), styledText(clipped, color), ftxui::text(" ")})
                     : ftxui::hbox({styledText(clipped, color), ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Element centered(ftxui::Element element) {
  return ftxui::hbox({ftxui::filler(), move(element), ftxui::filler()});
}

ftxui::Element bomStepBanner(const string& title, int width) {
  return ftxui::hbox({
             ftxui::filler(),
             uiHeaderText(" " + title + " ", uiFocusColor()),
             ftxui::filler(),
         }) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width) |
         ftxui::bgcolor(uiActiveBg());
}

// Pulse fill for a rack slot the current build needs opened. Kept page-local
// alongside the rack page's own state backgrounds.
ftxui::Color bomLitSlotBg() {
  return uiActiveBg();
}

}  // namespace

ftxui::Element App::renderBomProjectUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 40;
  auto self = const_cast<App*>(this);

  // ---------------------------------------------------------------- list ---
  if (bomView_ == BomView::List || !bomAnalysisValid_) {
    if (bomProjects_.empty()) {
      return ftxui::vbox({
          ftxui::filler(),
          centered(uiHeaderText("NO PROJECTS", uiPrimaryText())),
          centered(styledText("Import a KiCad BOM from the Import page", uiSecondaryText())),
          ftxui::text(""),
          centered(target(styledText(" Import a BOM ", uiInteractiveColor(), uiRaisedSurfaceBg()), "bom.import",
                          UiTargetKind::Button, [self] { self->beginCsvImport(); })),
          ftxui::filler(),
      });
    }

    const int nameWidth = clamp(screenWidth / 3, 24, 44);
    ftxui::Elements rows;
    rows.push_back(ftxui::hbox({
        bomCell(" Project", nameWidth, uiMutedColor()),
        bomCell("Lines", 8, uiMutedColor(), true),
        bomCell("Boards", 8, uiMutedColor(), true),
        styledText("   ", uiDimColor()),
        bomCell("Built", 10, uiMutedColor()),
    }) | ftxui::bgcolor(uiPanelLeftBg()));

    for (size_t index = 0; index < bomProjects_.size(); ++index) {
      const auto& project = bomProjects_[index];
      const bool selected = index == min(bomProjectSelection_, bomProjects_.size() - 1);
      const auto bg = selected ? uiSelectionBg() : (index % 2 == 0 ? uiRowDarkBg() : uiRowLightBg());
      const auto fg = selected ? uiFocusColor() : uiPrimaryText();

      // Render runs at 10 Hz, so the row count comes from a cheap newline count
      // rather than a full re-parse of the stored BOM.
      const auto lineCount = count(project.bomText.begin(), project.bomText.end(), '\n');
      auto row = ftxui::hbox({
          bomCell(" " + project.name, nameWidth, fg),
          bomCell(to_string(max<long long>(0, lineCount - 1)), 8, uiSecondaryText(), true),
          bomCell(to_string(project.boards), 8, uiAccentColor(), true),
          styledText("   ", uiDimColor()),
          bomCell(project.lastBuilt > 0 ? "yes" : "-", 10, project.lastBuilt > 0 ? uiSuccessColor() : uiDimColor()),
      }) | ftxui::bgcolor(bg);
      if (selected) {
        row = row | ftxui::select;
      }
      rows.push_back(target(row, "bom.project." + project.id, UiTargetKind::Row, [self, index] {
        self->bomProjectSelection_ = index;
        self->openSelectedBomProject();
      }));
    }

    rows.insert(rows.begin(), ftxui::hbox({
        uiHeaderText(" Projects ", uiPrimaryText()),
        styledText(to_string(bomProjects_.size()) +
                       (bomProjects_.size() == 1 ? " project" : " projects"),
                   uiMutedColor()),
        ftxui::filler(),
        target(styledText(" Import BOM  i ", uiInteractiveColor(), uiRaisedSurfaceBg()), "bom.import",
               UiTargetKind::Button, [self] { self->beginCsvImport(); }),
    }) | ftxui::bgcolor(uiSurfaceBg()));
    if (bomProjectsDirty_) {
      rows.push_back(fullLine("UNSAVED PROJECT CHANGES  Press R to retry saving", uiDangerColor(), uiDangerBg()));
    }
    return ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg());
  }

  const auto* project = activeBomProject();
  const auto projectName = project == nullptr ? string("PROJECT") : project->name;

  // --------------------------------------------------------------- build ---
  if (bomView_ == BomView::Build) {
    const auto steps = bomBuildSteps();
    const auto stepIndex = steps.empty() ? 0 : min(bomBuildStep_, steps.size() - 1);

    int pickedPieces = 0;
    int totalPieces = 0;
    for (size_t index = 0; index < steps.size(); ++index) {
      for (const auto& pick : steps[index].picks) {
        totalPieces += pick.quantity;
        if (index < stepIndex || bomDeductPrompt_) {
          pickedPieces += pick.quantity;
        }
      }
    }
    ftxui::Elements header;
    header.push_back(ftxui::hbox({
        uiHeaderText(" " + projectName + " ", uiPrimaryText()),
        ftxui::filler(),
        styledText(bomDeductPrompt_ ? "Complete" : "Stop " + to_string(stepIndex + 1) + " of " +
                                                     to_string(steps.size()),
                   uiSecondaryText()),
        styledText("  ·  " + to_string(pickedPieces) + " / " + to_string(totalPieces) + " pieces ",
                   uiMutedColor()),
    }) | ftxui::bgcolor(uiSurfaceBg()));
    header.push_back(uiDivider());

    if (bomDeductPrompt_) {
      ftxui::Elements promptRows;
      promptRows.push_back(fullLine("Build complete.  " + to_string(totalPieces) + " pieces · " +
                                        to_string(steps.size()) + " stops",
                                    uiTitleColor(), uiPanelRightBg()));
      promptRows.push_back(uiDivider());
      if (bomBuildReady(bomAnalysis_)) {
        promptRows.push_back(fullLine("Subtract these from stock?", uiAccentColor(), uiPanelRightBg()));
        promptRows.push_back(ftxui::hbox({
            target(styledText(" y  subtract ", uiInteractiveColor(), uiRaisedSurfaceBg()), "bom.deduct.yes",
                   UiTargetKind::Button, [self] { self->finishBomBuild(true); }),
            ftxui::text("  "),
            target(styledText(" n  keep stock ", uiSecondaryText(), uiRaisedSurfaceBg()), "bom.deduct.no",
                   UiTargetKind::Button, [self] { self->finishBomBuild(false); }),
            ftxui::filler(),
        }));
      } else {
        promptRows.push_back(fullLine("Shortages appeared while walking the BOM; completion is disabled.",
                                      uiDangerColor(), uiPanelRightBg()));
        promptRows.push_back(fullLine("Press Escape to return to the shortage list.", uiMutedColor(), uiPanelRightBg()));
      }

      auto prompt = panel("Find in racks", move(promptRows), uiAccentColor(), uiAccentColor()) |
                    ftxui::bgcolor(uiPanelRightBg()) |
                    ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, max(48, min(screenWidth - 8, 72)));
      return ftxui::vbox({
          ftxui::vbox(move(header)),
          ftxui::filler(),
          centered(move(prompt)),
          ftxui::filler(),
      });
    }

    const auto& step = steps[stepIndex];
    const bool loose = step.rackId.empty();
    const bool blink = uiBlinkOn(700);

    // Slots holding something this build needs.
    set<string> litSlots;
    for (const auto& pick : step.picks) {
      litSlots.insert(pick.slot);
    }

    // The loose-item summary needs more room for its location and package
    // columns than a rack stop does. Keep the rail wide enough to preserve
    // those labels without taking the whole workspace away from the main pane.
    const int sideWidth = loose ? clamp(screenWidth / 3, 54, 64) : clamp(screenWidth / 3, 42, 52);
    const int sideContentWidth = max(20, sideWidth - 1);  // reserve the rail's scroll marker column
    const int slotColumn = loose ? clamp(sideWidth / 3, 14, 18) : 8;
    const int quantityColumn = 8;
    const int detailColumn = loose ? clamp(sideWidth / 5, 10, 14) : clamp(sideWidth / 5, 8, 12);
    const int labelColumn = max(8, sideContentWidth - slotColumn - quantityColumn - detailColumn - 1);
    auto sideListHeader = ftxui::hbox({
        bomCell(" " + string(loose ? "Location" : "Slot"), slotColumn, uiMutedColor()),
        bomCell("Part", labelColumn, uiMutedColor()),
        bomCell("Package", detailColumn, uiMutedColor()),
        ftxui::filler(),
        bomCell("Need", quantityColumn, uiMutedColor(), true),
    }) | ftxui::bgcolor(uiPanelLeftBg());

    ftxui::Elements sideRows;
    for (const auto& pick : step.picks) {
      sideRows.push_back(ftxui::hbox({
          bomCell(" " + pick.slot, slotColumn, uiAccentColor()),
          bomCell(pick.label, labelColumn, uiPrimaryText()),
          bomCell(pick.detail, detailColumn, uiMutedColor()),
          ftxui::filler(),
          bomCell("x" + to_string(pick.quantity), quantityColumn, uiFocusColor(), true),
      }));
    }

    auto sideList = ftxui::vbox(move(sideRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                    ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;

    ftxui::Elements sideFooter;
    sideFooter.push_back(uiDivider());
    if (stepIndex + 1 < steps.size()) {
      sideFooter.push_back(fullLine("Next: " + steps[stepIndex + 1].title + " · " +
                                        to_string(steps[stepIndex + 1].picks.size()) + " slots",
                                    uiMutedColor(), uiSurfaceBg()));
    } else {
      sideFooter.push_back(fullLine("End of pick list", uiMutedColor(), uiSurfaceBg()));
    }
    sideFooter.push_back(ftxui::hbox({
        target(styledText(" Next stop  Enter ", uiCanvasBg(), uiInteractiveColor()), "bom.build.next",
               UiTargetKind::Button, [self] { self->advanceBomBuild(1); }),
        ftxui::text(" "),
        target(styledText(" Back  Bksp ", uiSecondaryText(), uiRaisedSurfaceBg()), "bom.build.back",
               UiTargetKind::Button, [self] { self->advanceBomBuild(-1); }),
        ftxui::filler(),
    }));

    ftxui::Elements mainRows;

    if (loose) {
      const int mainContentWidth = max(30, screenWidth - sideWidth - 3);
      const int mainLabelColumn = max(12, mainContentWidth - 18 - quantityColumn);
      mainRows.push_back(ftxui::hbox({
          bomCell(" Location", 18, uiMutedColor()),
          bomCell("Part", mainLabelColumn, uiMutedColor()),
          bomCell("Need", quantityColumn, uiMutedColor(), true),
      }) | ftxui::bgcolor(uiPanelLeftBg()));
      for (const auto& pick : step.picks) {
        mainRows.push_back(ftxui::hbox({
            bomCell(" " + pick.slot, 18, uiMutedColor()),
            bomCell(pick.label, mainLabelColumn, uiPrimaryText()),
            bomCell("x" + to_string(pick.quantity), quantityColumn,
                    blink ? uiFocusColor() : uiInteractiveColor(), true),
        }));
      }
    } else {
      const auto rackIt = find_if(store_.racks().begin(), store_.racks().end(),
                                  [&](const InventatoryRack& candidate) { return candidate.id == step.rackId; });
      // Honour the rack's own geometry rather than assuming a 5x5 grid.
      const int rows = rackIt == store_.racks().end() ? 5 : max(1, rackIt->rows);
      const int columns = rackIt == store_.racks().end() ? 5 : max(1, rackIt->columns);
      const int gridWidth = max(30, screenWidth - sideWidth - 3);
      const int slotSpace = gridWidth - (columns - 1);
      const int slotWidth = max(7, slotSpace / columns);
      const int extraColumns = max(0, slotSpace - slotWidth * columns);
      const int slotHeight = max(2, (screenHeight - 8) / max(1, rows));

      for (int row = 0; row < rows; ++row) {
        ftxui::Elements rowCells;
        for (int column = 0; column < columns; ++column) {
          const auto slot = string(1, static_cast<char>('A' + row)) + to_string(column + 1);
          const auto* item = itemAtRackSlot(store_, step.rackId, slot);
          const bool lit = litSlots.count(slot) != 0;
          // A lit slot pulses between a filled highlight and the resting
          // surface, so the eye lands on exactly what to open.
          const auto bg = lit ? (blink ? bomLitSlotBg() : uiSurfaceBg()) : uiCanvasBg();
          const auto slotColor = lit ? (blink ? uiPrimaryText() : uiSuccessColor())
                                     : (item == nullptr ? uiDimColor() : uiMutedColor());
          const auto bodyColor = lit ? (blink ? uiPrimaryText() : uiSecondaryText()) : uiDimColor();

          ftxui::Elements cellRows;
          cellRows.push_back(centered(uiHeaderText(slot, slotColor)));
          cellRows.push_back(ftxui::paragraphAlignLeft(item == nullptr ? string("--") : item->partName) |
                             ftxui::color(bodyColor));
          const int cellWidth = slotWidth + (column < extraColumns ? 1 : 0);
          rowCells.push_back(ftxui::vbox(move(cellRows)) | ftxui::bgcolor(bg) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth) |
                             ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, slotHeight));
          if (column + 1 < columns) {
            rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
          }
        }
        mainRows.push_back(ftxui::hbox(move(rowCells)));
        if (row + 1 < rows) {
          mainRows.push_back(uiDivider());
        }
      }
    }

    auto mainPanel = ftxui::vbox(move(mainRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                     ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
    auto sidePanel = ftxui::vbox({
                         bomStepBanner(step.title, sideWidth),
                         uiDivider(),
                         move(sideListHeader),
                         move(sideList),
                         move(ftxui::vbox(move(sideFooter))),
                     }) |
                     ftxui::bgcolor(uiSurfaceBg()) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, sideWidth);

    return ftxui::vbox({
        ftxui::vbox(move(header)),
        ftxui::hbox({
            move(mainPanel),
            ftxui::separator() | ftxui::color(uiDimColor()),
            move(sidePanel),
        }) | ftxui::flex,
    });
  }

  // ------------------------------------------------------------ compare ---
  // The open-project screen has one job: make the stock comparison readable
  // and put the rack workflow at the point where the user needs it.
  const int tableWidth = screenWidth;
  const int partWidth = clamp(tableWidth / 4, 24, 34);
  const int packageWidth = clamp(tableWidth / 8, 12, 16);
  const int quantityWidth = 9;
  const int statusWidth = 14;
  const int detailWidth = max(18, tableWidth - partWidth - packageWidth -
                                      quantityWidth * 2 - statusWidth);

  ftxui::Elements headerRows;
  headerRows.push_back(ftxui::hbox({
      uiHeaderText(" " + projectName + " ", uiPrimaryText()),
      styledText("  ", uiMutedColor()),
      target(styledText(" - ", uiInteractiveColor(), uiRaisedSurfaceBg()), "bom.boards.less",
             UiTargetKind::Button, [self] { self->adjustBomBoards(-1); }),
      target(styledText(" " + to_string(bomAnalysis_.boards) +
                            (bomAnalysis_.boards == 1 ? " board " : " boards "),
                        uiFocusColor(), uiRaisedSurfaceBg()),
             "bom.boards", UiTargetKind::Button, [self] { self->adjustBomBoards(1); }),
      target(styledText(" + ", uiInteractiveColor(), uiRaisedSurfaceBg()), "bom.boards.more",
             UiTargetKind::Button, [self] { self->adjustBomBoards(1); }),
      ftxui::filler(),
      styledText(to_string(bomAnalysis_.readyCount) + " ready", uiSuccessColor()),
      styledText("  ·  ", uiDimColor()),
      styledText(to_string(bomAnalysis_.shortCount) + " missing ",
                 bomAnalysis_.shortCount == 0 ? uiSuccessColor() : uiWarnColor()),
  }) | ftxui::bgcolor(uiSurfaceBg()));
  headerRows.push_back(uiDivider());
  if (bomProjectsDirty_) {
    headerRows.push_back(fullLine("UNSAVED PROJECT CHANGES  Press R to retry saving", uiDangerColor(), uiDangerBg()));
    headerRows.push_back(uiDivider());
  }

  headerRows.push_back(ftxui::hbox({
      styledText("Compare BOM with stock", uiSecondaryText()),
      styledText("  ·  " + to_string(bomAnalysis_.lines.size()) + " lines", uiMutedColor()),
      ftxui::filler(),
      target(styledText(" Find in racks  f ", uiCanvasBg(), uiInteractiveColor()),
             "bom.find-in-racks", UiTargetKind::Button, [self] { self->beginBomBuild(); }),
  }) | ftxui::bgcolor(uiPanelRightBg()));
  headerRows.push_back(uiDivider());

  ftxui::Elements tableRows;
  tableRows.push_back(ftxui::hbox({
      bomCell(" Part", partWidth, uiMutedColor()),
      bomCell("Package", packageWidth, uiMutedColor()),
      bomCell("Need", quantityWidth, uiMutedColor(), true),
      bomCell("Have", quantityWidth, uiMutedColor(), true),
      bomCell("Status", statusWidth, uiMutedColor()),
      bomCell("Where / suggested match", detailWidth, uiMutedColor()),
  }) | ftxui::bgcolor(uiPanelLeftBg()));

  for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
    const auto& match = bomAnalysis_.matches[index];
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const bool selected = index == min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
    const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
    const auto fg = selected ? uiFocusColor() : uiPrimaryText();
    const auto package = packageFromFootprint(line.footprint);
    const auto* item = store_.findById(match.chosenItemId());
    string detail = "-";
    if (match.sufficient) {
      const auto slot = item == nullptr ? string() : rackLocation(*item, store_.racks());
      detail = slot.empty() ? (item == nullptr ? string("-") : item->location) : slot;
    } else if (project != nullptr) {
      const auto lineKey = bomLineKey(line);
      const auto found = project->enrichment.find(lineKey);
      if (found != project->enrichment.end()) detail = found->second;
      else if (bomEnrichmentFuture_.valid() && lineKey == bomEnrichmentActiveKey_) detail = "Looking up";
      else if (find(bomEnrichmentQueue_.begin(), bomEnrichmentQueue_.end(), lineKey) !=
               bomEnrichmentQueue_.end()) detail = "In lookup queue";
    }

    auto row = ftxui::hbox({
        bomCell(line.designation, partWidth, fg),
        bomCell(package, packageWidth, selected ? uiTitleColor() : uiSecondaryText()),
        bomCell(to_string(match.needed), quantityWidth,
                match.sufficient ? uiMutedColor() : uiDangerColor(), true),
        bomCell(to_string(match.available), quantityWidth,
                match.sufficient ? uiSuccessColor() : uiMutedColor(), true),
        bomCell(match.sufficient ? "READY" : "MISSING", statusWidth,
                match.sufficient ? uiSuccessColor() : uiDangerColor()),
        bomCell(detail, detailWidth, match.sufficient ? uiAccentColor() : uiLinkColor()),
    }) | ftxui::bgcolor(bg);
    if (selected) row = row | ftxui::select;
    row = target(row, "bom.line." + to_string(index), UiTargetKind::Row, [self, index] {
      self->bomSplitSelection_ = index;
      self->dirty_ = true;
    });
    tableRows.push_back(move(row));
  }

  auto table = ftxui::vbox(move(tableRows)) | ftxui::yframe | ftxui::vscroll_indicator |
               ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex | ftxui::reflect(bomTableBounds_);
  return ftxui::vbox({ftxui::vbox(move(headerRows)), move(table)}) | ftxui::flex;
}

}  // namespace inventatory
