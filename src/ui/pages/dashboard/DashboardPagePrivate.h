// Inventatory - Private dashboard warning components.

#pragma once

#include "core/inventory/Inventory.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace inventatory::dashboard_detail {

enum class AttentionSeverity {
  Low = 2,
  Out = 3,
};

enum class AttentionGroup {
  Out,
  Low,
};

enum class DashboardRowSurface {
  Surface,
  Selection,
};

inline DashboardRowSurface dashboardRowSurface(bool selected, bool active) {
  return selected && active ? DashboardRowSurface::Selection : DashboardRowSurface::Surface;
}

inline std::string dashboardScannerIdleMessage(const std::string& state) {
  if (state == "UNPAIRED") return "Scanner R1 not paired";
  if (state == "WAITING") return "Scanner R1 waiting to connect";
  return "Scanner R1 offline";
}

struct AttentionRow {
  std::string issue;
  std::string partName;
  std::string location;
  std::string reason;
  int quantity = 0;
  AttentionSeverity severity = AttentionSeverity::Low;
  AttentionGroup group = AttentionGroup::Low;
};

struct DashboardSnapshot {
  std::size_t itemCount = 0;
  std::size_t totalQuantity = 0;
  std::size_t lowStockCount = 0;
  std::size_t outOfStockCount = 0;
  std::size_t dataErrorCount = 0;
  std::size_t missingMetadataCount = 0;
  std::vector<AttentionRow> attention;
};

ftxui::Element fixedCell(const std::string& value, int width, ftxui::Color color,
                         bool rightAlign = false, bool header = false);
ftxui::Element centeredCell(const std::string& value, int width, ftxui::Color color,
                            bool header = false);
DashboardSnapshot buildDashboardSnapshot(const std::vector<InventoryItem>& items, int lowStockThreshold);
ftxui::Element attentionPanel(
    const DashboardSnapshot& snapshot, int width, std::size_t selectedRow, bool active,
    const std::function<ftxui::Element(ftxui::Element, std::size_t)>& wrapRow);

}  // namespace inventatory::dashboard_detail
