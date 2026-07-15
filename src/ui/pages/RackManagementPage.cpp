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

// A part mid-move gets a distinct turquoise highlight: brighter on its grid
// cell, dimmer on the informational banner in the slot detail panel.
ftxui::Color rackMovingSourceBg() {
  return ftxui::Color::RGB(18, 61, 64);
}

ftxui::Color rackMovingBannerBg() {
  return ftxui::Color::RGB(24, 48, 50);
}

ftxui::Element rackQuantityIndicator(const InventoryItem& item, bool selected) {
  const auto foreground = item.quantity <= 0 ? uiDangerColor()
                        : item.lowStock() ? uiWarnColor()
                                          : uiSuccessColor();
  const auto background = selected ? uiSelectionBg()
                        : item.quantity <= 0 ? ftxui::Color::RGB(47, 27, 27)
                        : item.lowStock() ? ftxui::Color::RGB(48, 39, 24)
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
  const int gridWidth = max(42, screenWidth - listWidth - detailWidth - 4);
  const auto rackIndices = sortedRackIndices();
  const auto* rack = selectedRack();
  const auto selectedSlot = selectedRackSlot();
  const auto* selectedSlotItem = selectedRackItem();

  ftxui::Elements rackRows;
  rackRows.push_back(ftxui::hbox({
      rackFixedCell("Rack", 6, uiMutedColor()),
      rackFixedCell("Type", max(8, listWidth - 18), uiMutedColor()),
      rackFixedCell("Used", 7, uiMutedColor(), true),
  }) | ftxui::bgcolor(uiPanelLeftBg()));
  if (rackIndices.empty()) {
    rackRows.push_back(fullLine(rackFilter_.empty() ? "No racks yet." : "No racks match filter.", uiMutedColor(),
                                uiPanelLeftBg()));
  } else {
    for (size_t visible = 0; visible < rackIndices.size(); ++visible) {
      const auto& candidate = store_.racks()[rackIndices[visible]];
      const bool selected = visible == min(rackSelection_, rackIndices.size() - 1);
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto fg = selected ? uiFocusColor() : uiPrimaryText();
      const auto occupied = rackOccupiedSlotCount(store_, candidate);
      auto rackRow = ftxui::hbox({
          rackFixedCell(" " + candidate.code, 6, fg),
          rackFixedCell(candidate.componentType, max(8, listWidth - 18), selected ? uiTitleColor() : uiLabelColor()),
          rackFixedCell(to_string(occupied) + "/25", 7, occupied >= 25 ? uiWarnColor() : uiSuccessColor(), true),
      }) | ftxui::bgcolor(bg);
      auto self = const_cast<App*>(this);
      rackRows.push_back(target(rackRow, "racks.row." + candidate.id, UiTargetKind::Row, [self, visible] {
        self->rackSelection_ = visible;
        self->rackRow_ = 0;
        self->rackColumn_ = 0;
        self->dirty_ = true;
      }));
    }
  }

  ftxui::Elements gridRows;
  if (rack == nullptr) {
    gridRows.push_back(fullLine("Eligible inventory will create racks automatically.", uiMutedColor(), uiPanelRightBg()));
  } else {
    gridRows.push_back(fullLine(rack->code + "  " + rack->componentType + "  " +
                                    to_string(rackOccupiedSlotCount(store_, *rack)) + "/25 occupied",
                                uiAccentColor(), uiPanelRightBg()));
    if (!rackFilter_.empty()) {
      gridRows.push_back(fullLine("Filter: " + rackFilter_, uiWarnColor(), uiPanelRightBg()));
    }
    gridRows.push_back(uiDivider());
    const int slotWidth = max(7, (gridWidth - 8) / 5);
    // Expand the five rows into the otherwise unused grid workspace. Longer
    // part names can still grow their cell and remain reachable by scrolling.
    const int slotHeight = max(3, (screenHeight - 13) / 5);
    for (int row = 0; row < 5; ++row) {
      ftxui::Elements rowCells;
      for (int column = 0; column < 5; ++column) {
        const auto slot = rackSlotLabel(row, column);
        const auto* item = itemAtRackSlot(store_, rack->id, slot);
        const bool selected = row == rackRow_ && column == rackColumn_;
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
        auto cell = ftxui::vbox(move(cellRows)) | ftxui::bgcolor(bg) |
                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL, slotWidth) |
                    ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, slotHeight);
        if (selected) {
          cell = cell | ftxui::select;
        }
        auto self = const_cast<App*>(this);
        rowCells.push_back(target(cell, "racks.cell." + slot, UiTargetKind::Cell, [self, row, column] {
          self->rackRow_ = row;
          self->rackColumn_ = column;
          self->dirty_ = true;
        }));
        if (column < 4) {
          rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        }
      }
      gridRows.push_back(ftxui::hbox(move(rowCells)));
      if (row < 4) {
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
  detailRows.push_back(fullLine("Selected slot", uiSecondaryText(), uiSurfaceBg()));
  if (rack == nullptr) {
    detailRows.push_back(detailFieldLine({"Status: ", "No racks", uiWarnColor(), uiTitleColor()}, detailWidth - 2));
    detailRows.push_back(ftxui::paragraphAlignLeft("Add or import an eligible small component to create racks automatically.") |
                         ftxui::color(uiMutedColor()));
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
  detailRows.push_back(ftxui::filler());
  detailRows.push_back(uiDivider());
  detailRows.push_back(ftxui::hbox(move(primaryActions)));
  if (!secondaryActions.empty()) detailRows.push_back(ftxui::hbox(move(secondaryActions)));
  rackRows.insert(rackRows.begin(), fullLine("RACKS", uiSecondaryText(), uiSurfaceBg()));
  gridRows.insert(gridRows.begin(), fullLine("RACK GRID", uiSecondaryText(), uiSurfaceBg()));
  detailRows.insert(detailRows.begin(), fullLine("SLOT DETAIL", uiSecondaryText(), uiSurfaceBg()));
  auto rackPanel = ftxui::vbox(move(rackRows)) | ftxui::bgcolor(uiSurfaceBg()) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listWidth);
  auto gridPanel = ftxui::vbox(move(gridRows)) | ftxui::yframe | ftxui::vscroll_indicator | ftxui::bgcolor(uiSurfaceBg()) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, gridWidth) | ftxui::flex;
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
    moveRackSlot(-1, 0);
  } else if (key.type == KeyType::Down) {
    moveRackSlot(1, 0);
  } else if (key.type == KeyType::Left) {
    moveRackSlot(0, -1);
  } else if (key.type == KeyType::Right) {
    moveRackSlot(0, 1);
  } else if (key.type == KeyType::Character) {
    // Every other command on this screen (jump, filter, move/place, print,
    // rename, create/delete rack, quit, ...) is a registered action in
    // ActionRegistry.cpp and is dispatched before this handler ever runs;
    // only vim-style slot movement lives here.
    switch (tolower(static_cast<unsigned char>(key.ch))) {
      case 'h':
        moveRackSlot(0, -1);
        break;
      case 'j':
        moveRackSlot(1, 0);
        break;
      case 'k':
        moveRackSlot(-1, 0);
        break;
      case 'l':
        moveRackSlot(0, 1);
        break;
      default:
        break;
    }
  }
}

}  // namespace inventatory
