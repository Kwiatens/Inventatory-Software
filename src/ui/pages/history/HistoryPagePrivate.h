#pragma once

#include "core/inventory/Inventory.h"

#include <array>
#include <ctime>
#include <string>
#include <vector>

namespace inventatory::history_page_detail {

enum class HistorySourceFilter {
  All,
  Manual,
  Corrective,
  DigiKey,
  Checkpoint,
  Import,
  Project,
};

struct HistoryRecord {
  std::string entityType;
  std::string entityId;
  std::string label;
  std::vector<InventoryFieldChange> changes;
};

struct HistoryCommitGroup {
  std::string label;
  std::vector<size_t> indices;
};

struct HistoryFieldDiff {
  std::string field;
  std::string previous;
  std::string next;
};

std::array<int, 2> historyPaneWidths(int contentWidth);
std::string historySourceFilterLabel(HistorySourceFilter filter);
std::string historyCommitType(const InventoryCommit& commit);
std::string historyCommitDisplayMessage(const InventoryCommit& commit);
std::string historyCommitImpactSummary(const InventoryCommit& commit);
std::vector<size_t> filteredHistoryIndices(const std::vector<InventoryCommit>& commits, const std::string& query,
                                           HistorySourceFilter filter);
std::vector<HistoryCommitGroup> groupedHistoryCommits(const std::vector<InventoryCommit>& commits,
                                                      const std::string& query, HistorySourceFilter filter,
                                                      std::time_t now);
std::vector<HistoryFieldDiff> historyFieldDiffs(const HistoryRecord& record);
std::vector<HistoryRecord> groupedHistoryRecords(const InventoryCommitDetail& detail);

}  // namespace inventatory::history_page_detail
