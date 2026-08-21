// Inventatory - Hardware Inventory Management System
// Inventatory Rack management page rendering and keyboard handling.

#include "App.h"

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
  auto content = rightAlign
                     ? ftxui::hbox({ftxui::filler(), styledText(ellipsize(text, static_cast<size_t>(max(0, width))), color)})
                     : ftxui::hbox({styledText(ellipsize(text, static_cast<size_t>(max(0, width))), color), ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
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

ftxui::Element rackQuantityIndicator(const InventoryItem& item, bool selected) {
  const auto foreground = item.quantity <= 0 ? uiDangerColor()
                        : item.lowStock() ? uiWarnColor()
                                          : uiSuccessColor();
  const auto background = selected ? uiSelectionBg()
                        : item.quantity <= 0 ? uiDangerBg()
                        : item.lowStock() ? uiWarningBg()
                                          : uiRaisedSurfaceBg();
  return styledText(" Quantity:[" + to_string(item.quantity) + "] ", foreground, background) | ftxui::bold;
}

}  // namespace

ftxui::Element App::renderRackManagementUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 40;
  // The supported 100-column terminal still keeps the rack matrix beside its
  // context panels. Stacking would make the 5x5 grid taller than the viewport.
  const bool compact = screenWidth < 100;
  const int listWidth = screenWidth < 118 ? 24 : 30;
  const int detailWidth = screenWidth < 118 ? 30 : 36;
  // In the horizontal layout, account for its two one-column separators. The
  // compact layout stacks the grid below the side panels, so it uses the full
  // screen width instead.
  const int gridWidth = compact ? screenWidth : max(42, screenWidth - listWidth - detailWidth - 2);
  const auto rackIndices = sortedRackIndices();
  const bool inventoryHasNoRacks = store_.racks().empty();
  const auto* rack = selectedRack();
  const auto selectedSlot = selectedRackSlot();
  const auto* selectedSlotItem = selectedRackItem();

  // Rack code, type, and a right-aligned occupancy column that ends one cell
  // short of the panel edge. Type absorbs the remaining width so the numbers
  // sit flush right instead of floating in the middle of the panel.
  constexpr int rackCodeWidth = 6;
  constexpr int rackUsedWidth = 7;
  const int rackTypeWidth = max(8, listWidth - rackCodeWidth - rackUsedWidth - 1);

  ftxui::Elements rackRows;
  rackRows.push_back(ftxui::hbox({
      rackFixedCell("Rack", rackCodeWidth, uiMutedColor()),
      rackFixedCell("Type", rackTypeWidth, uiMutedColor()),
      rackFixedCell("Used", rackUsedWidth, uiMutedColor(), true),
      ftxui::text(" "),
  }) | ftxui::bgcolor(uiPanelLeftBg()));
  if (!rackIndices.empty()) {
    for (size_t visible = 0; visible < rackIndices.size(); ++visible) {
      const auto& candidate = store_.racks()[rackIndices[visible]];
      const bool selected = visible == min(rackSelection_, rackIndices.size() - 1);
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto fg = selected ? uiFocusColor() : uiPrimaryText();
      const auto occupied = rackOccupiedSlotCount(store_, candidate);
      auto rackRow = ftxui::hbox({
          rackFixedCell(" " + candidate.code, rackCodeWidth, fg),
          rackFixedCell(candidate.componentType, rackTypeWidth, selected ? uiTitleColor() : uiLabelColor()),
          rackFixedCell(to_string(occupied) + "/25", rackUsedWidth, occupied >= 25 ? uiWarnColor() : uiSuccessColor(),
                        true),
          ftxui::text(" "),
      }) | ftxui::bgcolor(bg);
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
    gridRows.push_back(fullLine(rack->code + "  " + rack->componentType + "  " +
                                    to_string(rackOccupiedSlotCount(store_, *rack)) + "/25 occupied",
                                uiAccentColor(), uiPanelRightBg()));
    if (!rackFilter_.empty()) {
      gridRows.push_back(fullLine("Filter: " + rackFilter_, uiWarnColor(), uiPanelRightBg()));
    }
    gridRows.push_back(uiDivider());
    // Four one-column separators divide the five cells. Distribute the
    // remaining columns across the first cells so there is no trailing gap.
    const int slotSpace = gridWidth - 4;
    const int slotWidth = max(7, slotSpace / 5);
    const int extraSlotColumns = max(0, slotSpace - slotWidth * 5);
    // Expand the five rows into the otherwise unused grid workspace. Longer
    // part names can still grow their cell and remain reachable by scrolling.
    const int slotHeight = max(3, (screenHeight - 13) / 5);
    // Display the rack like the physical unit: slot numbers run downward
    // within each lettered column, while letters advance from left to right.
    // Keep the existing rackRow_/rackColumn_ state and slot lookup untouched
    // by translating the visual coordinates back to storage coordinates here.
    for (int displayRow = 0; displayRow < 5; ++displayRow) {
      ftxui::Elements rowCells;
      for (int displayColumn = 0; displayColumn < 5; ++displayColumn) {
        const auto slot = rackSlotLabel(displayColumn, displayRow);
        const auto* item = itemAtRackSlot(store_, rack->id, slot);
        const bool selected = displayColumn == rackRow_ && displayRow == rackColumn_;
        const bool movingSource = item != nullptr && item->id == movingRackItemId_;
        const auto bg = movingSource ? rackMovingSourceBg()
                        : selected ? uiSelectionBg()
                                   : (item == nullptr ? uiCanvasBg() : uiSurfaceBg());
        const auto titleColor = selected ? uiTitleColor() : (item == nullptr ? uiMutedColor() : uiAccentColor());
        const auto itemText = item == nullptr ? string("[ empty ]") : item->partName;
        ftxui::Elements cellRows;
        cellRows.push_back(ftxui::hbox({
            ftxui::filler(),
            styledText(slot, titleColor) | ftxui::bold,
            ftxui::filler(),
        }));
        cellRows.push_back(item == nullptr
                               ? styledText("available", uiDimColor())
                               : ftxui::hbox({ftxui::filler(), rackQuantityIndicator(*item, selected), ftxui::filler()}));
        cellRows.push_back(ftxui::paragraphAlignLeft(itemText) |
                           ftxui::color(item == nullptr ? uiDimColor() : uiTitleColor()));
        const int cellWidth = slotWidth + (displayColumn < extraSlotColumns ? 1 : 0);
        auto cell = ftxui::vbox(move(cellRows)) | ftxui::bgcolor(bg) |
                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth) |
                    ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, slotHeight);
        if (selected) {
          cell = cell | ftxui::select;
        }
        auto self = const_cast<App*>(this);
        rowCells.push_back(target(cell, "racks.cell." + slot, UiTargetKind::Cell, [self, displayColumn, displayRow] {
          self->rackRow_ = displayColumn;
          self->rackColumn_ = displayRow;
          self->dirty_ = true;
        }));
        if (displayColumn < 4) {
          rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        }
      }
      gridRows.push_back(ftxui::hbox(move(rowCells)));
      if (displayRow < 4) {
        gridRows.push_back(uiDivider());
      }
    }
  }

  ftxui::Elements detailRows;
  auto self = const_cast<App*>(this);
  ftxui::Elements primaryActions;
  primaryActions.push_back(target(styledText(" Place / Move ", rack != nullptr ? uiInteractiveColor() : uiMutedText(),
                                                   uiRaisedSurfaceBg()),
                                  "racks.move", UiTargetKind::Button, [self] { self->beginOrCompleteRackMove(); },
                                  rack != nullptr));
  primaryActions.push_back(ftxui::text(" "));
  primaryActions.push_back(target(styledText(" Print part ", selectedSlotItem != nullptr ? uiInteractiveColor() : uiMutedText(),
                                                   uiRaisedSurfaceBg()),
                                  "racks.print.part", UiTargetKind::Button,
                                  [self] { self->printSelectedRackPartLabel(); }, selectedSlotItem != nullptr));
  ftxui::Elements secondaryActions;
  if (selectedSlotItem != nullptr) {
    secondaryActions.push_back(target(styledText(" - ", uiInteractiveColor(), uiRaisedSurfaceBg()),
                                      "racks.part.minus", UiTargetKind::Button,
                                      [self] { self->adjustSelectedRackItemQuantity(-1); }));
    secondaryActions.push_back(target(styledText(" + ", uiInteractiveColor(), uiRaisedSurfaceBg()),
                                      "racks.part.plus", UiTargetKind::Button,
                                      [self] { self->adjustSelectedRackItemQuantity(1); }));
    secondaryActions.push_back(ftxui::text(" "));
    secondaryActions.push_back(target(styledText(" Details ", uiInteractiveColor(), uiRaisedSurfaceBg()),
                                      "racks.part.details", UiTargetKind::Button,
                                      [self] { self->openSelectedRackItemDetail(); }));
  }
  if (!inventoryHasNoRacks) detailRows.push_back(fullLine("Selected slot", uiSecondaryText(), uiSurfaceBg()));
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
      detailRows.push_back(fullLine("Empty slot", uiMutedColor(), uiRowDarkBg()));
      detailRows.push_back(ftxui::paragraphAlignLeft(movingRackItemId_.empty()
                                                         ? "Press v on an occupied slot to start moving a part."
                                                         : "Press v here to place the moving part.") |
                           ftxui::color(movingRackItemId_.empty() ? uiMutedColor() : uiAccentColor()));
    } else {
      detailRows.push_back(fullLine("Inventatory RACK: " + rack->code + "-" + selectedSlot, uiTitleColor(), uiRowSelectedBg()));
      detailRows.push_back(detailFieldLine({"Part: ", selectedSlotItem->partName, uiLabelColor(), uiTitleColor()}, detailWidth - 2));
      detailRows.push_back(detailFieldLine({"Category: ", displayCategory(selectedSlotItem->category), uiLabelColor(), uiTitleColor()},
                                          detailWidth - 2));
      detailRows.push_back(detailFieldLine({"Package: ", packageSummary(*selectedSlotItem), uiLabelColor(), uiTitleColor()},
                                          detailWidth - 2));
      detailRows.push_back(detailFieldLine({"Mode: ", assignmentLabel(selectedSlotItem->rackAssignment), uiLabelColor(),
                                           selectedSlotItem->rackAssignment == RackAssignmentMode::Manual ? uiWarnColor()
                                                                                                         : uiSuccessColor()},
                                          detailWidth - 2));
      detailRows.push_back(detailFieldLine({"Qty: ", to_string(selectedSlotItem->quantity), uiLabelColor(), uiTitleColor()},
                                          detailWidth - 2));
    }
    if (!movingRackItemId_.empty()) {
      detailRows.push_back(uiDivider());
      const auto* moving = store_.findById(movingRackItemId_);
      detailRows.push_back(fullLine("Moving: " + (moving == nullptr ? string("missing item") : ellipsize(moving->partName, 28)),
                                    uiWarnColor(), rackMovingBannerBg()));
      detailRows.push_back(styledText("From " + movingRackSource_, uiMutedColor()));
    }
  }
  if (!inventoryHasNoRacks) {
    detailRows.push_back(ftxui::filler());
    detailRows.push_back(uiDivider());
    detailRows.push_back(ftxui::hbox(move(primaryActions)));
    if (!secondaryActions.empty()) detailRows.push_back(ftxui::hbox(move(secondaryActions)));
  }
  rackRows.insert(rackRows.begin(), fullLine("RACKS", uiSecondaryText(), uiSurfaceBg()));
  if (!inventoryHasNoRacks) detailRows.insert(detailRows.begin(), fullLine("SLOT DETAIL", uiSecondaryText(), uiSurfaceBg()));
  auto rackPanel = ftxui::vbox(move(rackRows)) | ftxui::bgcolor(uiSurfaceBg()) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listWidth);
  auto gridPanel = ftxui::vbox(move(gridRows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg()) |
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
