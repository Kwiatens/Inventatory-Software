// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "core/inventory/Inventory.h"
#include "core/racks/StorageGridLayout.h"

namespace inventatory {
namespace rack_page_detail {

inline std::string shortComponentType(const std::string& componentType) {
  if (componentType == "Integrated Circuits" || componentType == "integrated circuits" ||
      componentType == "Integrated circuits" || componentType == "Integrated Circuits (ICs)") {
    return "ICs";
  }
  return componentType;
}

// Floor division keeps every row within the space, so the grid cannot clip the bottom row.
inline int equalRackSlotHeight(int slotRowsSpace, int rackGridRows) {
  if (rackGridRows <= 0) return 0;
  return std::max(3, slotRowsSpace / rackGridRows);
}

// Rows left over after each slot row gets `slotHeight`. Never negative.
inline int rackSlotRemainderRows(int slotRowsSpace, int rackGridRows, int slotHeight) {
  if (rackGridRows <= 0) return 0;
  return std::max(0, slotRowsSpace - slotHeight * rackGridRows);
}

// Floor division keeps every column the same width and the total within the grid.
inline int equalRackSlotWidth(int slotColumnsSpace, int rackGridColumns) {
  if (rackGridColumns <= 0) return 0;
  return std::max(7, slotColumnsSpace / rackGridColumns);
}

// One slot's contents: type, bold value and specification, with the quantity at the bottom.
// Shared by the Racks and Projects grids.
ftxui::Element rackCellBody(const InventoryItem& item, bool selected, int lowStockThreshold, int cellWidth,
                            int rowHeight);

// The letter above a visible column, with an arrow when letters are off screen on that side: "◂ H", "L ▸".
std::string storageLetterHeader(const StorageGridFit& fit, int displayColumn);
// The scroll bar text above or below the rows: "▲ 2 more rows above", or empty when nothing is there.
std::string storageScrollText(const StorageGridFit& fit, bool above);

}  // namespace rack_page_detail
}  // namespace inventatory
