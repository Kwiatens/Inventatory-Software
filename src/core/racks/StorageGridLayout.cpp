// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - how a rack or organizer grid fits on screen.

#include "core/racks/StorageGridLayout.h"

#include <algorithm>

namespace inventatory {

using namespace std;

namespace {

// Rows of at least the minimum height that fit in `height`, with a divider line between rows.
int rowsThatFit(int height) {
  return max(0, (height + 1) / (kStorageCellMinHeight + 1));
}

// Where a window of `visible` cells starts so it holds `positions`, centred on them, inside `total`.
// Positions that do not fit with the first one are left out.
int windowStart(const vector<int>& positions, int visible, int total) {
  if (visible >= total || positions.empty()) return 0;
  const int anchor = positions.front();
  int low = anchor;
  int high = anchor;
  for (const int position : positions) {
    const int newLow = min(low, position);
    const int newHigh = max(high, position);
    if (newHigh - newLow + 1 <= visible) {
      low = newLow;
      high = newHigh;
    }
  }
  const int spare = visible - (high - low + 1);
  return clamp(low - spare / 2, 0, total - visible);
}

}  // namespace

StorageGridFit fitStorageGrid(const StorageGridRequest& request) {
  StorageGridFit fit;
  fit.letters = max(0, request.letters);
  fit.numbers = max(0, request.numbers);
  if (fit.letters == 0 || fit.numbers == 0) return fit;
  const int width = max(1, request.width);
  const int height = max(1, request.height);

  // Rows: all of them when each gets the minimum height, sharing the whole height so nothing is left empty.
  // Otherwise as many as fit between the two scroll bars.
  if (rowsThatFit(height) >= fit.numbers) {
    fit.numberCount = fit.numbers;
  } else {
    fit.scrollBars = true;
    fit.numberCount = clamp(rowsThatFit(height - 2), 1, fit.numbers);
  }
  const int rowSpace = height - (fit.scrollBars ? 2 : 0) - (fit.numberCount - 1);
  fit.cellHeight = max(kStorageCellMinHeight, rowSpace / fit.numberCount);
  fit.spareLines = max(0, rowSpace - fit.cellHeight * fit.numberCount);

  // Columns: every letter when each cell can be at least the minimum width, else a window of readable cells.
  const int allLettersWidth = (width - (fit.letters - 1)) / fit.letters;
  if (allLettersWidth >= kStorageCellMinWidth) {
    fit.letterCount = fit.letters;
    fit.cellWidth = allLettersWidth;
  } else {
    fit.letterCount = clamp((width + 1) / (kStorageCellScrollWidth + 1), 1, fit.letters);
    fit.cellWidth = max(kStorageCellMinWidth, (width - (fit.letterCount - 1)) / fit.letterCount);
  }

  vector<int> letterFocus;
  vector<int> numberFocus;
  for (const auto& [letter, number] : request.focus) {
    letterFocus.push_back(clamp(letter, 0, fit.letters - 1));
    numberFocus.push_back(clamp(number, 0, fit.numbers - 1));
  }
  fit.letterStart = windowStart(letterFocus, fit.letterCount, fit.letters);
  fit.numberStart = clamp(windowStart(numberFocus, fit.numberCount, fit.numbers) + request.scroll, 0,
                          fit.numbers - fit.numberCount);
  for (const auto& cell : request.focus) {
    const bool inside = cell.first >= fit.letterStart && cell.first < fit.letterStart + fit.letterCount &&
                        cell.second >= fit.numberStart && cell.second < fit.numberStart + fit.numberCount;
    if (!inside) fit.hiddenFocus.push_back(cell);
  }
  return fit;
}

}  // namespace inventatory
