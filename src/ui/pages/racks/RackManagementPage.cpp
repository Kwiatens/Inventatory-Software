// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Rack management page rendering and keyboard handling.

#include "App.h"

#include "core/racks/StorageGridLayout.h"
#include "ui/pages/racks/RackManagementPagePrivate.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <limits>
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
  return storageGridRows(rack);
}

int rackColumnCount(const InventatoryRack& rack) {
  return storageGridColumns(rack);
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

string packageSummary(const InventoryItem& item) {
  for (const auto& parameter : item.parameters) {
    if (parameterLabelMatches(parameter.name, "Package") || parameterLabelMatches(parameter.name, "Package / Case")) {
      return parameter.value;
    }
  }
  return "-";
}

// The moving part's grid cell and its detail banner use different active fills.
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

string storageLetterHeader(const StorageGridFit& fit, int displayColumn) {
  const string letter(1, static_cast<char>('A' + fit.letterStart + displayColumn));
  string label = letter;
  if (displayColumn == 0 && fit.hiddenLeft()) label = "◂ " + label;
  if (displayColumn == fit.letterCount - 1 && fit.hiddenRight()) label += " ▸";
  return label;
}

string storageScrollText(const StorageGridFit& fit, bool above) {
  const int rows = above ? fit.rowsAbove() : fit.rowsBelow();
  // The far end says so, so the bar line never looks like a gap.
  if (rows <= 0) return above ? "· top ·" : "· bottom ·";
  return string(above ? "▲ " : "▼ ") + to_string(rows) + (rows == 1 ? " more row " : " more rows ") +
         (above ? "above" : "below");
}

// One slot's contents: type, bold value and specification as the cell height allows,
// then the quantity. Out of stock also says OUT.
ftxui::Element rackCellBody(const InventoryItem& item, bool selected, int lowStockThreshold, int cellWidth,
                            int rowHeight) {
  // Narrow cells drop their padding so a value such as 0.47UF stays whole.
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
  // Centre the text in tall cells; the quantity stays at the bottom.
  area.push_back(ftxui::filler());
  if (parts.type.empty() || areaLines == 1) {
    // No recognizable value, or no room to split: the whole name in bold, wrapped to the cell.
    if (areaLines == 1) {
      // One line: the value alone ("100nF") says more than the start of the full name.
      if (!parts.value.empty()) {
        area.push_back(line(uiHeaderText(ellipsize(parts.value, static_cast<size_t>(textWidth)), strong)));
      } else {
        area.push_back(line(uiPartName(item.partName, textWidth, selected)));
      }
    } else {
      auto lines = wrapText(item.partName, textWidth);
      if (static_cast<int>(lines.size()) > areaLines) {
        lines.resize(static_cast<size_t>(areaLines));
        lines.back() = ellipsize(lines.back() + "...", static_cast<size_t>(textWidth));
      }
      for (const auto& text : lines) area.push_back(line(uiHeaderText(text, strong)));
    }
  } else if (static_cast<int>(ftxui::string_width(parts.type)) > textWidth) {
    // Too narrow for the type, which the rack already implies: value first, then the spec wrapped.
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
    // Two lines: the value, then the specification (the rack type already implies the kind).
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
  // Even at 100 columns the matrix stays beside the panels.
  // Stacking would make the 5x5 grid too tall.
  const bool compact = screenWidth < 100;
  const int listWidth = screenWidth < 118 ? 26 : 31;
  int detailWidth = screenWidth < 118 ? 29 : (screenWidth < 160 ? 34 : 40);
  // Horizontal layout reserves the two separator columns. Compact uses the full width.
  int gridWidth = compact ? screenWidth : max(42, screenWidth - listWidth - detailWidth - 2);
  const bool hasGrid = rack != nullptr && rackRowCount(*rack) > 0 && rackColumnCount(*rack) > 0;
  const bool plainRack = rack != nullptr && !isOrganizer(*rack);
  // Two-digit row numbers need a wider column once an arrow marks rows above or below the screen.
  const int rowDesignatorWidth = hasGrid && !plainRack && rackColumnCount(*rack) >= 10 ? 4 : 3;
  // Rows outside the slots: the shell (5), the letter row and its divider,
  // the optional filter banner, and the footer (a divider and a summary band).
  const int slotRowsSpace = screenHeight - 5 - 4 - (!rackFilter_.empty() ? 1 : 0);
  StorageGridFit gridFit;
  if (hasGrid) {
    StorageGridRequest request;
    request.letters = rackRowCount(*rack);
    request.numbers = rackColumnCount(*rack);
    request.height = slotRowsSpace;
    request.focus = {{rackRow_, rackColumn_}};
    request.width = gridWidth - rowDesignatorWidth - 1;
    gridFit = fitStorageGrid(request);
    if (!compact && gridFit.letterCount < gridFit.letters) {
      // A wide organizer takes width from the slot panel before it scrolls sideways.
      constexpr int narrowestDetail = 29;
      request.width = max(request.width, screenWidth - listWidth - narrowestDetail - 2 - rowDesignatorWidth - 1);
      gridFit = fitStorageGrid(request);
    }
    if (!compact) {
      const int fittedGridWidth = rowDesignatorWidth + 1 + gridFit.usedWidth();
      detailWidth = max(20, screenWidth - listWidth - 2 - fittedGridWidth);
      gridWidth = fittedGridWidth;
    }
  }
  const int slotWidth = gridFit.cellWidth;
  // PageUp and PageDown move by the rows on screen.
  rackVisibleNumbers_ = max(1, gridFit.numberCount);

  // Code, type, then a right-aligned occupancy column. Type takes the remaining width.
  // The scroll indicator takes the last column, so the table is one column narrower.
  const int rackContentWidth = listWidth - 1;
  // Names win over types: a name keeps up to 14 cells, and the type gives up its width first.
  TableColumn codeColumn{5, 5}, typeColumn{0, 0, true}, usedColumn{0, 6};
  usedColumn.fit("Usage");
  for (const auto index : rackIndices) {
    const auto& candidate = store_.racks()[index];
    typeColumn.fit(shortComponentType(toTitleCase(candidate.componentType)));
    usedColumn.fit(to_string(rackOccupiedSlotCount(store_, candidate)) + "/" + to_string(rackCapacity(candidate)));
  }
  // A blank cell follows the usage count.
  const auto rackWidths = fitColumns({codeColumn, typeColumn, usedColumn}, rackContentWidth - 1);
  const int rackCodeWidth = rackWidths[0];
  const int rackTypeWidth = rackWidths[1];
  const int rackUsedWidth = rackWidths[2];

  ftxui::Elements rackRows;
  if (!rackIndices.empty()) {
    for (size_t visible = 0; visible < rackIndices.size(); ++visible) {
      const auto& candidate = store_.racks()[rackIndices[visible]];
      const bool selected = visible == min(rackSelection_, rackIndices.size() - 1);
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto fg = selected ? uiFocusColor() : uiPrimaryText();
      const auto occupied = rackOccupiedSlotCount(store_, candidate);
      const auto capacity = rackCapacity(candidate);
      // Each type starts two cells after its own name, so one long name moves only its own row.
      const int nameWidth = min(static_cast<int>(displayWidth(candidate.code)) + 1, 15);
      auto rackRow = ftxui::hbox({
          // Two blank cells keep a name apart from the type, so "Tray 2" and "Screws" never read as one name.
          fixedCell(" " + candidate.code, nameWidth, fg),
          ftxui::text("  "),
          fixedCell(shortComponentType(toTitleCase(candidate.componentType)), max(0, rackCodeWidth + rackTypeWidth - nameWidth - 2),
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
        self->selectRackAt(visible);
      }));
    }
  } else if (!inventoryHasNoRacks) {
    rackRows.push_back(fullLine("No racks match filter.", uiMutedColor(), uiPanelLeftBg()));
  }

  ftxui::Elements gridRows;
  if (rack == nullptr) {
    const auto emptyState = inventoryHasNoRacks ? "Add or import an eligible component to create racks automatically, or press New storage."
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
    const int fullGridRows = rackRowCount(*rack);
    const int fullGridColumns = rackColumnCount(*rack);
    if (fullGridRows == 0 || fullGridColumns == 0) {
      gridRows.push_back(fullLine("Rack dimensions are unavailable.", uiWarnColor(), uiPanelRightBg()));
    } else {
      // Letters across the top and numbers down the left. Arrows at the edges mark slots off screen.
      const int designatorWidth = rowDesignatorWidth;
      const int letterStart = gridFit.letterStart;
      const int numberStart = gridFit.numberStart;
      const int visibleLetters = gridFit.letterCount;
      const int visibleNumbers = gridFit.numberCount;
      // Leftover rows go to the summary band, so the matrix ends on the bottom edge.
      const int footerBandRows = 1 + max(0, slotRowsSpace - gridFit.usedHeight());
      ftxui::Elements columnHeaders;
      columnHeaders.push_back(rackCenteredCell("", designatorWidth, uiDimColor()));
      columnHeaders.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
      for (int displayColumn = 0; displayColumn < visibleLetters; ++displayColumn) {
        columnHeaders.push_back(rackCenteredCell(rack_page_detail::storageLetterHeader(gridFit, displayColumn), slotWidth,
                                                 uiAccentColor()));
        if (displayColumn < visibleLetters - 1) {
          columnHeaders.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        }
      }
      gridRows.push_back(ftxui::hbox(move(columnHeaders)) | ftxui::bgcolor(uiPanelRightBg()));
      gridRows.push_back(uiDivider());
      // While the rows scroll, a bar above and below says how many rows are off screen. A click scrolls one row.
      const auto scrollBar = [&](bool above) {
        const auto text = rack_page_detail::storageScrollText(gridFit, above);
        const bool more = (above ? gridFit.rowsAbove() : gridFit.rowsBelow()) > 0;
        auto bar = ftxui::hbox({ftxui::filler(), styledText(text, more ? uiAccentColor() : uiDimColor()), ftxui::filler()}) |
                   ftxui::bgcolor(uiPanelRightBg());
        if (!more) return bar;
        auto self = const_cast<App*>(this);
        const int row = above ? numberStart - 1 : numberStart + visibleNumbers;
        return target(move(bar), above ? "racks.scroll.up" : "racks.scroll.down", UiTargetKind::Row,
                      [self, row] { self->jumpToRackSlot(self->rackRow_, row); });
      };
      if (gridFit.scrollBars) gridRows.push_back(scrollBar(true));
      // Numbers run down each lettered column, as on the rack. Storage coordinates are unchanged.
      for (int displayRow = 0; displayRow < visibleNumbers; ++displayRow) {
        const int rowHeight = gridFit.rowHeight(displayRow);
        ftxui::Elements rowCells;
        auto rowDesignator = ftxui::vbox({
                                  ftxui::filler(),
                                  ftxui::hbox({ftxui::filler(),
                                               styledText(to_string(numberStart + displayRow + 1), uiAccentColor()),
                                               ftxui::filler()}),
                                  ftxui::filler(),
                              }) |
                             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, designatorWidth) |
                             ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, rowHeight);
        rowCells.push_back(move(rowDesignator));
        rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
        for (int displayColumn = 0; displayColumn < visibleLetters; ++displayColumn) {
          const int slotLetter = letterStart + displayColumn;
          const int slotNumber = numberStart + displayRow;
          const auto slot = rackSlotLabel(slotLetter, slotNumber);
          const auto* item = itemAtRackSlot(store_, rack->id, slot);
          const bool selected = slotLetter == rackRow_ && slotNumber == rackColumn_;
          const bool movingSource = item != nullptr && item->id == movingRackItemId_;
          const auto bg = movingSource ? rackMovingSourceBg()
                          : selected ? uiSelectionBg()
                                     : (item == nullptr ? uiCanvasBg() : uiSurfaceBg());
          const int cellWidth = slotWidth;
          ftxui::Elements cellRows;
          if (item == nullptr) {
            // Short or narrow cells mark an empty slot with a dot, so the full ones stand out.
            const auto mark = rowHeight >= 3 && cellWidth >= 8 ? string("empty") : string("·");
            cellRows.push_back(ftxui::filler());
            cellRows.push_back(ftxui::hbox({ftxui::filler(), styledText(mark, selected ? uiSecondaryText() : uiDimColor()), ftxui::filler()}) |
                               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth));
            cellRows.push_back(ftxui::filler());
          } else {
            cellRows.push_back(rack_page_detail::rackCellBody(*item, selected, settings_.lowStockThreshold, cellWidth, rowHeight));
          }
          // Fill after the fixed size so every slot paints its full rectangle.
          auto cell = ftxui::vbox(move(cellRows)) |
                      ftxui::size(ftxui::WIDTH, ftxui::EQUAL, cellWidth) |
                      ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, rowHeight) |
                      ftxui::bgcolor(bg);
          if (selected) {
            cell = cell | ftxui::select;
          }
          auto self = const_cast<App*>(this);
          rowCells.push_back(target(cell, "racks.cell." + slot, UiTargetKind::Cell, [self, slotLetter, slotNumber] {
            self->rackRow_ = slotLetter;
            self->rackColumn_ = slotNumber;
            self->dirty_ = true;
          }));
          if (displayColumn < visibleLetters - 1) {
            rowCells.push_back(ftxui::separator() | ftxui::color(uiDimColor()));
          }
        }
        gridRows.push_back(ftxui::hbox(move(rowCells)));
        if (displayRow < visibleNumbers - 1) {
          gridRows.push_back(uiDivider());
        }
      }
      if (gridFit.scrollBars) gridRows.push_back(scrollBar(false));
      // Summary band, centred in the rows the slots could not use.
      size_t lowSlots = 0;
      size_t outSlots = 0;
      for (const auto& candidate : store_.items()) {
        if (candidate.rackId != rack->id) continue;
        if (candidate.quantity <= 0) ++outSlots;
        else if (isLowStock(candidate, settings_.lowStockThreshold)) ++lowSlots;
      }
      ftxui::Elements summary = {styledText((isOrganizer(*rack) ? " " : " Rack ") + rack->code + "  ", uiMutedColor()),
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
  if (inputMode_ == InputMode::StorageNew || inputMode_ == InputMode::StorageForm) {
    auto selfForm = const_cast<App*>(this);
    const int formInner = max(10, detailWidth - 2);
    const auto wide = [&](const string& text, ftxui::Color color) {
      detailRows.push_back(ftxui::hbox({ftxui::text(" "), ftxui::paragraphAlignLeft(text) | ftxui::color(color) | ftxui::flex}));
    };
    const auto formButton = [&](const string& label, const string& keyName, UiButtonKind kind, const string& id,
                                function<void()> action) {
      return target(uiButton(label, keyName, kind), id, UiTargetKind::Button, move(action));
    };
    detailRows.push_back(uiHeaderText(" New storage", uiSecondaryText()));
    detailRows.push_back(uiDivider());
    if (inputMode_ == InputMode::StorageNew) {
      wide("What are you adding?", uiSecondaryText());
      detailRows.push_back(ftxui::text(""));
      detailRows.push_back(ftxui::hbox({ftxui::text(" "),
                                        formButton("Container with slots", "Enter", UiButtonKind::Primary, "racks.new.container",
                                                   [selfForm] { selfForm->beginStorageForm(); }),
                                        ftxui::filler()}));
      detailRows.push_back(ftxui::text(""));
      detailRows.push_back(ftxui::hbox({ftxui::text(" "),
                                        formButton("Rack with slots", "r", UiButtonKind::Normal, "racks.new.rack",
                                                   [selfForm] {
                                                     KeyEvent press;
                                                     press.type = KeyType::Character;
                                                     press.ch = 'r';
                                                     selfForm->handleStorageNewKey(press);
                                                   }),
                                        ftxui::filler()}));
      detailRows.push_back(ftxui::text(""));
      detailRows.push_back(ftxui::hbox({ftxui::text(" "),
                                        formButton("Cancel", "Esc", UiButtonKind::Normal, "racks.new.cancel",
                                                   [selfForm] { selfForm->inputMode_ = InputMode::None; selfForm->dirty_ = true; }),
                                        ftxui::filler()}));
    } else {
      struct FormField { const char* label; const char* placeholder; };
      const FormField fields[4] = {{"Name", "required"}, {"Rows", "1-10"}, {"Columns", "1-10"}, {"Category", "optional"}};
      for (int index = 0; index < 4; ++index) {
        const bool focused = index == storageFormField_;
        const auto& value = storageFormValues_[index];
        const bool empty = value.empty();
        // The value sits in a filled box, so it reads as something to click and type in.
        const int boxWidth = max(6, formInner - 10);
        const string shown = empty && !focused ? string(fields[index].placeholder) : value + (focused ? "_" : "");
        auto box = fixedCell(" " + shown, static_cast<size_t>(boxWidth),
                             focused ? uiTitleColor() : empty ? uiMutedText() : uiPrimaryText()) |
                   ftxui::bgcolor(focused ? uiSelectionBg() : uiRaisedSurfaceBg());
        auto row = ftxui::hbox({fixedCell(fields[index].label, 9, focused ? uiTitleColor() : uiMutedColor()), move(box),
                                ftxui::filler()});
        detailRows.push_back(target(ftxui::hbox({ftxui::text(" "), move(row)}), "racks.new.field." + to_string(index),
                                    UiTargetKind::Field, [selfForm, index] {
                                      selfForm->storageFormField_ = index;
                                      selfForm->suggestionIndex_ = -1;
                                      selfForm->dirty_ = true;
                                    }));
        if (!focused) continue;
        if (index != 3) continue;
        const auto suggestions = organizerTypeSuggestions(suggestionIndex_ >= 0 ? suggestionBase_ : value);
        for (size_t hint = 0; hint < suggestions.size(); ++hint) {
          const bool current = static_cast<int>(hint) == suggestionIndex_;
          auto label = styledText("  " + ellipsize(suggestions[hint], static_cast<size_t>(formInner - 2)),
                                  current ? uiTitleColor() : uiMutedText());
          if (current) label = label | ftxui::inverted;
          const auto suggestion = suggestions[hint];
          detailRows.push_back(target(ftxui::hbox({ftxui::text(" "), label, ftxui::filler()}),
                                      "racks.new.suggestion." + to_string(hint), UiTargetKind::Button,
                                      [selfForm, suggestion] {
                                        selfForm->storageFormValues_[3] = suggestion;
                                        selfForm->dirty_ = true;
                                      }));
        }
      }
      detailRows.push_back(ftxui::text(""));
      detailRows.push_back(ftxui::hbox({ftxui::text(" "),
                                        formButton("Create", "Enter", UiButtonKind::Primary, "racks.new.create",
                                                   [selfForm] { selfForm->submitStorageForm(); }),
                                        ftxui::text(" "),
                                        formButton("Cancel", "Esc", UiButtonKind::Normal, "racks.new.cancel",
                                                   [selfForm] { selfForm->inputMode_ = InputMode::None; selfForm->dirty_ = true; }),
                                        ftxui::filler()}));
      // A small picture of the grid this makes, once both sizes are numbers in range.
      const auto digits = [](const string& text, int maximum) {
        return text.empty() || text.size() > 2 ? 0 : (stoi(text) >= 1 && stoi(text) <= maximum ? stoi(text) : 0);
      };
      const int previewRows = digits(storageFormValues_[1], kOrganizerMaxColumns);
      const int previewColumns = digits(storageFormValues_[2], kOrganizerMaxRows);
      detailRows.push_back(ftxui::text(""));
      if (previewRows > 0 && previewColumns > 0) {
        const string last = rackSlotLabel(previewColumns - 1, previewRows - 1);
        wide(to_string(previewRows * previewColumns) + " slots, A1 to " + last, uiSecondaryText());
        string header = "   ";
        for (int column = 0; column < previewColumns; ++column) header += string(1, static_cast<char>('A' + column)) + " ";
        detailRows.push_back(styledText(" " + header, uiLabelColor()));
        for (int row = 0; row < previewRows; ++row) {
          string line = (row + 1 < 10 ? " " : "") + to_string(row + 1) + " ";
          for (int column = 0; column < previewColumns; ++column) line += "o ";
          detailRows.push_back(styledText(" " + line, uiMutedColor()));
        }
      }
    }
  } else if (rack == nullptr) {
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

      // Same specifications as the Stock page, then the package.
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

    detailRows.push_back(ftxui::filler());
    // Two columns of wide buttons: Move and Datasheet, quantity, then the removing actions.
    const bool hasSlotItem = selectedSlotItem != nullptr;
    ftxui::Elements buttons;
    buttons.push_back(target(uiWideButton("Move", "v", hasSlotItem || !movingRackItemId_.empty() ? UiButtonKind::Primary : UiButtonKind::Normal),
                             "racks.move", UiTargetKind::Button, [selfDetail] { selfDetail->beginOrCompleteRackMove(); }));
    if (hasSlotItem) {
      buttons.push_back(target(uiWideButton("Datasheet", "d"), "racks.datasheet", UiTargetKind::Button,
                               [selfDetail] { selfDetail->openSelectedRackDatasheet(); }));
      buttons.push_back(target(uiWideButton("Add", "+"), "racks.part.plus", UiTargetKind::Button,
                               [selfDetail] { selfDetail->adjustSelectedRackItemQuantity(1); }));
      buttons.push_back(target(uiWideButton("Subtract", "-"), "racks.part.minus", UiTargetKind::Button,
                               [selfDetail] { selfDetail->adjustSelectedRackItemQuantity(-1); }));
      buttons.push_back(target(uiWideButton("Remove", "u", UiButtonKind::Danger), "racks.part.remove",
                               UiTargetKind::Button, [selfDetail] { selfDetail->unassignSelectedRackItem(); }));
    }
    if (rackOccupiedSlotCount(store_, *rack) == 0) {
      // Rack-level destructive action; arms the same modal as x.
      buttons.push_back(target(uiWideButton("Delete rack", "x", UiButtonKind::Danger), "racks.delete_rack",
                               UiTargetKind::Button, [selfDetail] { selfDetail->deleteSelectedRack(); }));
    }
    detailRows.push_back(uiButtonGrid(move(buttons), 2, detailWidth));
  }

  auto rackListBody = ftxui::vbox(move(rackRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                      ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex | ftxui::reflect(rackListPanelBounds_);
  auto* const app = const_cast<App*>(this);
  auto newStorageButton = ftxui::vbox({
      target(uiWideButton("New part", "n"), "racks.new-part", UiTargetKind::Button, [app] { app->beginNewPartInRack(); }),
      ftxui::text(""),
      target(uiWideButton("New storage", "c"), "racks.new", UiTargetKind::Button, [app] { app->beginStorageNew(); }),
  });
  newStorageButton = ftxui::hbox({ftxui::text(" "), newStorageButton | ftxui::flex, ftxui::text(" ")}) |
                     ftxui::bgcolor(uiSurfaceBg());
  auto rackPanel = ftxui::vbox({move(rackListBody), uiDivider(), move(newStorageButton)}) | ftxui::bgcolor(uiSurfaceBg()) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listWidth) | ftxui::flex;
  auto gridPanel = ftxui::vbox(move(gridRows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::reflect(rackGridPanelBounds_) |
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

  return uiDeleteConfirmation(move(page), screenWidth, "Delete rack", "rack",
                              rackToDelete == nullptr ? std::string("this rack") : rackToDelete->code,
                              rackDeleteConfirmationSecondsLeft());
}

void App::handleRackManagementKey(const KeyEvent& key) {
  // Delete confirmation is modal: Enter confirms once unlocked, anything else cancels.
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

  // Esc first cancels a pending move; leaving the page would cancel it silently.
  if (key.type == KeyType::Escape && !movingRackItemId_.empty()) {
    movingRackItemId_.clear();
    movingRackSource_.clear();
    setMessage("Move cancelled", 2);
    dirty_ = true;
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
  } else if (key.type == KeyType::PageUp) {
    moveRackSlot(0, -rackVisibleNumbers_);
  } else if (key.type == KeyType::PageDown) {
    moveRackSlot(0, rackVisibleNumbers_);
  } else if (key.type == KeyType::Home) {
    jumpToRackSlot(0, 0);
  } else if (key.type == KeyType::End) {
    jumpToRackSlot(numeric_limits<int>::max(), numeric_limits<int>::max());
  } else if (key.type == KeyType::Character) {
    // Other commands are registered actions dispatched first; only slot movement is here.
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
