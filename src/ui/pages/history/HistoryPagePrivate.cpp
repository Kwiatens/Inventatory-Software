// Inventatory - Pure data helpers for the History page.

#include "HistoryPagePrivate.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace inventatory::history_page_detail {

using namespace std;

namespace {

tm localTime(time_t value) {
  tm result{};
#ifdef _WIN32
  localtime_s(&result, &value);
#else
  localtime_r(&value, &result);
#endif
  return result;
}

string dateKey(const tm& value) {
  ostringstream key;
  key << (value.tm_year + 1900) << '-';
  if (value.tm_mon + 1 < 10) key << '0';
  key << (value.tm_mon + 1) << '-';
  if (value.tm_mday < 10) key << '0';
  key << value.tm_mday;
  return key.str();
}

bool sameDate(const tm& lhs, const tm& rhs) {
  return lhs.tm_year == rhs.tm_year && lhs.tm_mon == rhs.tm_mon && lhs.tm_mday == rhs.tm_mday;
}

string previousDateKey(const tm& current) {
  tm yesterday = current;
  yesterday.tm_hour = 12;
  yesterday.tm_min = 0;
  yesterday.tm_sec = 0;
  yesterday.tm_mday -= 1;
  mktime(&yesterday);
  return dateKey(yesterday);
}

string explicitDateLabel(const tm& value) {
  static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  ostringstream label;
  label << kMonths[max(0, min(value.tm_mon, 11))] << ' ';
  if (value.tm_mday < 10) label << '0';
  label << value.tm_mday << ", " << (value.tm_year + 1900);
  return label.str();
}

string lowercase(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(tolower(character));
  });
  return value;
}

bool contains(const string& haystack, const string& needle) {
  return haystack.find(needle) != string::npos;
}

bool filterMatches(const InventoryCommit& commit, HistorySourceFilter filter) {
  switch (filter) {
    case HistorySourceFilter::All:
      return true;
    case HistorySourceFilter::Manual:
      return historyCommitType(commit) == "MANUAL";
    case HistorySourceFilter::Corrective:
      return historyCommitType(commit) == "CORRECTIVE";
    case HistorySourceFilter::DigiKey:
      return historyCommitType(commit) == "DIGIKEY";
    case HistorySourceFilter::Checkpoint:
      return historyCommitType(commit) == "CHECKPOINT";
    case HistorySourceFilter::Import:
      return historyCommitType(commit) == "IMPORT";
    case HistorySourceFilter::Project:
      return historyCommitType(commit) == "PROJECT";
  }
  return false;
}

vector<pair<string, string>> parameterPairs(const string& encoded) {
  vector<pair<string, string>> pairs;
  if (encoded.empty()) return pairs;

  size_t start = 0;
  while (start <= encoded.size()) {
    const auto end = encoded.find(';', start);
    const auto segment = trim(encoded.substr(start, end == string::npos ? string::npos : end - start));
    if (!segment.empty()) {
      const auto separator = segment.find('=');
      if (separator == string::npos) return {};
      pairs.emplace_back(trim(segment.substr(0, separator)), trim(segment.substr(separator + 1)));
    }
    if (end == string::npos) break;
    start = end + 1;
  }
  return pairs;
}

struct ParameterDiff {
  string name;
  string before;
  string after;
};

vector<ParameterDiff> parameterDiffs(const string& before, const string& after) {
  const auto beforePairs = parameterPairs(before);
  const auto afterPairs = parameterPairs(after);
  if (beforePairs.empty() && !before.empty()) return {};
  if (afterPairs.empty() && !after.empty()) return {};

  unordered_map<string, string> beforeValues;
  unordered_map<string, string> afterValues;
  vector<string> order;
  beforeValues.reserve(beforePairs.size());
  afterValues.reserve(afterPairs.size());

  for (const auto& pair : beforePairs) {
    beforeValues[pair.first] = pair.second;
    order.push_back(pair.first);
  }
  for (const auto& pair : afterPairs) {
    afterValues[pair.first] = pair.second;
    if (find(order.begin(), order.end(), pair.first) == order.end()) order.push_back(pair.first);
  }

  vector<ParameterDiff> diffs;
  for (const auto& name : order) {
    const auto beforeValue = beforeValues.find(name);
    const auto afterValue = afterValues.find(name);
    const auto oldValue = beforeValue == beforeValues.end() ? "(absent)" : beforeValue->second;
    const auto newValue = afterValue == afterValues.end() ? "(absent)" : afterValue->second;
    if (oldValue != newValue) diffs.push_back({name, oldValue, newValue});
  }
  return diffs;
}

string displayValue(const string& field, const string& value) {
  if (value.empty()) return "(empty)";
  if (field == "last updated" || field == "created at") {
    const bool numeric = all_of(value.begin(), value.end(), [](unsigned char character) {
      return isdigit(character) != 0;
    });
    if (numeric) {
      try {
        const auto timestamp = stoll(value);
        if (timestamp > 0) return nowTimestampString(static_cast<time_t>(timestamp));
      } catch (...) {
        // Keep unexpected persisted values readable and lossless.
      }
    }
  }
  return value;
}

string fieldLabel(const string& value) {
  auto label = prettyLabel(value);
  if (!label.empty()) {
    label[0] = static_cast<char>(toupper(static_cast<unsigned char>(label[0])));
  }
  return label;
}

}  // namespace

array<int, 2> historyPaneWidths(int contentWidth) {
  const int safeContentWidth = max(2, contentWidth);
  const int availablePanelWidth = safeContentWidth - 1;
  const int listWidth = availablePanelWidth / 2;
  return {listWidth, availablePanelWidth - listWidth};
}

string historySourceFilterLabel(HistorySourceFilter filter) {
  switch (filter) {
    case HistorySourceFilter::All:
      return "All";
    case HistorySourceFilter::Manual:
      return "Manual";
    case HistorySourceFilter::Corrective:
      return "Corrective";
    case HistorySourceFilter::DigiKey:
      return "DigiKey";
    case HistorySourceFilter::Checkpoint:
      return "Checkpoint";
    case HistorySourceFilter::Import:
      return "Import";
    case HistorySourceFilter::Project:
      return "Project";
  }
  return "All";
}

string historyCommitType(const InventoryCommit& commit) {
  if (commit.checkpoint) return "CHECKPOINT";
  if (commit.corrective) return "CORRECTIVE";
  const auto source = lowercase(trim(commit.source));
  return source.empty() || source == "manual" ? "MANUAL" : toUpper(source);
}

string historyCommitImpactSummary(const InventoryCommit& commit) {
  const auto parts = commit.changedItemCount;
  const auto racks = commit.changedRackCount;
  if (parts == 0 && racks == 0) return "No inventory changes";

  const auto countLabel = [](size_t count, const char* singular, const char* plural) {
    return to_string(count) + " " + (count == 1 ? singular : plural);
  };
  if (parts == 0) return countLabel(racks, "rack", "racks") + " changed";
  if (racks == 0) return countLabel(parts, "part", "parts") + " changed";
  return countLabel(parts, "part", "parts") + " · " + countLabel(racks, "rack", "racks") + " changed";
}

vector<size_t> filteredHistoryIndices(const vector<InventoryCommit>& commits, const string& query,
                                      HistorySourceFilter filter) {
  const auto normalizedQuery = lowercase(trim(query));
  vector<size_t> indices;
  indices.reserve(commits.size());
  for (size_t index = 0; index < commits.size(); ++index) {
    const auto& commit = commits[index];
    if (!filterMatches(commit, filter)) continue;
    if (!normalizedQuery.empty()) {
      const auto searchable = lowercase(to_string(commit.sequence) + " " + commit.id + " " +
                                        historyCommitType(commit) + " " + commit.source + " " + commit.message + " " +
                                        commit.reference);
      if (!contains(searchable, normalizedQuery)) continue;
    }
    indices.push_back(index);
  }
  return indices;
}

vector<HistoryCommitGroup> groupedHistoryCommits(const vector<InventoryCommit>& commits, const string& query,
                                                 HistorySourceFilter filter, time_t now) {
  const auto indices = filteredHistoryIndices(commits, query, filter);
  const auto current = localTime(now);
  const auto todayKey = dateKey(current);
  const auto yesterdayKey = previousDateKey(current);
  vector<HistoryCommitGroup> groups;
  for (const auto index : indices) {
    const auto commitDate = localTime(commits[index].timestamp);
    const auto key = dateKey(commitDate);
    string label;
    if (key == todayKey) {
      label = "Today";
    } else if (key == yesterdayKey) {
      label = "Yesterday";
    } else {
      label = explicitDateLabel(commitDate);
    }
    if (groups.empty() || groups.back().label != label) groups.push_back({label, {}});
    groups.back().indices.push_back(index);
  }
  return groups;
}

vector<HistoryFieldDiff> historyFieldDiffs(const HistoryRecord& record) {
  vector<HistoryFieldDiff> diffs;
  for (const auto& change : record.changes) {
    if (change.field == "record") continue;
    const auto parameterChanges = (change.field == "parameters" || change.field == "vendor parameters")
                                      ? parameterDiffs(change.before, change.after)
                                      : vector<ParameterDiff>();
    if (!parameterChanges.empty()) {
      for (const auto& parameter : parameterChanges) {
        diffs.push_back({fieldLabel(change.field) + " / " + prettyLabel(parameter.name), parameter.before,
                         parameter.after});
      }
      continue;
    }
    diffs.push_back({fieldLabel(change.field), displayValue(change.field, change.before),
                     displayValue(change.field, change.after)});
  }
  return diffs;
}

vector<HistoryRecord> groupedHistoryRecords(const InventoryCommitDetail& detail) {
  vector<HistoryRecord> records;
  unordered_map<string, size_t> indices;
  indices.reserve(detail.changes.size());

  for (const auto& change : detail.changes) {
    const auto key = change.entityType + '\x1f' + change.entityId;
    const auto existing = indices.find(key);
    size_t index = 0;
    if (existing == indices.end()) {
      index = records.size();
      indices.emplace(key, index);
      records.push_back({change.entityType, change.entityId, change.label, {}});
    } else {
      index = existing->second;
    }
    records[index].changes.push_back(change);
  }
  return records;
}

}  // namespace inventatory::history_page_detail
