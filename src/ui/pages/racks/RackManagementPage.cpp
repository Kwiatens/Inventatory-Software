// Inventatory - Hardware Inventory Management System
// Inventatory Rack management page rendering and keyboard handling.

#include "App.h"

#include "ui/pages/racks/RackManagementPagePrivate.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

ftxui::Element rackFixedCell(const string& text, int width, ftxui::Color color, bool rightAlign = false) {
  const auto clipped = ellipsize(text, static_cast<size_t>(max(0, rightAlign ? width - 1 : width)));
  auto content = rightAlign
                     ? ftxui::hbox({ftxui::filler(), styledText(clipped, color), ftxui::text(" ")})
                     : ftxui::hbox({styledText(clipped, color), ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

ftxui::Element rackCenteredCell(const string& text, int width, ftxui::Color color) {
  return ftxui::paragraphAlignCenter(ellipsize(text, static_cast<size_t>(max(1, width - 1)))) |
         ftxui::color(color) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

int rackRowCount(const InventatoryRack& rack) {
  return clamp(rack.rows, 0, 5);
}

int rackColumnCount(const InventatoryRack& rack) {
  return clamp(rack.columns, 0, 5);
}

size_t rackCapacity(const InventatoryRack& rack) {
  return static_cast<size_t>(rackRowCount(rack)) * static_cast<size_t>(rackColumnCount(rack));
}

ftxui::Element rackOccupancyIndicator(size_t occupied, size_t capacity, int width, ftxui::Color occupiedColor) {
  return ftxui::hbox({
             ftxui::filler(),
             ftxui::text(to_string(occupied)) | ftxui::color(occupiedColor),
             ftxui::text("/") | ftxui::color(uiDimColor()),
             ftxui::text(to_string(capacity)) | ftxui::color(uiMutedColor()),
         }) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

string assignmentLabel(RackAssignmentMode mode) {
  if (mode == RackAssignmentMode::Manual) return "manual";
  if (mode == RackAssignmentMode::Unassigned) return "unassigned";
  return "automatic";
}

string packageSummary(const InventoryItem& item) {
  for (const auto& parameter : item.parameters) {
    if (parameterLabelMatches(parameter.name, "Package") || parameterLabelMatches(parameter.name, "Package / Case")) {
      return parameter.value;
    }
  }
  return "-";
}

// A part mid-move gets a distinct blue-gray active highlight: brighter on its grid
// cell, dimmer on the informational banner in the slot detail panel.
ftxui::Color rackMovingSourceBg() {
  return uiActiveBg();
}

ftxui::Color rackMovingBannerBg() {
  return uiActiveSoftBg();
}

string toTitleCase(string s) {
  if (s.empty()) return s;
  s[0] = static_cast<char>(toupper(static_cast<unsigned char>(s[0])));
  return s;
}

string shortComponentType(const string& componentType) {
  if (componentType == "Integrated Circuits") return "ICs";
  return componentType;
}

ftxui::Element rackQuantityIndicator(const InventoryItem& item, bool selected, int lowStockThreshold) {
  const auto foreground = item.quantity <= 0 ? uiDangerColor()
                        : isLowStock(item, lowStockThreshold) ? uiWarnColor()
                                          : uiPrimaryText();
  const auto background = selected ? uiSelectionBg()
                        : uiRaisedSurfaceBg();
  auto indicator = styledText(" Quantity:[" + to_string(item.quantity) + "] ", foreground, background);
  if (selected) indicator = indicator | ftxui::bold;
  return indicator;
}

vector<string> rackTitleLines(const string& value, int width) {
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

    if (!line.empty() && static_cast<int>(line.size() + word.size() + 1) > lineWidth) {
      flushLine();
    }
    if (!line.empty()) {
      line.push_back(' ');
    }
    line += word;
  }

  flushLine();
  if (lines.empty()) {
    lines.push_back({});
  }
  return lines;
}

ftxui::Element centeredRackText(const string& value, int width, ftxui::Color color, int maxLines = 0) {
  const int lineWidth = max(1, width - 2);
  auto lines = rackTitleLines(value, lineWidth);
  if (maxLines > 0 && static_cast<int>(lines.size()) > maxLines) {
    lines.resize(maxLines);
    lines.back() = ellipsize(lines.back() + "...", static_cast<size_t>(lineWidth));
  }
  string wrapped;
  for (size_t index = 0; index < lines.size(); ++index) {
    if (index > 0) {
      wrapped.push_back('\n');
    }
    wrapped += lines[index];
  }
  return (ftxui::paragraphAlignCenter(wrapped) | ftxui::color(color)) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

}  // namespace

ftxui::Element App::renderRackManagementUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 40;
  const auto rackIndices = sortedRackIndices();
  const bool inventoryHasNoRacks = store_.racks().empty();
  const auto* rack = selectedRack();
  const auto selectedSlot = selectedRackSlot();
  const auto* selectedSlotItem = selectedRackItem();
  // The supported 100-column terminal still keeps the rack matrix beside its
  // context panels. Stacking would make the 5x5 grid taller than the viewport.
  const bool compact = screenWidth < 100;
  const int listWidth = screenWidth < 118 ? 24 : 30;
  int detailWidth = screenWidth < 118 ? 30 : 36;
  // In the horizontal layout, account for its two one-column separators. The
  // compact layout stacks the grid below the side panels, so it uses the full
  // screen width instead.
  int gridWidth = compact ? screenWidth : max(42, screenWidth - listWidth - detailWidth - 2);
  if (!compact && rack != nullptr && rackRowCount(*rack) > 0) {
    constexpr int rowDesignatorWidth = 3;
    const int columnCount = rackRowCount(*rack);
    const int separatorCount = columnCount;
    const int slotWidth = max(7, (gridWidth - rowDesignatorWidth - separatorCount) / columnCount);
    const int fittedGridWidth = rowDesignatorWidth + separatorCount + slotWidth * columnCount;
    detailWidth += gridWidth - fittedGridWidth;
    gridWidth = fittedGridWidth;
  }

  // Rack code, type, and a right-aligned occupancy column that ends one cell
  // short of the panel edge. Type absorbs the remaining width so the numbers
  // sit flush right instead of floating in the middle of the panel.
  constexpr int rackCodeWidth = 5;
  constexpr int rackUsedWidth = 12;
  // The scroll indicator owns the final column of the rack-list body. Keep
  // the table itself one column narrower so its fixed columns remain intact
  // at the supported 100-column layout.
  const int rackContentWidth = listWidth - 1;
  const int rackTypeWidth = max(1, rackContentWidth - rackCodeWidth - rackUsedWidth - 1);

  ftxui::Elements rackRows;
  if (!rackIndices.empty()) {
    for (size_t visible = 0; visible < rackIndices.size(); ++visible) {
      const auto& candidate = store_.racks()[rackIndices[visible]];
      const bool selected = visible == min(rackSelection_, rackIndices.size() - 1);
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto fg = selected ? uiFocusColor() : uiPrimaryText();
      const auto occupied = rackOccupiedSlotCount(store_, candidate);
      const auto capacity = rackCapacity(candidate);
      auto rackRow = ftxui::hbox({
          rackFixedCell(" " + candidate.code, rackCodeWidth, fg),
          rackFixedCell(toTitleCase(candidate.componentType), rackTypeWidth,
                        selected ? uiTitleColor() : uiLabelColor()),
          rackOccupancyIndicator(occupied, capacity, rackUsedWidth,
                                 occupied >= capacity && capacity != 0 ? uiWarnColor() : uiPrimaryText()),
          ftxui::text(" "),
      }) | ftxui::bgcolor(bg);
      if (selected) {
        rackRow = rackRow | ftxui::select;
      }
      auto self = const_cast<App*>(this);
      rackRows.push_back(target(rackRow, "racks.row." + candidate.id, UiTargetKind::Row, [self, visible] {
        self->rackSelection_ = visible;
        self->rackRow_ = 0;
        self->rackColumn_ = 0;
        self->dirty_ = true;
      }));
    }
  } else if (!inventoryHasNoRacks) {
    rackRows.push_back(fullLine("No racks match filter.", uiMutedColor(), uiPanelLeftBg()));
  }

  ftxui::Elements gridRows;
  if (rack == nullptr) {
    const auto emptyState = inventoryHasNoRacks ? "Add or import an eligible small component to create racks automatically."
                                                : "No racks match filter.";
    gridRows.push_back(ftxui::filler());
    gridRows.push_back(ftxui::hbox({
        ftxui::filler(),
        styledText(emptyState, uiMutedColor()),
        ftxui::filler(),
    }));
    gridRows.push_back(ftxui::filler());
  } else {
    if (!rackFilter_.empty()) {
      gridRows.push_back(fullLine("Filter: " + rackFilter_, uiWarnColor(), uiPanelRightBg()));
    }
    const int rackGridRows = rackRowCount(*rack);
    const int rackGridColumns = rackColumnCount(*rack);
    if (rackGridRows == 0 || rackGridColumns == 0) {
      gridRows.push_back(fullLine("Rack dimensions are unavailable.", uiWarnColor(), uiPanelRightBg()));
    } else {
      // The left header cell labels the physical row numbers; the header cells
      // above them label the lettered rack columns. Both dimensions come from
      // the selected rack instead of assuming the default five-by-five size.
      constexpr int rowHeaderWidth = 3;
      // Include the divider between the numeric row labels and the slot cells.
      const int separatorCount = rackGridRows;
      const int slotSpace = max(rackGridRows, gridWidth - rowHeaderWidth - separatorCount);
      const int slotWidth = max(7, slotSpace / rackGridRows);
      // Keep every slot column identical. The designator lane absorbs the
      // indivisible terminal-width remainder so the complete rack width still
      // matches the available grid width exactly.
      const int designatorWidth = rowHeaderWidth;
      // Keep the letter headers to one row immediately below the controls.
      // Keep the letter headers to one plain row immediately below the
      // controls. A single grid-wide divider below it gives the column axis
      // the same clean separation as the numbered row axis.
      // The shell permanently reserves its notification row, even when it is
      // quiet. Leave that row, the search row, dividers, and rack controls
      // outside the slot matrix so the last slot line remains visible.
      const int slotRowsSpace = max(rackGridColumns * 3,
                                    screenHeight - 12 - (!rackFilter_.empty() ? 1 : 0));
      const int slotHeight = rack_page_detail::equalRackSlotHeight(slotRowsSpace, rackGridColumns);
      ftxui::Elements columnHeaders;
      columnHeaders.push_back(rackCenteredCell("", designatorWidth, uiDimColor()));
      columnHeaders.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
      for (int displayColumn = 0; displayColumn < rackGridRows; ++displayColumn) {
        columnHeaders.push_back(rackCenteredCell(string(1, static_cast<char>('A' + displayColumn)), slotWidth,
                                                  uiAccentColor()));
        if (displayColumn < rackGridRows - 1) {
          columnHeaders.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        }
      }
      gridRows.push_back(ftxui::hbox(move(columnHeaders)) | ftxui::bgcolor(uiPanelRightBg()));
      gridRows.push_back(uiDivider());
      // Display the rack like the physical unit: slot numbers run downward
      // within each lettered column, while letters advance from left to right.
      // Keep the existing rackRow_/rackColumn_ state and slot lookup untouched
      // by translating the visual coordinates back to storage coordinates here.
      for (int displayRow = 0; displayRow < rackGridColumns; ++displayRow) {
        // Keep every physical slot row identical. Distributing a terminal-row
        // remainder to the first rows makes the top row taller and can leave
        // the final row clipped by the page boundary.
        const int rowHeight = slotHeight;
        ftxui::Elements rowCells;
        auto rowDesignator = ftxui::vbox({
                                  ftxui::filler(),
                                  rackCenteredCell(to_string(displayRow + 1), designatorWidth, uiAccentColor()),
                                  ftxui::filler(),
                              }) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, designatorWidth) |
                             ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, rowHeight);
        rowCells.push_back(move(rowDesignator));
        rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        for (int displayColumn = 0; displayColumn < rackGridRows; ++displayColumn) {
          const auto slot = rackSlotLabel(displayColumn, displayRow);
          const auto* item = itemAtRackSlot(store_, rack->id, slot);
          const bool selected = displayColumn == rackRow_ && displayRow == rackColumn_;
          const bool movingSource = item != nullptr && item->id == movingRackItemId_;
          const auto bg = movingSource ? rackMovingSourceBg()
                          : selected ? uiSelectionBg()
                                     : (item == nullptr ? uiCanvasBg() : uiSurfaceBg());
          const auto itemText = item == nullptr ? string("[ empty ]") : item->partName;
          const int cellWidth = slotWidth;
          ftxui::Elements cellRows;
          const auto nameColor = item == nullptr ? uiDimColor() : uiPrimaryText();
          auto nameArea = ftxui::vbox({
                                ftxui::filler(),
                                centeredRackText(itemText, cellWidth, nameColor, max(1, rowHeight - 1)),
                                ftxui::filler(),
                            }) |
                          ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, max(1, rowHeight - 1));
          cellRows.push_back(move(nameArea));
          auto quantity = item == nullptr
                              ? styledText(" available ", uiDimColor())
                              : rackQuantityIndicator(*item, selected, settings_.lowStockThreshold);
          cellRows.push_back(ftxui::hbox({
              move(quantity),
              ftxui::filler(),
          }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth));
          // Apply the fill after the fixed geometry so every slot, including
          // the selected one, paints its complete equal-sized rectangle.
          auto cell = ftxui::vbox(move(cellRows)) |
                      ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth) |
                      ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, rowHeight) |
                      ftxui::bgcolor(bg);
          if (selected) {
            cell = cell | ftxui::select;
          }
          auto self = const_cast<App*>(this);
          rowCells.push_back(target(cell, "racks.cell." + slot, UiTargetKind::Cell, [self, displayColumn, displayRow] {
            self->rackRow_ = displayColumn;
            self->rackColumn_ = displayRow;
            self->dirty_ = true;
          }));
          if (displayColumn < rackGridRows - 1) {
            rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
          }
        }
        gridRows.push_back(ftxui::hbox(move(rowCells)));
        if (displayRow < rackGridColumns - 1) {
          gridRows.push_back(uiDivider());
        }
      }
    }
  }

  ftxui::Elements detailRows;
  if (rack == nullptr) {
    if (!inventoryHasNoRacks) {
      detailRows.push_back(detailFieldLine({"Status: ", "No racks match filter", uiWarnColor(), uiTitleColor()}, detailWidth - 2));
    }
  } else {
    detailRows.push_back(detailFieldLine({"Rack: ", rack->code, uiLabelColor(), uiTitleColor()}, detailWidth - 2));
    detailRows.push_back(detailFieldLine({"Slot: ", selectedSlot, uiLabelColor(), uiTitleColor()}, detailWidth - 2));
    detailRows.push_back(detailFieldLine({"Type: ", rack->componentType, uiLabelColor(), uiTitleColor()}, detailWidth - 2));
    detailRows.push_back(uiDivider());
    if (selectedSlotItem == nullptr) {
      detailRows.push_back(fullLine("Empty slot", uiMutedColor(), uiSurfaceBg()));
      detailRows.push_back(ftxui::paragraphAlignLeft(movingRackItemId_.empty()
                                                         ? "Press v on an occupied slot to start moving a part."
                                                         : "Press v here to place the moving part.") |
                           ftxui::color(movingRackItemId_.empty() ? uiMutedColor() : uiAccentColor()));
    } else {
      detailRows.push_back(fullLine("Inventatory rack: " + rack->code + "-" + selectedSlot, uiTitleColor(), uiRowSelectedBg()));
      detailRows.push_back(detailFieldLine({"Part: ", selectedSlotItem->partName, uiLabelColor(), uiTitleColor()}, detailWidth - 2));
      detailRows.push_back(detailFieldLine({"Category: ", displayCategory(selectedSlotItem->category), uiLabelColor(), uiTitleColor()},
                                          detailWidth - 2));
      detailRows.push_back(detailFieldLine({"Package: ", packageSummary(*selectedSlotItem), uiLabelColor(), uiTitleColor()},
                                          detailWidth - 2));
      detailRows.push_back(detailFieldLine({"Mode: ", assignmentLabel(selectedSlotItem->rackAssignment), uiLabelColor(),
                                            selectedSlotItem->rackAssignment == RackAssignmentMode::Manual ? uiWarnColor()
                                                                                                          : uiSuccessColor()},
                                           detailWidth - 2));
    }
    auto selfDetail = const_cast<App*>(this);
    ftxui::Elements slotActions;
    slotActions.push_back(target(uiSecondaryButton("Move"), "racks.move", UiTargetKind::Button,
                                 [selfDetail] { selfDetail->beginOrCompleteRackMove(); }));
    if (selectedSlotItem != nullptr) {
      slotActions.push_back(ftxui::text(" "));
      slotActions.push_back(styledText("Qty", uiMutedColor()));
      slotActions.push_back(ftxui::text(" "));
      slotActions.push_back(target(uiSecondaryButton("-"), "racks.part.minus", UiTargetKind::Button,
                                   [selfDetail] { selfDetail->adjustSelectedRackItemQuantity(-1); }));
      slotActions.push_back(target(uiSecondaryButton("+"), "racks.part.plus", UiTargetKind::Button,
                                   [selfDetail] { selfDetail->adjustSelectedRackItemQuantity(1); }));
      slotActions.push_back(ftxui::text(" "));
      slotActions.push_back(target(uiSecondaryButton("Remove", uiWarnColor()), "racks.part.remove", UiTargetKind::Button,
                                   [selfDetail] { selfDetail->unassignSelectedRackItem(); }));
    }
    detailRows.push_back(ftxui::hbox(move(slotActions)));
    if (!movingRackItemId_.empty()) {
      detailRows.push_back(uiDivider());
      const auto* moving = store_.findById(movingRackItemId_);
      detailRows.push_back(fullLine("Moving: " + (moving == nullptr ? string("missing item") : ellipsize(moving->partName, 28)),
                                    uiWarnColor(), rackMovingBannerBg()));
      detailRows.push_back(styledText("From " + movingRackSource_, uiMutedColor()));
    }
  }

  if (!inventoryHasNoRacks) detailRows.insert(detailRows.begin(), fullLine("Slot detail", uiSecondaryText(), uiSurfaceBg()));
  auto rackHeader = ftxui::vbox({
      fullLine("Racks", uiSecondaryText(), uiSurfaceBg()),
      ftxui::hbox({
          rackFixedCell("Rack", rackCodeWidth, uiMutedColor()),
          rackFixedCell("Type", rackTypeWidth, uiMutedColor()),
          rackFixedCell("Usage", rackUsedWidth, uiMutedColor(), true),
          ftxui::text("  "),
      }) | ftxui::bgcolor(uiPanelLeftBg()),
  });
  auto rackListBody = ftxui::vbox(move(rackRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                      ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex | ftxui::reflect(rackListPanelBounds_);
  auto rackPanel = ftxui::vbox({move(rackHeader), move(rackListBody)}) | ftxui::bgcolor(uiSurfaceBg()) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listWidth) | ftxui::flex;
  auto gridPanel = ftxui::vbox(move(gridRows)) | ftxui::bgcolor(uiSurfaceBg()) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, gridWidth);
  auto detailPanel = ftxui::vbox(move(detailRows)) | ftxui::bgcolor(uiSurfaceBg()) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, detailWidth);

  if (compact) {
    return ftxui::vbox({
        ftxui::hbox({rackPanel, uiDivider(), detailPanel}),
        uiDivider(),
        gridPanel,
    });
  }

  return ftxui::hbox({
      rackPanel,
      ftxui::separator() | ftxui::color(uiDimColor()),
      gridPanel,
      ftxui::separator() | ftxui::color(uiDimColor()),
      detailPanel,
  });
}

void App::handleRackManagementKey(const KeyEvent& key) {
  if (key.type == KeyType::CtrlZ) {
    undoLastInventoryChange();
    syncRackSelection();
    return;
  }

  if (key.type == KeyType::Tab || key.type == KeyType::Escape) {
    changePage(Page::Home);
    return;
  }

  if (key.type == KeyType::Enter) {
    const auto* item = selectedRackItem();
    if (item == nullptr) {
      setMessage("No part in this slot", 2);
      return;
    }
    const auto it = find_if(store_.items().begin(), store_.items().end(), [&](const InventoryItem& candidate) {
      return candidate.id == item->id;
    });
    if (it != store_.items().end()) {
      selectedPosition_ = static_cast<size_t>(distance(store_.items().begin(), it));
      syncSelectionToFilter();
      changePage(Page::Stock);
    }
    return;
  }

  if (key.type == KeyType::Up) {
    moveRackSlot(0, -1);
  } else if (key.type == KeyType::Down) {
    moveRackSlot(0, 1);
  } else if (key.type == KeyType::Left) {
    moveRackSlot(-1, 0);
  } else if (key.type == KeyType::Right) {
    moveRackSlot(1, 0);
  } else if (key.type == KeyType::Character) {
    // Every other command on this screen (jump, filter, move/place, print,
    // rename, create/delete rack, quit, ...) is a registered action in
    // ActionRegistry.cpp and is dispatched before this handler ever runs;
    // only vim-style slot movement lives here.
    switch (tolower(static_cast<unsigned char>(key.ch))) {
      case 'h':
        moveRackSlot(-1, 0);
        break;
      case 'j':
        moveRackSlot(0, 1);
        break;
      case 'k':
        moveRackSlot(0, -1);
        break;
      case 'l':
        moveRackSlot(1, 0);
        break;
      default:
        break;
    }
  }
}

}  // namespace inventatory
