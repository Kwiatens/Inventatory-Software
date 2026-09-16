// Inventatory - Private Rack page layout helpers.

#pragma once

#include <algorithm>

namespace inventatory {
namespace rack_page_detail {

// Round up to a shared whole-row height so the final physical rack row does
// not stop short of the space used by the preceding rows.
inline int equalRackSlotHeight(int slotRowsSpace, int rackGridColumns) {
  if (rackGridColumns <= 0) return 0;
  const int minimumRows = rackGridColumns * 3;
  const int availableRows = std::max(minimumRows, slotRowsSpace);
  return std::max(3, (availableRows + rackGridColumns - 1) / rackGridColumns);
}

}  // namespace rack_page_detail
}  // namespace inventatory
