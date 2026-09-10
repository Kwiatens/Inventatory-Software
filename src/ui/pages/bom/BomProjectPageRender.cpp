// Inventatory - Hardware Inventory Management System
// KiCad BOM project rendering: pinned list, BOM comparison, Find in racks workflow.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
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

vector<string> bomRackTitleLines(const string& value, int width) {
  const int lineWidth = max(1, width);
  vector<string> lines;
  istringstream words(value);
  string word;
  string line;

  const auto flushLine = [&] {
    if (!line.empty()) {
      lines.push_back(move(line));
      line.clear();
    }
  };

  while (words >> word) {
    if (static_cast<int>(word.size()) > lineWidth) {
      flushLine();
      for (size_t offset = 0; offset < word.size(); offset += static_cast<size_t>(lineWidth)) {
        lines.push_back(word.substr(offset, static_cast<size_t>(lineWidth)));
      }
      continue;
    }
    if (!line.empty() && static_cast<int>(line.size() + word.size() + 1) > lineWidth) flushLine();
    if (!line.empty()) line.push_back(' ');
    line += word;
  }

  flushLine();
  if (lines.empty()) lines.push_back({});
  return lines;
}

ftxui::Element centeredBomRackText(const string& value, int width, ftxui::Color color) {
  const auto lines = bomRackTitleLines(value, max(1, width - 2));
  string wrapped;
  for (size_t index = 0; index < lines.size(); ++index) {
    if (index > 0) wrapped.push_back('\n');
    wrapped += lines[index];
  }
  return (ftxui::paragraphAlignCenter(wrapped) | ftxui::color(color)) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Element bomRackQuantityIndicator(const InventoryItem& item, bool active, int lowStockThreshold) {
  const auto foreground = item.quantity <= 0 ? uiDangerColor()
                        : isLowStock(item, lowStockThreshold) ? uiWarnColor()
                                                              : uiPrimaryText();
  const auto background = active ? uiSelectionBg() : uiRaisedSurfaceBg();
  auto indicator = styledText(" Quantity:[" + to_string(item.quantity) + "] ", foreground, background);
  if (active) indicator = indicator | ftxui::bold;
  return indicator;
}

ftxui::Element bomRackSlotLabel(const string& value, ftxui::Color color) {
  return ftxui::hbox({uiHeaderText(value, color), ftxui::text(" ")});
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

    const int sideWidth = clamp(screenWidth / 3, 32, 46);
    ftxui::Elements sideRows;
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
    if (stepIndex + 1 < steps.size()) {
      sideRows.push_back(fullLine("Next: " + steps[stepIndex + 1].title + " · " +
                                      to_string(steps[stepIndex + 1].picks.size()) + " slots",
                                  uiMutedColor(), uiSurfaceBg()));
    }
    sideRows.push_back(ftxui::hbox({
        target(styledText(" Next stop  Enter ", uiCanvasBg(), uiInteractiveColor()), "bom.build.next",
               UiTargetKind::Button, [self] { self->advanceBomBuild(1); }),
        ftxui::text(" "),
        target(styledText(" Back  Bksp ", uiSecondaryText(), uiRaisedSurfaceBg()), "bom.build.back",
               UiTargetKind::Button, [self] { self->advanceBomBuild(-1); }),
        ftxui::filler(),
    }));

    ftxui::Elements mainRows;
    mainRows.push_back(centered(uiHeaderText(step.title, uiAccentColor())) | ftxui::bgcolor(uiPanelRightBg()));
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
      // Keep the walkthrough grid identical to the Racks page: five columns,
      // five rows, centered part names, and the quantity/slot strip at the
      // bottom of every cell.
      const int rows = 5;
      const int columns = 5;
      const int gridWidth = max(30, screenWidth - sideWidth - 3);
      const int slotSpace = gridWidth - (columns - 1);
      const int slotWidth = max(7, slotSpace / columns);
      const int extraColumns = max(0, slotSpace - slotWidth * columns);
      // The page frame reserves five rows for the shared shell and this view's
      // project header. The rack title/divider and four row dividers consume
      // four more rows, leaving the same slot-space calculation as Racks.
      // Truncating to a multiple keeps every slot exactly the same height.
      const int availableSlotRows = max(15, screenHeight - 13);
      const int slotHeight = max(3, availableSlotRows / rows);

      int maxTitleLines = 1;
      for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
          const auto slot = rackSlotLabel(column, row);
          const auto* item = itemAtRackSlot(store_, step.rackId, slot);
          const auto itemText = item == nullptr ? string("[ empty ]") : item->partName;
          maxTitleLines = max(maxTitleLines,
                              static_cast<int>(bomRackTitleLines(itemText, max(1, slotWidth - 2)).size()));
        }
      }
      const int rowHeight = max(slotHeight, maxTitleLines + 1);

      for (int row = 0; row < rows; ++row) {
        ftxui::Elements rowCells;
        for (int column = 0; column < columns; ++column) {
          const auto slot = rackSlotLabel(column, row);
          const auto* item = itemAtRackSlot(store_, step.rackId, slot);
          const bool lit = litSlots.count(slot) != 0;
          // A lit slot pulses between a filled highlight and the resting
          // surface, so the eye lands on exactly what to open.
          const auto bg = lit ? (blink ? bomLitSlotBg() : uiSurfaceBg()) : uiCanvasBg();
          const auto titleColor = lit ? (blink ? uiPrimaryText() : uiSuccessColor())
                                     : (item == nullptr ? uiMutedColor() : uiAccentColor());
          const auto nameColor = item == nullptr ? uiDimColor() : uiPrimaryText();
          const int cellWidth = slotWidth + (column < extraColumns ? 1 : 0);

          auto nameArea = ftxui::vbox({
                                ftxui::filler(),
                                centeredBomRackText(item == nullptr ? string("[ empty ]") : item->partName,
                                                    cellWidth, nameColor),
                                ftxui::filler(),
                            }) |
                          ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, max(1, rowHeight - 1));
          auto quantity = item == nullptr
                              ? styledText(" available ", uiDimColor())
                              : bomRackQuantityIndicator(*item, lit && blink, settings_.lowStockThreshold);
          auto bottom = ftxui::hbox({
              move(quantity),
              ftxui::filler(),
              bomRackSlotLabel(slot, titleColor),
          }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth);
          rowCells.push_back(ftxui::vbox({move(nameArea), move(bottom)}) | ftxui::bgcolor(bg) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth) |
                             ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, rowHeight));
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
    });
  }

  // ------------------------------------------------------------ compare ---
  // The open-project screen has one job: make the stock comparison readable
  // and put the rack workflow at the point where the user needs it.
  const int tableWidth = screenWidth;
  const int categoryTreeWidth = 4;
  const int partTreeWidth = 4;
  const int treeWidth = categoryTreeWidth + partTreeWidth;
  const int partWidth = clamp(tableWidth / 4, 24, 34);
  const int categoryNameWidth = max(1, partWidth - categoryTreeWidth);
  const int partNameWidth = max(1, partWidth - treeWidth);
  const int packageWidth = clamp(tableWidth / 8, 12, 16);
  const int needHaveWidth = 14;
  const int statusWidth = 10;
  const int detailWidth = max(18, tableWidth - partWidth - packageWidth -
                                      needHaveWidth - statusWidth);

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
      styledText("  ", uiMutedColor()),
      target(styledText(" Find in racks  f ", uiCanvasBg(), uiInteractiveColor()),
             "bom.find-in-racks", UiTargetKind::Button, [self] { self->beginBomBuild(); }),
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

  ftxui::Elements tableRows;
  tableRows.push_back(ftxui::hbox({
      bomCell("", treeWidth, uiMutedColor()),
      bomCell("Part", partNameWidth, uiMutedColor()),
      bomCell("Package", packageWidth, uiMutedColor()),
      bomCell("Where / suggested match", detailWidth, uiMutedColor()),
      bomCell("Need / Have", needHaveWidth, uiMutedColor(), true),
      bomCell("Status", statusWidth, uiMutedColor(), true),
  }) | ftxui::bgcolor(uiPanelLeftBg()));

  const auto groupRow = [&](const string& label, ftxui::Color color, bool nested,
                            bool branchLast) {
    ftxui::Element partColumn;
    if (nested) {
      const auto branch = ftxui::hbox({
          styledText(branchLast ? "└──" : "├──", uiDividerColor()),
          ftxui::filler(),
      }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, categoryTreeWidth);
      const auto title = uiHeaderText(label, color);
      partColumn = ftxui::hbox({
          branch,
          ftxui::hbox({title, ftxui::filler()}) |
              ftxui::size(ftxui::WIDTH, ftxui::EQUAL, categoryNameWidth),
      });
    } else {
      partColumn = bomCell(" " + label, partWidth, color);
    }
    return ftxui::hbox({
               partColumn,
               bomCell("", packageWidth, uiMutedColor()),
               bomCell("", detailWidth, uiMutedColor()),
               bomCell("", needHaveWidth, uiMutedColor(), true),
               bomCell("", statusWidth, uiMutedColor(), true),
           }) |
           ftxui::bgcolor(uiRaisedSurfaceBg());
  };

  const auto categoryIsLast = [&](size_t index, const string& category, bool sufficient) {
    for (size_t next = index + 1; next < bomAnalysis_.matches.size(); ++next) {
      const auto& nextMatch = bomAnalysis_.matches[next];
      if (nextMatch.sufficient != sufficient) return true;
      const auto& nextLine = bomAnalysis_.lines[nextMatch.lineIndex];
      const auto nextCategory = bomComparisonCategory(nextLine, nextMatch, store_.items());
      if (toLower(nextCategory) != toLower(category)) return false;
    }
    return true;
  };

  const auto partIsLast = [&](size_t index, const string& category, bool sufficient) {
    if (index + 1 >= bomAnalysis_.matches.size()) return true;
    const auto& nextMatch = bomAnalysis_.matches[index + 1];
    if (nextMatch.sufficient != sufficient) return true;
    const auto& nextLine = bomAnalysis_.lines[nextMatch.lineIndex];
    const auto nextCategory = bomComparisonCategory(nextLine, nextMatch, store_.items());
    return toLower(nextCategory) != toLower(category);
  };

  string previousAvailability;
  string previousCategory;
  const auto needHaveCell = [&](const BomMatch& match) {
    constexpr int countFieldWidth = 3;
    const auto need = ftxui::hbox({
        ftxui::filler(),
        styledText(to_string(match.needed), uiMutedColor()),
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, countFieldWidth);
    const auto have = ftxui::hbox({
        styledText(to_string(match.available),
                   match.sufficient ? uiSuccessColor() : uiDangerColor()),
        ftxui::filler(),
    }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, countFieldWidth);
    return ftxui::hbox({
               ftxui::filler(),
               need,
               styledText("/", uiDimColor()),
               have,
               ftxui::text(" "),
           }) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, needHaveWidth);
  };

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
    const auto categoryLast = categoryIsLast(index, category, match.sufficient);
    const auto partLast = partIsLast(index, category, match.sufficient);
    if (availability != previousAvailability) {
      if (!previousAvailability.empty()) {
        tableRows.push_back(ftxui::text("") | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1) |
                            ftxui::bgcolor(uiSurfaceBg()));
      }
      tableRows.push_back(groupRow(availability, match.sufficient ? uiSuccessColor() : uiDangerColor(), false,
                                   false));
      previousAvailability = availability;
      previousCategory.clear();
    }
    if (toLower(category) != toLower(previousCategory)) {
      tableRows.push_back(groupRow(category, uiSecondaryText(), true, categoryLast));
      previousCategory = category;
    }
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
        ftxui::hbox({
            styledText(categoryLast ? "    " : "│   ", uiDividerColor()),
            ftxui::filler(),
        }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, categoryTreeWidth),
        ftxui::hbox({
            styledText(partLast ? "└──" : "├──", uiDividerColor()),
            ftxui::filler(),
        }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, partTreeWidth),
        bomCell(line.designation, partNameWidth, fg),
        bomCell(package, packageWidth, selected ? uiTitleColor() : uiSecondaryText()),
        bomCell(detail, detailWidth, match.sufficient ? uiAccentColor() : uiLinkColor()),
        needHaveCell(match),
        bomCell(match.sufficient ? "READY" : "MISSING", statusWidth,
                match.sufficient ? uiSuccessColor() : uiDangerColor(), true),
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
