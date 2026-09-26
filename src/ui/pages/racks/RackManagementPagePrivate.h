// Inventatory - Private Rack page layout helpers.

#pragma once

#include <algorithm>
#include <string>

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

// Floor division ensures that every slot column receives the exact same width
// and that all columns together do not exceed the available horizontal grid space.
inline int equalRackSlotWidth(int slotColumnsSpace, int rackGridColumns) {
  if (rackGridColumns <= 0) return 0;
  return std::max(7, slotColumnsSpace / rackGridColumns);
}

}  // namespace rack_page_detail
}  // namespace inventatory
