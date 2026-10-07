// Inventatory - Private Rack page layout helpers.

#pragma once

#include <algorithm>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "core/inventory/Inventory.h"

namespace inventatory {
namespace rack_page_detail {

inline std::string shortComponentType(const std::string& componentType) {
  if (componentType == "Integrated Circuits" || componentType == "integrated circuits" ||
      componentType == "Integrated circuits" || componentType == "Integrated Circuits (ICs)") {
    return "ICs";
  }
  return componentType;
}

// Floor division ensures that multiplying by the number of slot rows will never
// exceed the available vertical space, preventing the grid from overflowing
// the terminal and clipping the bottom row or quantity indicators.
// Every rack slot row receives the exact same height.
inline int equalRackSlotHeight(int slotRowsSpace, int rackGridRows) {
  if (rackGridRows <= 0) return 0;
  return std::max(3, slotRowsSpace / rackGridRows);
}

// Rows left over after every slot row receives `slotHeight`. Never negative: when the space is too small for the
// minimum slot height there is nothing to hand out.
inline int rackSlotRemainderRows(int slotRowsSpace, int rackGridRows, int slotHeight) {
  if (rackGridRows <= 0) return 0;
  return std::max(0, slotRowsSpace - slotHeight * rackGridRows);
}

// Floor division ensures that every slot column receives the exact same width
// and that all columns together do not exceed the available horizontal grid space.
inline int equalRackSlotWidth(int slotColumnsSpace, int rackGridColumns) {
  if (rackGridColumns <= 0) return 0;
  return std::max(7, slotColumnsSpace / rackGridColumns);
}

// One slot's contents: part name split into dim type, bold value and dim specification, with a bold quantity pinned to
// the bottom edge. Shared by the Racks page and the Projects build walkthrough so both grids read the same.
ftxui::Element rackCellBody(const InventoryItem& item, bool selected, int lowStockThreshold, int cellWidth,
                            int rowHeight);

}  // namespace rack_page_detail
}  // namespace inventatory
