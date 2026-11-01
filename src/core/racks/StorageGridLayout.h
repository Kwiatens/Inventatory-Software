// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - how a rack or organizer grid fits on screen.
//
// Slot letters run across the screen and slot numbers run down it. Cells keep the rack look and fill the
// height. When the rows do not fit, the grid scrolls, with a bar above and below that says what is off screen.

#pragma once

#include <utility>
#include <vector>

namespace inventatory {

struct StorageGridRequest {
  int letters = 0;  // slot letters, across
  int numbers = 0;  // slot numbers, down
  // Room for the cells and the one-cell gaps between them, without the row-number column and letter row.
  int width = 0;
  int height = 0;
  // Zero-based (letter, number) cells that must be on screen. The first one is the cursor or anchor.
  std::vector<std::pair<int, int>> focus;
  // Rows to skip past the first focus cell, for a map the user scrolls by hand.
  int scroll = 0;
};

struct StorageGridFit {
  int cellWidth = 0;
  int cellHeight = 0;
  int letterStart = 0;
  int letterCount = 0;
  int numberStart = 0;
  int numberCount = 0;
  int letters = 0;
  int numbers = 0;
  // A line above and below the rows that says how many rows are off screen. Shown while the rows scroll.
  bool scrollBars = false;
  // Lines left after every row got `cellHeight`; the first rows take one each, so the grid fills the height.
  int spareLines = 0;
  // Focus cells left outside the window when they are too far apart to show together.
  std::vector<std::pair<int, int>> hiddenFocus;

  bool hiddenLeft() const { return letterStart > 0; }
  bool hiddenRight() const { return letterStart + letterCount < letters; }
  int rowsAbove() const { return numberStart; }
  int rowsBelow() const { return numbers - numberStart - numberCount; }
  bool windowed() const { return hiddenLeft() || hiddenRight() || scrollBars; }
  // Lines the slot rows use: the rows, the dividers between them, and the scroll bars.
  int usedHeight() const { return numberCount * cellHeight + spareLines + (numberCount - 1) + (scrollBars ? 2 : 0); }
  // The height of one visible row.
  int rowHeight(int displayRow) const { return cellHeight + (displayRow < spareLines ? 1 : 0); }
  // Columns the cells use, gaps included.
  int usedWidth() const { return letterCount * cellWidth + (letterCount - 1); }
};

// The smallest cell: three lines (type, value, quantity) and seven columns, as on a 5x5 rack.
constexpr int kStorageCellMinHeight = 3;
constexpr int kStorageCellMinWidth = 7;
// The width a cell gets when the grid has to scroll sideways anyway.
constexpr int kStorageCellScrollWidth = 9;

StorageGridFit fitStorageGrid(const StorageGridRequest& request);

}  // namespace inventatory
