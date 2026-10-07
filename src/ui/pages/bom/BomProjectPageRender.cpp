// Inventatory - Hardware Inventory Management System
// KiCad BOM project rendering: pinned list, BOM comparison, Find in racks workflow.

#include "App.h"

#include "ui/pages/racks/RackManagementPagePrivate.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ftxui/dom/canvas.hpp>
#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;


namespace {

struct BomRackGlyph {
  char value;
  int width;
  array<const char*, 5> rows;
};

const BomRackGlyph* bomRackGlyph(char value) {
  static const BomRackGlyph glyphs[] = {
      {'R', 4, {"1110", "1001", "1110", "1010", "1001"}},
      {'A', 4, {"0110", "1001", "1111", "1001", "1001"}},
      {'C', 4, {"0111", "1000", "1000", "1000", "0111"}},
      {'K', 4, {"1001", "1010", "1100", "1010", "1001"}},
      {'0', 4, {"0110", "1001", "1011", "1101", "0110"}},
      {'1', 4, {"0100", "1100", "0100", "0100", "1110"}},
      {'2', 4, {"1110", "0001", "0110", "1000", "1111"}},
      {'3', 4, {"1110", "0001", "0110", "0001", "1110"}},
      {'4', 4, {"0010", "0110", "1010", "1111", "0010"}},
      {'5', 4, {"1111", "1000", "1110", "0001", "1110"}},
      {'6', 4, {"0110", "1000", "1110", "1001", "0110"}},
      {'7', 4, {"1111", "0001", "0010", "0100", "0100"}},
      {'8', 4, {"0110", "1001", "0110", "1001", "0110"}},
      {'9', 4, {"0110", "1001", "0111", "0001", "0110"}},
      {' ', 2, {"00", "00", "00", "00", "00"}},
  };
  for (const auto& glyph : glyphs) {
    if (glyph.value == value) return &glyph;
  }
  return nullptr;
}

bool isBomRackTitle(const string& title) {
  if (title.size() <= 5 || title.compare(0, 5, "Rack ") != 0) return false;
  return all_of(title.begin() + 5, title.end(), [](unsigned char value) {
    return isdigit(value) != 0;
  });
}

ftxui::Element bomRackTitleCanvas(const string& title, int width, ftxui::Color color) {
  constexpr int glyphHeight = 5;
  constexpr int glyphGap = 1;

  int titleWidth = 0;
  vector<const BomRackGlyph*> glyphs;
  for (const char value : title) {
    const auto* glyph = bomRackGlyph(value);
    if (glyph == nullptr) return {};
    glyphs.push_back(glyph);
    titleWidth += glyph->width;
  }
  titleWidth += max(0, static_cast<int>(glyphs.size()) - 1) * glyphGap;

  // Two cells of breathing room keep the block lettering away from the rail.
  if (titleWidth + 2 > width) return {};

  auto titleCanvas = ftxui::canvas(titleWidth * 2, glyphHeight * 4,
                                   [glyphs = move(glyphs), color, titleWidth](ftxui::Canvas& canvas) {
                                     const auto background = uiActiveBg();
                                     for (int row = 0; row < glyphHeight; ++row) {
                                       for (int column = 0; column < titleWidth; ++column) {
                                         // Canvas cells are opaque when rendered, so initialize the
                                         // whole raster before placing foreground blocks.
                                         canvas.Style(column * 2, row * 4, [background](ftxui::Cell& cell) {
                                           cell.background_color = background;
                                         });
                                       }
                                     }

                                     int x = 0;
                                     for (const auto* glyph : glyphs) {
                                       for (int row = 0; row < glyphHeight; ++row) {
                                         for (int column = 0; column < glyph->width; ++column) {
                                           if (glyph->rows[row][column] != '1') continue;
                                           // DrawBlock is a native FTXUI canvas primitive. Filling both
                                           // halves of the terminal cell keeps each raster pixel crisp.
                                           canvas.DrawBlock((x + column) * 2, row * 4, true, color);
                                           canvas.DrawBlock((x + column) * 2 + 1, row * 4, true, color);
                                           canvas.DrawBlock((x + column) * 2, row * 4 + 2, true, color);
                                           canvas.DrawBlock((x + column) * 2 + 1, row * 4 + 2, true, color);
                                         }
                                       }
                                       x += glyph->width + glyphGap;
                                     }
                                   });
  return ftxui::hbox({ftxui::filler(), move(titleCanvas), ftxui::filler()}) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Element bomStepBanner(const string& title, int width) {
  if (isBomRackTitle(title)) {
    auto largeTitle = bomRackTitleCanvas(title, width, uiFocusColor());
    if (largeTitle) return largeTitle | ftxui::bgcolor(uiActiveBg());
  }

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

// fixedCell with a bold value, for the figures a row is read by.
ftxui::Element boldCell(const string& text, int width, ftxui::Color color, bool rightAlign = false) {
  const auto clipped = ellipsize(text, static_cast<size_t>(max(0, rightAlign ? width - 1 : width)));
  auto content = rightAlign ? ftxui::hbox({ftxui::filler(), uiHeaderText(clipped, color), ftxui::text(" ")})
                            : ftxui::hbox({uiHeaderText(clipped, color), ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
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
          centered(uiHeaderText("No projects", uiPrimaryText())),
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
        fixedCell(" Project", nameWidth, uiMutedColor()),
        fixedCell("Lines", 8, uiMutedColor(), true),
        fixedCell("Boards", 8, uiMutedColor(), true),
        styledText("   ", uiDimColor()),
        fixedCell("Built", 10, uiMutedColor()),
    }) | ftxui::bgcolor(uiPanelLeftBg()));

    for (size_t index = 0; index < bomProjects_.size(); ++index) {
      const auto& project = bomProjects_[index];
      const bool selected = index == min(bomProjectSelection_, bomProjects_.size() - 1);
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto fg = selected ? uiFocusColor() : uiPrimaryText();

      // Render runs at 10 Hz, so the row count comes from a cheap newline count
      // rather than a full re-parse of the stored BOM.
      const auto lineCount = count(project.bomText.begin(), project.bomText.end(), '\n');
      auto row = ftxui::hbox({
          boldCell(" " + project.name, nameWidth, fg),
          fixedCell(to_string(max<long long>(0, lineCount - 1)), 8, uiSecondaryText(), true),
          boldCell(to_string(project.boards), 8, uiAccentColor(), true),
          styledText("   ", uiDimColor()),
          boldCell(project.lastBuilt > 0 ? "Built" : "Not yet", 10,
                   project.lastBuilt > 0 ? uiSuccessColor() : uiDimColor()),
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
      rows.push_back(fullLine("Unsaved project changes  Press R to retry saving", uiDangerColor(), uiDangerBg()));
    }
    return ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg());
  }

  const auto* project = activeBomProject();
  const auto projectName = project == nullptr ? string("Project") : project->name;

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
        uiHeaderText(bomDeductPrompt_ ? "Complete" : "Stop " + to_string(stepIndex + 1) + " of " +
                                                       to_string(steps.size()),
                     uiSecondaryText()),
        styledText("   ", uiMutedColor()),
        uiHeaderText(to_string(pickedPieces) + " / " + to_string(totalPieces), uiPrimaryText()),
        styledText(" pieces ", uiMutedColor()),
    }) | ftxui::bgcolor(uiSurfaceBg()));
    header.push_back(uiDivider());

    if (bomDeductPrompt_) {
      ftxui::Elements promptRows;
      promptRows.push_back(fullLine("Build complete.  " + to_string(totalPieces) + " pieces  " +
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
        fixedCell(" " + string(loose ? "Location" : "Slot"), slotColumn, uiMutedColor()),
        fixedCell("Part", labelColumn, uiMutedColor()),
        fixedCell("Package", detailColumn, uiMutedColor()),
        ftxui::filler(),
        fixedCell("Need", quantityColumn, uiMutedColor(), true),
    }) | ftxui::bgcolor(uiPanelLeftBg());

    ftxui::Elements sideRows;
    for (const auto& pick : step.picks) {
      sideRows.push_back(ftxui::hbox({
          boldCell(" " + pick.slot, slotColumn, uiAccentColor()),
          boldCell(ellipsize(pick.label, static_cast<size_t>(max(1, labelColumn - 1))), labelColumn, uiPrimaryText()),
          fixedCell(pick.detail, detailColumn, uiMutedColor()),
          ftxui::filler(),
          boldCell("x" + to_string(pick.quantity), quantityColumn, uiFocusColor(), true),
      }));
    }

    auto sideList = ftxui::vbox(move(sideRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                    ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;

    ftxui::Elements sideFooter;
    sideFooter.push_back(uiDivider());
    if (stepIndex + 1 < steps.size()) {
      sideFooter.push_back(fullLine("Next: " + steps[stepIndex + 1].title + "  " +
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
    // Exact width of the slot grid; the side rail takes whatever the floor division leaves over.
    int gridExactWidth = 0;

    if (loose) {
      const int mainContentWidth = max(30, screenWidth - sideWidth - 3);
      const int mainLabelColumn = max(12, mainContentWidth - 18 - quantityColumn);
      mainRows.push_back(ftxui::hbox({
          fixedCell(" Location", 18, uiMutedColor()),
          fixedCell("Part", mainLabelColumn, uiMutedColor()),
          fixedCell("Need", quantityColumn, uiMutedColor(), true),
      }) | ftxui::bgcolor(uiPanelLeftBg()));
      for (const auto& pick : step.picks) {
        mainRows.push_back(ftxui::hbox({
            boldCell(" " + pick.slot, 18, uiAccentColor()),
            boldCell(pick.label, mainLabelColumn, uiPrimaryText()),
            boldCell("x" + to_string(pick.quantity), quantityColumn,
                     blink ? uiFocusColor() : uiInteractiveColor(), true),
        }));
      }
    } else {
      // The walkthrough grid is the Racks grid: lettered columns, numbered rows, identical slots, and the
      // same slot body, with a footer band taking the rows that floor division cannot hand out evenly.
      const int rows = 5;
      const int columns = 5;
      constexpr int designatorWidth = 3;
      const int gridWidth = max(30, screenWidth - sideWidth - 3);
      const int slotSpace = gridWidth - designatorWidth - 1 - (columns - 1);
      const int slotWidth = rack_page_detail::equalRackSlotWidth(slotSpace, columns);
      // Shell and project header take seven rows; the letter header and its divider, four row dividers and the
      // footer divider take eight more with a one-row band.
      const int slotRowsSpace = max(rows * 3, screenHeight - 14);
      const int slotHeight = rack_page_detail::equalRackSlotHeight(slotRowsSpace, rows);
      gridExactWidth = designatorWidth + 1 + (columns - 1) + columns * slotWidth;
      const int footerBandRows = 1 + rack_page_detail::rackSlotRemainderRows(slotRowsSpace, rows, slotHeight);
      const auto centeredCell = [](const string& text, int width, ftxui::Color color) {
        return ftxui::hbox({ftxui::filler(), uiHeaderText(text, color), ftxui::filler()}) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
      };

      ftxui::Elements columnHeaders;
      columnHeaders.push_back(centeredCell("", designatorWidth, uiDimColor()));
      columnHeaders.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
      for (int column = 0; column < columns; ++column) {
        columnHeaders.push_back(centeredCell(string(1, static_cast<char>('A' + column)), slotWidth, uiAccentColor()));
        if (column + 1 < columns) columnHeaders.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
      }
      mainRows.push_back(ftxui::hbox(move(columnHeaders)) | ftxui::bgcolor(uiPanelRightBg()));
      mainRows.push_back(uiDivider());

      for (int row = 0; row < rows; ++row) {
        ftxui::Elements rowCells;
        rowCells.push_back(ftxui::vbox({ftxui::filler(), centeredCell(to_string(row + 1), designatorWidth, uiAccentColor()),
                                        ftxui::filler()}) |
                           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, designatorWidth) |
                           ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, slotHeight));
        rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        for (int column = 0; column < columns; ++column) {
          const auto slot = rackSlotLabel(column, row);
          const auto* item = itemAtRackSlot(store_, step.rackId, slot);
          const bool lit = litSlots.count(slot) != 0;
          // A lit slot pulses between a filled highlight and the resting
          // surface, so the eye lands on exactly what to open.
          const auto bg = lit ? (blink ? bomLitSlotBg() : uiSurfaceBg()) : uiCanvasBg();
          ftxui::Elements cellRows;
          if (item == nullptr) {
            cellRows.push_back(ftxui::filler());
            cellRows.push_back(ftxui::hbox({ftxui::filler(), styledText("empty", uiDimColor()), ftxui::filler()}) |
                               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, slotWidth));
            cellRows.push_back(ftxui::filler());
          } else {
            cellRows.push_back(rack_page_detail::rackCellBody(*item, lit && blink, settings_.lowStockThreshold,
                                                              slotWidth, slotHeight));
          }
          rowCells.push_back(ftxui::vbox(move(cellRows)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, slotWidth) |
                             ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, slotHeight) | ftxui::bgcolor(bg));
          if (column + 1 < columns) rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        }
        mainRows.push_back(ftxui::hbox(move(rowCells)));
        if (row + 1 < rows) mainRows.push_back(uiDivider());
      }
      mainRows.push_back(uiDivider());
      mainRows.push_back(ftxui::vbox({ftxui::filler(),
                                      ftxui::hbox({styledText(" " + step.title + "  ", uiMutedColor()),
                                                   uiHeaderText(to_string(litSlots.size()) +
                                                                    (litSlots.size() == 1 ? " slot to open" : " slots to open"),
                                                                uiAccentColor()),
                                                   ftxui::filler()}),
                                      ftxui::filler()}) |
                         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, footerBandRows) | ftxui::bgcolor(uiPanelRightBg()));
    }

    auto mainPanel = ftxui::vbox(move(mainRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                     ftxui::bgcolor(uiSurfaceBg());
    mainPanel = gridExactWidth > 0 ? move(mainPanel) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, gridExactWidth)
                                   : move(mainPanel) | ftxui::flex;
    auto sidePanel = ftxui::vbox({
                         bomStepBanner(step.title, sideWidth),
                         uiDivider(),
                         move(sideListHeader),
                         move(sideList),
                         move(ftxui::vbox(move(sideFooter))),
                     }) |
                     ftxui::bgcolor(uiSurfaceBg());
    sidePanel = gridExactWidth > 0 ? move(sidePanel) | ftxui::flex
                                   : move(sidePanel) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, sideWidth);

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
  const int indentWidth = 3;
  const int partWidth = clamp(tableWidth / 4, 24, 34);
  const int packageWidth = clamp(tableWidth / 8, 14, 20);
  const int needWidth = 7;
  const int haveWidth = 7;
  const int statusWidth = 11;
  const int detailWidth = max(18, tableWidth - partWidth - packageWidth - needWidth - haveWidth - statusWidth);

  ftxui::Elements headerRows;
  ftxui::Elements headerCounts;
  headerCounts.push_back(uiHeaderText(to_string(bomAnalysis_.readyCount) + " ready", uiSuccessColor()));
  headerCounts.push_back(ftxui::text("   "));
  headerCounts.push_back(uiHeaderText(to_string(bomAnalysis_.shortCount) + " missing ",
                                      bomAnalysis_.shortCount == 0 ? uiSuccessColor() : uiDangerColor()));
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
      styledText("  ", uiMutedColor()),
      target(styledText(" Find in racks  f ", uiCanvasBg(), uiInteractiveColor()),
             "bom.find-in-racks", UiTargetKind::Button, [self] { self->beginBomBuild(); }),
      ftxui::filler(),
      ftxui::hbox(move(headerCounts)),
  }) | ftxui::bgcolor(uiSurfaceBg()));
  headerRows.push_back(uiDivider());
  if (bomProjectsDirty_) {
    headerRows.push_back(fullLine("Unsaved project changes  Press R to retry saving", uiDangerColor(), uiDangerBg()));
    headerRows.push_back(uiDivider());
  }
  ftxui::Elements tableRows;
  tableRows.push_back(ftxui::hbox({
      fixedCell(" Part", partWidth, uiMutedColor()),
      fixedCell("Package", packageWidth, uiMutedColor()),
      fixedCell("Where / suggested match", detailWidth, uiMutedColor()),
      fixedCell("Need", needWidth, uiMutedColor(), true),
      fixedCell("Have", haveWidth, uiMutedColor(), true),
      fixedCell("Status", statusWidth, uiMutedColor(), true),
  }) | ftxui::bgcolor(uiPanelLeftBg()));

  // Group headers carry how many lines they hold, so the totals read without counting rows.
  map<string, int> groupCounts;
  for (const auto& match : bomAnalysis_.matches) {
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const auto category = toLower(bomComparisonCategory(line, match, store_.items()));
    ++groupCounts[(match.sufficient ? "s|" : "m|") + category];
    ++groupCounts[match.sufficient ? "s" : "m"];
  }

  const auto groupRow = [&](const string& label, int count, ftxui::Color color, bool nested) {
    return ftxui::hbox({
               ftxui::text(nested ? "  " : " "),
               uiHeaderText(label, color),
               styledText("  " + to_string(count), uiDimColor()),
               ftxui::filler(),
           }) |
           ftxui::bgcolor(uiRaisedSurfaceBg());
  };

  string previousAvailability;
  string previousCategory;
  for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
    const auto& match = bomAnalysis_.matches[index];
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const bool selected = index == min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
    const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
    const auto fg = selected ? uiFocusColor() : uiPrimaryText();
    const auto package = packageFromFootprint(line.footprint);
    const auto* item = store_.findById(match.chosenItemId());
    const auto availability = match.sufficient ? string("In Stock") : string("Missing");
    const auto category = bomComparisonCategory(line, match, store_.items());
    const string groupKey = match.sufficient ? "s" : "m";
    if (availability != previousAvailability) {
      if (!previousAvailability.empty()) {
        tableRows.push_back(ftxui::text("") | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1) |
                            ftxui::bgcolor(uiSurfaceBg()));
      }
      tableRows.push_back(groupRow(availability, groupCounts[groupKey],
                                   match.sufficient ? uiSuccessColor() : uiDangerColor(), false));
      previousAvailability = availability;
      previousCategory.clear();
    }
    if (toLower(category) != toLower(previousCategory)) {
      tableRows.push_back(groupRow(category, groupCounts[groupKey + "|" + toLower(category)], uiSecondaryText(), true));
      previousCategory = category;
    }
    string detail = "-";
    if (match.sufficient) {
      const auto slot = item == nullptr ? string() : rackLocation(*item, store_.racks());
      detail = slot.empty() ? (item == nullptr ? string("-") : item->location) : slot;
    } else if (project != nullptr) {
      const auto lineKey = bomLineKey(line);
      const auto found = project->enrichment.find(lineKey);
      if (bomEnrichmentCached(project->enrichment, lineKey)) detail = found->second;
      else if (bomEnrichmentFuture_.valid() && lineKey == bomEnrichmentActiveKey_) detail = "Looking up";
      else if (find(bomEnrichmentQueue_.begin(), bomEnrichmentQueue_.end(), lineKey) !=
               bomEnrichmentQueue_.end()) detail = "In lookup queue";
    }

    // Have is the number that matters: red when nothing is in stock, amber when some but not enough.
    const auto haveColor = match.sufficient ? uiPrimaryText()
                           : match.available <= 0 ? uiDangerColor()
                                                  : uiWarnColor();
    const int shortBy = max(0, match.needed - match.available);
    const bool noMatch = detail == "(no match)" || detail == "-";
    const auto detailColor = match.sufficient ? uiAccentColor() : noMatch ? uiDimColor() : uiLinkColor();

    auto row = ftxui::hbox({
        ftxui::text(string(static_cast<size_t>(indentWidth), ' ')),
        ftxui::hbox({uiHeaderText(ellipsize(line.designation, static_cast<size_t>(max(1, partWidth - indentWidth - 1))), fg),
                     ftxui::filler()}) |
            ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partWidth - indentWidth),
        fixedCell(ellipsize(package, static_cast<size_t>(max(1, packageWidth - 2))), packageWidth,
                  selected ? uiTitleColor() : uiSecondaryText()),
        fixedCell(detail, detailWidth, detailColor),
        fixedCell(to_string(match.needed), needWidth, uiMutedColor(), true),
        boldCell(to_string(match.available), haveWidth, haveColor, true),
        boldCell(match.sufficient ? "Ready" : "Short " + to_string(shortBy), statusWidth,
                 match.sufficient ? uiSuccessColor() : haveColor, true),
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
