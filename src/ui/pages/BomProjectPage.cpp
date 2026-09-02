// Inventatory - Hardware Inventory Management System
// KiCad BOM project rendering: pinned list, have/need split, build walkthrough.

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

    rows.insert(rows.begin(), fullLine("PROJECTS  " + to_string(bomProjects_.size()), uiSecondaryText(),
                                       uiSurfaceBg()));
    if (bomProjectsDirty_) {
      rows.push_back(fullLine("UNSAVED PROJECT CHANGES  Press R to retry saving", uiDangerColor(), uiDangerBg()));
    }
    rows.push_back(uiDivider());
    rows.push_back(fullLine("Enter open   d forget   i import a BOM", uiMutedColor(), uiSurfaceBg()));
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
    const double progress = totalPieces == 0 ? 0.0 : static_cast<double>(pickedPieces) / totalPieces;
    const int barWidth = clamp(screenWidth / 3, 18, 48);

    ftxui::Elements header;
    header.push_back(ftxui::hbox({
        styledText(" BUILD  ", uiSecondaryText()),
        uiHeaderText(projectName, uiFocusColor()),
        styledText("   ", uiDimColor()),
        styledText(bomDeductPrompt_ ? string("complete")
                                    : "stop " + to_string(stepIndex + 1) + "/" + to_string(steps.size()),
                   uiMutedColor()),
        ftxui::filler(),
        uiProgressBar(progress, barWidth, uiInteractiveColor()),
        styledText(" " + to_string(pickedPieces), uiSuccessColor()),
        styledText("/" + to_string(totalPieces) + " ", uiMutedColor()),
    }) | ftxui::bgcolor(uiSurfaceBg()));
    header.push_back(uiDivider());
    if (!bomBuildReady(bomAnalysis_)) {
      header.push_back(fullLine("BUILD BLOCKED  " + to_string(bomAnalysis_.shortCount) +
                                    " BOM line(s) still unmatched or short; walkthrough only",
                                uiDangerColor(), uiSurfaceBg()));
      header.push_back(uiDivider());
    }

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

      auto prompt = panel("Build", move(promptRows), uiAccentColor(), uiAccentColor()) |
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

    const int sideWidth = clamp(screenWidth / 3, 32, 46);
    ftxui::Elements sideRows;
    sideRows.push_back(fullLine("TAKE OUT", uiSecondaryText(), uiSurfaceBg()));
    sideRows.push_back(uiDivider());
    const int slotColumn = 10;
    const int quantityColumn = 7;
    const int detailColumn = clamp(sideWidth / 5, 6, 12);
    const int labelColumn = max(8, sideWidth - slotColumn - quantityColumn - detailColumn - 1);
    for (const auto& pick : step.picks) {
      sideRows.push_back(ftxui::hbox({
          bomCell(" " + pick.slot, slotColumn, uiAccentColor()),
          bomCell(pick.label, labelColumn, uiPrimaryText()),
          bomCell(pick.detail, detailColumn, uiMutedColor()),
          ftxui::filler(),
          bomCell("x" + to_string(pick.quantity), quantityColumn, uiFocusColor(), true),
      }));
    }
    sideRows.push_back(uiDivider());
    sideRows.push_back(fullLine(to_string(step.picks.size()) +
                                    (step.picks.size() == 1 ? " slot · " : " slots · ") +
                                    to_string(bomAnalysis_.boards) +
                                    (bomAnalysis_.boards == 1 ? " board" : " boards"),
                                uiMutedColor(), uiSurfaceBg()));
    sideRows.push_back(uiDivider());
    for (size_t index = 0; index < steps.size(); ++index) {
      const bool done = index < stepIndex;
      const bool current = index == stepIndex;
      const auto marker = done ? string("+ ") : current ? string("> ") : string("  ");
      const auto color = done ? uiSuccessColor() : current ? uiFocusColor() : uiMutedColor();
      sideRows.push_back(fullLine(marker + steps[index].title + "  " +
                                      to_string(steps[index].picks.size()) +
                                      (steps[index].picks.size() == 1 ? " slot" : " slots"),
                                  color, uiSurfaceBg()));
    }

    ftxui::Elements mainRows;
    mainRows.push_back(fullLine(step.title + (step.subtitle.empty() ? "" : "  " + step.subtitle),
                                uiAccentColor(), uiPanelRightBg()));
    mainRows.push_back(uiDivider());

    if (loose) {
      for (const auto& pick : step.picks) {
        mainRows.push_back(ftxui::hbox({
            bomCell(" " + pick.slot, 18, uiMutedColor()),
            bomCell(pick.label, max(12, screenWidth - sideWidth - 34), uiPrimaryText()),
            ftxui::filler(),
            bomCell("x" + to_string(pick.quantity), 8, blink ? uiFocusColor() : uiInteractiveColor(), true),
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
      const int slotHeight = max(2, (screenHeight - 12) / max(1, rows));

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
    auto sidePanel = ftxui::vbox(move(sideRows)) | ftxui::bgcolor(uiSurfaceBg()) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, sideWidth);

    return ftxui::vbox({
        ftxui::vbox(move(header)),
        ftxui::hbox({
            move(mainPanel),
            ftxui::separator() | ftxui::color(uiDimColor()),
            move(sidePanel),
        }) | ftxui::flex,
        uiDivider(),
        fullLine("Enter next rack   Backspace back   esc exit", uiMutedColor(), uiSurfaceBg()),
    });
  }

  // --------------------------------------------------------------- split ---
  const int lineCount = static_cast<int>(bomAnalysis_.matches.size());
  const double readyFraction = lineCount == 0 ? 0.0 : static_cast<double>(bomAnalysis_.readyCount) / lineCount;
  const double shortFraction = lineCount == 0 ? 0.0 : static_cast<double>(bomAnalysis_.shortCount) / lineCount;
  const int percent = lineCount == 0 ? 0 : static_cast<int>(readyFraction * 100.0 + 0.5);

  ftxui::Elements headerRows;
  headerRows.push_back(ftxui::hbox({
      uiHeaderText(" " + projectName + " ", uiPrimaryText()),
      styledText("  boards ", uiMutedColor()),
      target(styledText(" x" + to_string(bomAnalysis_.boards) + " ", uiFocusColor(), uiRaisedSurfaceBg()),
             "bom.boards", UiTargetKind::Button, [self] { self->adjustBomBoards(1); }),
      styledText(" +/- ", uiInteractiveColor()),
      ftxui::filler(),
      styledText(to_string(bomAnalysis_.readyCount) + " ready", uiSuccessColor()),
      styledText("  ·  ", uiDimColor()),
      styledText(to_string(bomAnalysis_.shortCount) + " short", uiDangerColor()),
      styledText("  ·  " + to_string(percent) + "% ", uiMutedColor()),
  }) | ftxui::bgcolor(uiSurfaceBg()));
  headerRows.push_back(uiSplitProgressBar(readyFraction, shortFraction, max(20, screenWidth - 2),
                                          uiInteractiveColor(), uiDangerColor()));
  headerRows.push_back(uiDivider());
  if (bomProjectsDirty_) {
    headerRows.push_back(fullLine("UNSAVED PROJECT CHANGES  Press R to retry saving", uiDangerColor(), uiDangerBg()));
    headerRows.push_back(uiDivider());
  }
  if (!bomBuildReady(bomAnalysis_)) {
    headerRows.push_back(fullLine("BUILD BLOCKED  Resolve every shortage before completion or stock deduction",
                                  uiDangerColor(), uiSurfaceBg()));
    headerRows.push_back(uiDivider());
  }

  const int halfWidth = max(30, (screenWidth - 3) / 2);
  const int valueWidth = clamp(halfWidth / 3, 12, 22);
  const int packageWidth = clamp(halfWidth / 5, 8, 14);

  ftxui::Elements readyRows;
  ftxui::Elements shortRows;
  readyRows.push_back(fullLine(" IN STOCK  " + to_string(bomAnalysis_.readyCount), uiSuccessColor(), uiPanelLeftBg()));
  readyRows.push_back(ftxui::hbox({
      bomCell(" value", valueWidth, uiMutedColor()),
      bomCell("pkg", packageWidth, uiMutedColor()),
      bomCell("need", 6, uiMutedColor(), true),
      bomCell("have", 7, uiMutedColor(), true),
      styledText("  ", uiDimColor()),
      bomCell("slot", 10, uiMutedColor()),
  }) | ftxui::bgcolor(uiPanelLeftBg()));
  shortRows.push_back(fullLine(" TO ORDER  " + to_string(bomAnalysis_.shortCount), uiDangerColor(), uiPanelRightBg()));
  shortRows.push_back(ftxui::hbox({
      bomCell(" value", valueWidth, uiMutedColor()),
      bomCell("pkg", packageWidth, uiMutedColor()),
      bomCell("need", 6, uiMutedColor(), true),
      bomCell("have", 6, uiMutedColor(), true),
      styledText("  ", uiDimColor()),
      bomCell("suggested", max(10, halfWidth - valueWidth - packageWidth - 22), uiMutedColor()),
  }) | ftxui::bgcolor(uiPanelRightBg()));

  for (size_t index = 0; index < bomAnalysis_.matches.size(); ++index) {
    const auto& match = bomAnalysis_.matches[index];
    const auto& line = bomAnalysis_.lines[match.lineIndex];
    const bool selected = index == min(bomSplitSelection_, bomAnalysis_.matches.size() - 1);
    const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
    const auto fg = selected ? uiFocusColor() : uiPrimaryText();
    const auto package = packageFromFootprint(line.footprint);

    ftxui::Element row;
    if (match.sufficient) {
      const auto* item = store_.findById(match.chosenItemId());
      const auto slot = item == nullptr ? string() : rackLocation(*item, store_.racks());
      row = ftxui::hbox({
          bomCell(" " + line.designation, valueWidth, fg),
          bomCell(package, packageWidth, selected ? uiTitleColor() : uiSecondaryText()),
          bomCell(to_string(match.needed), 6, selected ? uiTitleColor() : uiSecondaryText(), true),
          bomCell(to_string(match.available), 7, uiSuccessColor(), true),
          styledText("  ", uiDimColor()),
          bomCell(slot.empty() ? (item == nullptr ? string("-") : item->location) : slot, 10, uiAccentColor()),
      }) | ftxui::bgcolor(bg);
    } else {
      string suggestion = "-";
      if (project != nullptr) {
        const auto lineKey = bomLineKey(line);
        const auto found = project->enrichment.find(lineKey);
        if (found != project->enrichment.end()) {
          suggestion = found->second;
        } else if (bomEnrichmentFuture_.valid() && lineKey == bomEnrichmentActiveKey_) {
          const auto phase = static_cast<int>(chrono::duration_cast<chrono::milliseconds>(
                             chrono::steady_clock::now().time_since_epoch()).count() / 350 % 4);
          suggestion = "Looking up" + string(static_cast<size_t>(phase), '.');
        } else if (find(bomEnrichmentQueue_.begin(), bomEnrichmentQueue_.end(), lineKey) !=
                   bomEnrichmentQueue_.end()) {
          suggestion = "In lookup queue";
        }
      }
      row = ftxui::hbox({
          bomCell(" " + line.designation, valueWidth, selected ? uiFocusColor() : uiDangerColor()),
          bomCell(package, packageWidth, selected ? uiTitleColor() : uiSecondaryText()),
          bomCell(to_string(match.needed), 6, uiDangerColor(), true),
          bomCell(to_string(match.available), 6, uiMutedColor(), true),
          styledText("  ", uiDimColor()),
          bomCell(suggestion, max(10, halfWidth - valueWidth - packageWidth - 22), uiLinkColor()),
      }) | ftxui::bgcolor(bg);
    }

    if (selected) {
      row = row | ftxui::select;
    }
    row = target(row, "bom.line." + to_string(index), UiTargetKind::Row, [self, index] {
      self->bomSplitSelection_ = index;
      self->bomSplitShortFocused_ = !self->bomAnalysis_.matches[index].sufficient;
      self->dirty_ = true;
    });
    (match.sufficient ? readyRows : shortRows).push_back(move(row));
  }

  if (bomAnalysis_.readyCount == 0) {
    readyRows.push_back(fullLine(" Nothing in this BOM is in stock yet.", uiMutedColor(), uiSurfaceBg()));
  }
  if (bomAnalysis_.shortCount == 0) {
    shortRows.push_back(fullLine(" Everything is covered.", uiSuccessColor(), uiSurfaceBg()));
  }

  auto readyPanel = ftxui::vbox(move(readyRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                    ftxui::bgcolor(uiSurfaceBg()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, halfWidth) |
                    ftxui::reflect(bomReadyPanelBounds_);
  auto shortPanel = ftxui::vbox(move(shortRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                    ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex | ftxui::reflect(bomShortPanelBounds_);

  ftxui::Elements footer;
  footer.push_back(uiDivider());
  const bool selectedShortage = !bomAnalysis_.matches.empty() &&
                                !bomAnalysis_.matches[min(bomSplitSelection_, bomAnalysis_.matches.size() - 1)].sufficient;
  const bool selectedMatched = selectedShortage &&
                               !bomAnalysis_.matches[min(bomSplitSelection_, bomAnalysis_.matches.size() - 1)]
                                    .chosenItemId().empty();
  footer.push_back(ftxui::hbox({
      target(styledText(" Build ", uiInteractiveColor(), uiRaisedSurfaceBg()), "bom.build", UiTargetKind::Button,
             [self] { self->beginBomBuild(); }),
      ftxui::text(" "),
      selectedShortage
          ? target(styledText(selectedMatched ? " Receive shortage " : " Add missing in Stock ",
                              selectedMatched ? uiInteractiveColor() : uiWarnColor(), uiRaisedSurfaceBg()),
                   "bom.restock", UiTargetKind::Button, [self] { self->beginBomRestock(); })
          : ftxui::text(""),
      ftxui::text(" "),
      target(styledText(" Export shortages ", uiLinkColor(), uiRaisedSurfaceBg()), "bom.export",
             UiTargetKind::Button, [self] { self->exportBomShortages(); }),
      ftxui::filler(),
      styledText("b build   r receive/add   o export   a alternate   +/- boards   esc all projects ", uiMutedColor()),
  }) | ftxui::bgcolor(uiSurfaceBg()));

  return ftxui::vbox({
      ftxui::vbox(move(headerRows)),
      ftxui::hbox({
          move(readyPanel),
          ftxui::separator() | ftxui::color(uiDimColor()),
          move(shortPanel),
      }) | ftxui::flex,
      ftxui::vbox(move(footer)),
  });
}

void App::handleBomProjectKey(const KeyEvent& key) {
  if (bomDeductPrompt_) {
    if (key.type == KeyType::Enter) {
      finishBomBuild(true);
      return;
    }
    if (key.type == KeyType::Escape) {
      if (bomBuildReady(bomAnalysis_)) {
        finishBomBuild(false);
      } else {
        bomDeductPrompt_ = false;
        bomView_ = BomView::Split;
        dirty_ = true;
      }
    }
    return;  // y / n are registered actions and dispatch ahead of this handler
  }

  if (bomView_ == BomView::Build) {
    if (key.type == KeyType::Enter) {
      advanceBomBuild(1);
    } else if (key.type == KeyType::Backspace || key.type == KeyType::Left) {
      advanceBomBuild(-1);
    } else if (key.type == KeyType::Right) {
      advanceBomBuild(1);
    } else if (key.type == KeyType::Escape) {
      bomView_ = BomView::Split;
      bomBuildStep_ = 0;
      dirty_ = true;
    }
    return;
  }

  // Everything with a visible accelerator lives in the action registry, which
  // dispatches ahead of this handler; only raw navigation is handled here.
  if (bomView_ == BomView::List || !bomAnalysisValid_) {
    if (key.type == KeyType::Character) {
      const auto ch = tolower(static_cast<unsigned char>(key.ch));
      if (ch == 'j') {
        moveBomSelection(1);
      } else if (ch == 'k') {
        moveBomSelection(-1);
      }
      return;
    }
    if (key.type == KeyType::Up) {
      moveBomSelection(-1);
    } else if (key.type == KeyType::Down) {
      moveBomSelection(1);
    } else if (key.type == KeyType::PageUp) {
      moveBomSelection(-10);
    } else if (key.type == KeyType::PageDown) {
      moveBomSelection(10);
    } else if (key.type == KeyType::Enter) {
      openSelectedBomProject();
    } else if (key.type == KeyType::Escape) {
      changePage(Page::Home);
    }
    return;
  }

  if (key.type == KeyType::Character) {
    switch (tolower(static_cast<unsigned char>(key.ch))) {
      case 'j':
        moveBomSelection(1);
        break;
      case 'k':
        moveBomSelection(-1);
        break;
      // '=' and '_' are the unshifted twins of the registered '+' and '-'.
      case '=':
        adjustBomBoards(1);
        break;
      case '_':
        adjustBomBoards(-1);
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Up) {
    moveBomSelection(-1);
  } else if (key.type == KeyType::Down) {
    moveBomSelection(1);
  } else if (key.type == KeyType::PageUp) {
    moveBomSelection(-10);
  } else if (key.type == KeyType::PageDown) {
    moveBomSelection(10);
  } else if (key.type == KeyType::Home) {
    bomSplitSelection_ = 0;
    dirty_ = true;
  } else if (key.type == KeyType::End) {
    if (!bomAnalysis_.matches.empty()) {
      bomSplitSelection_ = bomAnalysis_.matches.size() - 1;
      dirty_ = true;
    }
  } else if (key.type == KeyType::Enter) {
    beginBomBuild();
  } else if (key.type == KeyType::Escape) {
    bomView_ = BomView::List;
    dirty_ = true;
  }
}

}  // namespace inventatory
