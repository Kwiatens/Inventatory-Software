// Inventatory - Hardware Inventory Management System
// Inventatory Rack management page rendering and keyboard handling.

#include "App.h"

#include "ui/pages/racks/RackManagementPagePrivate.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

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
  return rack_page_detail::shortComponentType(componentType);
}

}  // namespace

namespace rack_page_detail {

// One slot's contents: the part name split into dim type, bold value and dim specification (as many lines as the
// cell height allows), and a bold quantity line colored by stock state. Out of stock also says so in words.
ftxui::Element rackCellBody(const InventoryItem& item, bool selected, int lowStockThreshold, int cellWidth,
                            int rowHeight) {
  // Narrow cells (the 100-column terminal) give up their padding so a value such as 0.47UF stays whole.
  const bool narrow = cellWidth < 10;
  const int textWidth = max(1, narrow ? cellWidth : cellWidth - 2);
  const int areaLines = max(1, rowHeight - 1);
  const auto parts = splitPartName(item.partName);
  const auto dim = selected ? uiLinkColor() : uiMutedText();
  const auto strong = selected ? uiFocusColor() : uiPrimaryText();
  const auto line = [&](ftxui::Element content) {
    return ftxui::hbox({ftxui::text(narrow ? "" : " "), move(content), ftxui::filler()}) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth);
  };
  ftxui::Elements area;
  // The text block floats in the middle of tall cells; the quantity stays pinned to the bottom edge.
  area.push_back(ftxui::filler());
  if (parts.type.empty() || areaLines == 1) {
    // No recognizable value, or no room to split: the whole name in bold, wrapped to the cell.
    if (areaLines == 1) {
      area.push_back(line(uiPartName(item.partName, textWidth, selected)));
    } else {
      auto lines = wrapText(item.partName, textWidth);
      if (static_cast<int>(lines.size()) > areaLines) {
        lines.resize(static_cast<size_t>(areaLines));
        lines.back() = ellipsize(lines.back() + "...", static_cast<size_t>(textWidth));
      }
      for (const auto& text : lines) area.push_back(line(uiHeaderText(text, strong)));
    }
  } else if (static_cast<int>(ftxui::string_width(parts.type)) > textWidth) {
    // Too narrow for the type, which the rack's own type already implies: value first, then the specification
    // wrapped over the remaining lines.
    area.push_back(line(uiHeaderText(ellipsize(parts.value, static_cast<size_t>(textWidth)), strong)));
    auto specLines = wrapText(parts.spec, textWidth);
    const int specRoom = areaLines - 1;
    if (static_cast<int>(specLines.size()) > specRoom) {
      specLines.resize(static_cast<size_t>(specRoom));
      if (!specLines.empty() && !narrow) {
        specLines.back() = ellipsize(specLines.back() + "...", static_cast<size_t>(textWidth));
      }
    }
    for (const auto& text : specLines) area.push_back(line(styledText(text, dim)));
  } else if (areaLines == 2) {
    area.push_back(line(uiHeaderText(ellipsize(parts.value, static_cast<size_t>(textWidth)), strong)));
    // Two lines leave room for one more fact: the specification, since the rack's type already implies the kind.
    const auto& secondLine = parts.spec.empty() ? parts.type : parts.spec;
    string secondText = ellipsize(secondLine, static_cast<size_t>(textWidth));
    if (narrow) {
      // Whole words only: "6.3V" rather than "6.3V X5".
      const auto words = wrapText(secondLine, textWidth);
      if (!words.empty()) secondText = words.front();
    }
    area.push_back(line(styledText(secondText, dim)));
  } else {
    area.push_back(line(styledText(ellipsize(parts.type, static_cast<size_t>(textWidth)), dim)));
    area.push_back(line(uiHeaderText(ellipsize(parts.value, static_cast<size_t>(textWidth)), strong)));
    if (!parts.spec.empty()) {
      area.push_back(line(styledText(ellipsize(parts.spec, static_cast<size_t>(textWidth)), dim)));
    }
  }
  area.push_back(ftxui::filler());

  const auto quantityColor = uiQuantityColor(item, lowStockThreshold);
  ftxui::Elements quantityLine = {uiHeaderText(to_string(item.quantity), quantityColor)};
  if (item.quantity <= 0) quantityLine.push_back(uiHeaderText(" OUT", quantityColor));
  area.push_back(line(ftxui::hbox(move(quantityLine))));
  return ftxui::vbox(move(area)) | ftxui::yflex_grow;
}

}  // namespace rack_page_detail

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
  const int listWidth = screenWidth < 118 ? 26 : 31;
  int detailWidth = screenWidth < 118 ? 29 : (screenWidth < 160 ? 34 : 40);
  // In the horizontal layout, account for its two one-column separators. The
  // compact layout stacks the grid below the side panels, so it uses the full
  // screen width instead.
  int gridWidth = compact ? screenWidth : max(42, screenWidth - listWidth - detailWidth - 2);
  constexpr int rowDesignatorWidth = 3;
  const int colCount = rack != nullptr && rackRowCount(*rack) > 0 ? rackRowCount(*rack) : 5;
  const int separatorCount = colCount;
  const int slotColumnsSpace = max(colCount * 7, gridWidth - rowDesignatorWidth - separatorCount);
  const int slotWidth = rack_page_detail::equalRackSlotWidth(slotColumnsSpace, colCount);
  if (!compact && rack != nullptr && rackRowCount(*rack) > 0) {
    const int fittedGridWidth = rowDesignatorWidth + separatorCount + slotWidth * colCount;
    detailWidth += max(0, gridWidth - fittedGridWidth);
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
          fixedCell(" " + candidate.code, rackCodeWidth, fg),
          fixedCell(shortComponentType(toTitleCase(candidate.componentType)), rackTypeWidth,
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
      const int designatorWidth = rowHeaderWidth;
      // Fixed vertical chrome across the whole application window:
      // Shell: Header (1) + Top Divider (1) + Bottom Divider (1) + Search/Context (1) + Message (1) = 5 rows
      // Grid: Letter Header (1) + Header Divider (1) + (rackGridColumns - 1) row dividers + optional filter banner (1)
      const int shellChromeRows = 5;
      // Footer: one divider plus a summary band of at least one row.
      const int gridChromeRows = 2 + (rackGridColumns - 1) + 2 + (!rackFilter_.empty() ? 1 : 0);
      const int totalNonSlotRows = shellChromeRows + gridChromeRows;
      const int slotRowsSpace = max(rackGridColumns * 3, screenHeight - totalNonSlotRows);
      const int slotHeight = rack_page_detail::equalRackSlotHeight(slotRowsSpace, rackGridColumns);
      // Identical slots cannot absorb a remainder, and the letter header and row-number column never change size,
      // so the rows left over by the floor division go to the summary band under the matrix. The matrix then
      // ends exactly on the bottom edge instead of leaving a gap.
      const int footerBandRows = 1 + rack_page_detail::rackSlotRemainderRows(slotRowsSpace, rackGridColumns, slotHeight);
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
        // Keep every physical slot row identical. Floor division ensures that
        // the matrix fits strictly within the available vertical space.
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
          const int cellWidth = slotWidth;
          ftxui::Elements cellRows;
          if (item == nullptr) {
            cellRows.push_back(ftxui::filler());
            cellRows.push_back(ftxui::hbox({ftxui::filler(), styledText("empty", selected ? uiSecondaryText() : uiDimColor()), ftxui::filler()}) |
                               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth));
            cellRows.push_back(ftxui::filler());
          } else {
            cellRows.push_back(rack_page_detail::rackCellBody(*item, selected, settings_.lowStockThreshold, cellWidth, rowHeight));
          }
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
      // Footer band: rack summary, vertically centered in the rows the slots could not use.
      size_t lowSlots = 0;
      size_t outSlots = 0;
      for (const auto& candidate : store_.items()) {
        if (candidate.rackId != rack->id) continue;
        if (candidate.quantity <= 0) ++outSlots;
        else if (isLowStock(candidate, settings_.lowStockThreshold)) ++lowSlots;
      }
      ftxui::Elements summary = {styledText(" Rack " + rack->code + "  ", uiMutedColor()),
                                 uiHeaderText(to_string(rackOccupiedSlotCount(store_, *rack)) + "/" +
                                                  to_string(rackCapacity(*rack)) + " used",
                                              uiPrimaryText())};
      if (lowSlots > 0) summary.push_back(uiHeaderText("   " + to_string(lowSlots) + " low", uiWarnColor()));
      if (outSlots > 0) summary.push_back(uiHeaderText("   " + to_string(outSlots) + " out", uiDangerColor()));
      summary.push_back(ftxui::filler());
      gridRows.push_back(uiDivider());
      gridRows.push_back(ftxui::vbox({ftxui::filler(), ftxui::hbox(move(summary)), ftxui::filler()}) |
                         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, footerBandRows) | ftxui::bgcolor(uiPanelRightBg()));
    }
  }

  ftxui::Elements detailRows;
  if (rack == nullptr) {
    if (!inventoryHasNoRacks) {
      detailRows.push_back(fullLine("Slot detail", uiSecondaryText(), uiSurfaceBg()));
      detailRows.push_back(detailFieldLine({"Status: ", "No racks match filter", uiWarnColor(), uiTitleColor()}, detailWidth - 2));
    }
  } else {
    const int innerWidth = detailWidth - 2;
    auto selfDetail = const_cast<App*>(this);
    // Slot address on the left, what the rack holds on the right.
    detailRows.push_back(ftxui::hbox({
        styledText(" Slot: ", uiMutedColor()),
        uiHeaderText(selectedSlot, uiPrimaryText()),
        ftxui::filler(),
        styledText(toTitleCase(rack->componentType) + " ", uiMutedColor()),
    }));
    detailRows.push_back(uiDivider());
    if (selectedSlotItem == nullptr) {
      detailRows.push_back(uiHeaderText(" Empty slot", uiSecondaryText()));
      detailRows.push_back(ftxui::hbox({ftxui::text(" "), ftxui::paragraphAlignLeft(
                               movingRackItemId_.empty() ? "Press v on an occupied slot to pick up a part, then v here to place it."
                                                         : "Press v here to place the moving part.") |
                                                               ftxui::color(movingRackItemId_.empty() ? uiMutedColor()
                                                                                                      : uiAccentColor())}));
    } else {
      const auto& slotItem = *selectedSlotItem;
      // The part is the subject: full name in bold, then who makes it.
      detailRows.push_back(ftxui::hbox({ftxui::text(" "), ftxui::paragraphAlignLeft(slotItem.partName) | ftxui::bold |
                                                              ftxui::color(uiPrimaryText())}));
      detailRows.push_back(styledText(" " + ellipsize(trim(slotItem.manufacturer).empty() ? string("Unknown manufacturer")
                                                                                          : slotItem.manufacturer,
                                                      static_cast<size_t>(max(1, innerWidth - 1))),
                                      uiMutedColor()));
      detailRows.push_back(ftxui::text(""));

      const auto quantityColor = uiQuantityColor(slotItem, settings_.lowStockThreshold);
      const bool outOfStock = slotItem.quantity <= 0;
      const bool lowStock = !outOfStock && isLowStock(slotItem, settings_.lowStockThreshold);
      ftxui::Elements strip = {
          styledText(" Qty ", uiMutedColor(), uiRaisedSurfaceBg()),
          uiHeaderText(to_string(slotItem.quantity), quantityColor, uiRaisedSurfaceBg()),
      };
      if (outOfStock || lowStock) {
        strip.push_back(uiHeaderText(outOfStock ? "  OUT" : "  LOW", quantityColor, uiRaisedSurfaceBg()));
      }
      strip.push_back(ftxui::filler());
      detailRows.push_back(ftxui::hbox(move(strip)) | ftxui::bgcolor(uiRaisedSurfaceBg()));
      detailRows.push_back(ftxui::text(""));

      // The same specifications the Stock page leads with, then the package.
      size_t shown = 0;
      for (const auto& field : electricalFieldsForItem(slotItem)) {
        if (shown >= 3) break;
        if (field.label == "Package: " || trim(field.value).empty()) continue;
        detailRows.push_back(ftxui::hbox({ftxui::text(" "), detailFieldLine(field, innerWidth - 1)}));
        ++shown;
      }
      const auto package = packageSummary(slotItem);
      if (package != "-") {
        detailRows.push_back(ftxui::hbox({ftxui::text(" "), detailFieldLine({"Package: ", package, uiLabelColor(),
                                                                              uiTitleColor()}, innerWidth - 1)}));
      }
    }

    if (!movingRackItemId_.empty()) {
      detailRows.push_back(ftxui::text(""));
      const auto* moving = store_.findById(movingRackItemId_);
      detailRows.push_back(fullLine(" Moving: " + (moving == nullptr ? string("missing item") : ellipsize(moving->partName, 28)),
                                    uiWarnColor(), rackMovingBannerBg()));
      detailRows.push_back(styledText(" From " + movingRackSource_, uiMutedColor()));
    }

    // Controls and the rack summary sit at the bottom edge of the panel.
    detailRows.push_back(ftxui::filler());
    ftxui::Elements quantityControls;
    if (selectedSlotItem != nullptr) {
      quantityControls.push_back(styledText("Qty", uiMutedColor()));
      quantityControls.push_back(ftxui::text(" "));
      quantityControls.push_back(target(uiSecondaryButton("-"), "racks.part.minus", UiTargetKind::Button,
                                        [selfDetail] { selfDetail->adjustSelectedRackItemQuantity(-1); }));
      quantityControls.push_back(target(uiSecondaryButton("+"), "racks.part.plus", UiTargetKind::Button,
                                        [selfDetail] { selfDetail->adjustSelectedRackItemQuantity(1); }));
    }
    auto moveButton = target(selectedSlotItem != nullptr || !movingRackItemId_.empty() ? uiPrimaryButton("Move")
                                                                                       : uiSecondaryButton("Move"),
                             "racks.move", UiTargetKind::Button, [selfDetail] { selfDetail->beginOrCompleteRackMove(); });
    ftxui::Elements linkControls;
    if (selectedSlotItem != nullptr) {
      linkControls.push_back(target(uiSecondaryButton("Datasheet"), "racks.datasheet", UiTargetKind::Button,
                                    [selfDetail] { selfDetail->openSelectedRackDatasheet(); }));
      linkControls.push_back(ftxui::text(" "));
      linkControls.push_back(target(uiSecondaryButton("Remove", uiDangerColor()), "racks.part.remove",
                                    UiTargetKind::Button, [selfDetail] { selfDetail->unassignSelectedRackItem(); }));
    }
    if (rackOccupiedSlotCount(store_, *rack) == 0) {
      // Rack-level destructive action; arms the same modal as x.
      linkControls.push_back(target(uiSecondaryButton("Delete rack", uiDangerColor()), "racks.delete_rack",
                                    UiTargetKind::Button, [selfDetail] { selfDetail->deleteSelectedRack(); }));
    }
    // One natural row (Move, quantity, links, Remove) needs 39 columns. Narrower panels split it into two rows.
    const bool singleRow = detailWidth >= 40;
    ftxui::Elements firstRow = {ftxui::text(" "), move(moveButton)};
    if (!quantityControls.empty()) {
      firstRow.push_back(ftxui::text(" "));
      for (auto& element : quantityControls) firstRow.push_back(move(element));
    }
    if (singleRow) {
      if (!linkControls.empty()) {
        firstRow.push_back(ftxui::text(" "));
        for (auto& element : linkControls) firstRow.push_back(move(element));
      }
      detailRows.push_back(ftxui::hbox(move(firstRow)));
    } else {
      detailRows.push_back(ftxui::hbox(move(firstRow)));
      if (!linkControls.empty()) {
        ftxui::Elements secondRow = {ftxui::text(" ")};
        for (auto& element : linkControls) secondRow.push_back(move(element));
        detailRows.push_back(ftxui::hbox(move(secondRow)));
      }
    }
  }

  auto rackHeader = ftxui::vbox({
      fullLine("Racks", uiSecondaryText(), uiSurfaceBg()),
      ftxui::hbox({
          fixedCell("Rack", rackCodeWidth, uiMutedColor()),
          fixedCell("Type", rackTypeWidth, uiMutedColor()),
          fixedCell("Usage", rackUsedWidth, uiMutedColor(), true),
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

  ftxui::Element page;
  if (compact) {
    page = ftxui::vbox({
        ftxui::hbox({rackPanel, uiDivider(), detailPanel}),
        uiDivider(),
        gridPanel,
    });
  } else {
    page = ftxui::hbox({
        rackPanel,
        ftxui::separator() | ftxui::color(uiDimColor()),
        gridPanel,
        ftxui::separator() | ftxui::color(uiDimColor()),
        detailPanel,
    });
  }

  if (!rackDeleteConfirmationActive()) {
    return page;
  }

  const auto* rackToDelete = [&] {
    for (const auto& candidate : store_.racks()) {
      if (candidate.id == rackDeleteConfirmationRackId_) {
        return &candidate;
      }
    }
    return static_cast<const InventatoryRack*>(nullptr);
  }();

  const auto secondsLeft = rackDeleteConfirmationSecondsLeft();
  const int popupWidth = std::max(48, std::min(screenWidth - 12, 72));

  ftxui::Elements popupRows;
  popupRows.push_back(ftxui::paragraphAlignLeft("Are you sure you want to delete this rack from the database?") |
                      ftxui::color(uiTitleColor()));
  popupRows.push_back(uiDivider());
  popupRows.push_back(ftxui::paragraphAlignLeft(
                         "Selected: " + (rackToDelete == nullptr ? std::string("this rack") : rackToDelete->code)) |
                      ftxui::color(uiWarnColor()));
  popupRows.push_back(ftxui::paragraphAlignLeft("Press Enter to confirm after the timer unlocks.") |
                      ftxui::color(secondsLeft == 0 ? uiTitleColor() : uiMutedColor()));
  popupRows.push_back(ftxui::paragraphAlignLeft("Press Esc to cancel.") | ftxui::color(uiMutedColor()));
  popupRows.push_back(ftxui::paragraphAlignLeft(std::to_string(secondsLeft) + " second" +
                                                (secondsLeft == 1 ? std::string() : std::string("s")) + " remaining") |
                      ftxui::color(uiWarnColor()));

  auto popup = ftxui::window(styledText("Delete rack", uiDangerColor()),
                             ftxui::vbox(move(popupRows)) | ftxui::bgcolor(uiPanelRightBg()) |
                                 ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, popupWidth)) |
               ftxui::color(uiDangerColor());

  auto overlay = ftxui::vbox({
      ftxui::filler(),
      ftxui::hbox({
          ftxui::filler(),
          popup,
          ftxui::filler(),
      }),
      ftxui::filler(),
  });

  return ftxui::dbox({
      page,
      overlay,
  });
}

void App::handleRackManagementKey(const KeyEvent& key) {
  // Rack delete confirmation is modal, mirroring the stock item flow: Enter
  // confirms once unlocked, anything else cancels.
  if (rackDeleteConfirmationActive()) {
    if (key.type == KeyType::Enter) {
      confirmRackDeletion();
      return;
    }
    cancelRackDeletion();
    return;
  }

  if (key.type == KeyType::CtrlZ) {
    undoLastInventoryChange();
    syncRackSelection();
    return;
  }

  if (key.type == KeyType::Tab || key.type == KeyType::Escape) {
    changePage(Page::Stock);
    return;
  }

  if (key.type == KeyType::Enter) {
    const auto* item = selectedRackItem();
    if (item == nullptr) {
      setMessage("No part in this slot", 2);
      return;
    }
    if (selectStockItemById(item->id)) changePage(Page::Stock);
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
