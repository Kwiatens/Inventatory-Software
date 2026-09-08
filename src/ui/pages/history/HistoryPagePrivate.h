#pragma once

#include "App.h"

#include <string>
#include <vector>

namespace inventatory::history_page_detail {

struct HistoryRecord {
  std::string entityType;
  std::string entityId;
  std::string label;
  std::vector<InventoryFieldChange> changes;
};

std::vector<HistoryRecord> groupedHistoryRecords(const InventoryCommitDetail& detail);

}  // namespace inventatory::history_page_detail
