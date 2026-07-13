// HIMS - Hardware Inventory Management System
// Core inventory serialization helpers.

#include "core/InventoryInternals.h"

#include <cstddef>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace hims {

using namespace std;

namespace {

vector<Parameter> parseParametersFromDb(const string& value) {
  vector<Parameter> parameters;
  for (const auto& entry : split(value, ';')) {
    const auto equalsPos = entry.find('=');
    if (equalsPos == string::npos) {
      continue;
    }
    parameters.push_back({trim(entry.substr(0, equalsPos)), trim(entry.substr(equalsPos + 1))});
  }
  return parameters;
}

vector<string> parseTagsFromDb(const string& value) {
  return split(value, '|');
}

}  // namespace

string serializeItem(const InventoryItem& item) {
  ostringstream out;
  vector<string> parameterEntries;
  parameterEntries.reserve(item.parameters.size());
  for (const auto& parameter : item.parameters) {
    parameterEntries.push_back(parameter.name + "=" + parameter.value);
  }

  out << quoted(item.id) << '\t' << quoted(item.partName) << '\t' << quoted(item.manufacturer) << '\t'
      << quoted(item.category) << '\t' << item.quantity << '\t' << item.reorderThreshold << '\t'
      << quoted(item.location) << '\t' << quoted(join(item.tags, '|')) << '\t' << quoted(join(parameterEntries, ';'))
      << '\t' << quoted(item.notes) << '\t' << quoted(item.digikeyPartNumber) << '\t' << quoted(item.datasheetUrl)
      << '\t' << quoted(item.productUrl) << '\t' << quoted(item.syncStatus) << '\t' << quoted(item.sku) << '\t'
      << item.lastUpdated << '\t' << quoted(item.himsId) << '\t' << item.createdAt << '\t'
      << quoted(item.machineCode) << '\t' << quoted(item.rackId) << '\t' << quoted(item.rackSlot) << '\t'
      << quoted(rackAssignmentModeName(item.rackAssignment));
  return out.str();
}

bool deserializeItem(const string& line, InventoryItem& item) {
  istringstream input(line);
  string tags;
  string parameters;
  if (!(input >> quoted(item.id) >> quoted(item.partName) >> quoted(item.manufacturer) >> quoted(item.category) >>
        item.quantity >> item.reorderThreshold >> quoted(item.location) >> quoted(tags) >> quoted(parameters) >>
        quoted(item.notes) >> quoted(item.digikeyPartNumber) >> quoted(item.datasheetUrl) >>
        quoted(item.productUrl) >> quoted(item.syncStatus) >> quoted(item.sku) >> item.lastUpdated)) {
    return false;
  }

  item.tags = parseTagsFromDb(tags);
  item.parameters = parseParametersFromDb(parameters);
  item.machineCode.clear();

  if (item.lastUpdated == 0) {
    item.lastUpdated = nowEpoch();
  }
  item.createdAt = item.lastUpdated;
  if (input >> quoted(item.himsId)) {
    if (!(input >> item.createdAt) || item.createdAt == 0) {
      item.createdAt = item.lastUpdated;
    }
    if (!(input >> quoted(item.machineCode))) {
      item.machineCode.clear();
    }
    string rackMode;
    if (input >> quoted(item.rackId) >> quoted(item.rackSlot) >> quoted(rackMode)) {
      item.rackAssignment = parseRackAssignmentMode(rackMode);
    }
  }

  return true;
}

string serializeActivity(const ActivityEntry& entry) {
  ostringstream out;
  out << entry.timestamp << '\t' << quoted(entry.kind) << '\t' << quoted(entry.message);
  return out.str();
}

bool deserializeActivity(const string& line, ActivityEntry& entry) {
  istringstream input(line);
  if (!(input >> entry.timestamp >> quoted(entry.kind) >> quoted(entry.message))) {
    return false;
  }
  return true;
}

bool loadActivities(const filesystem::path& path, vector<ActivityEntry>& activities) {
  activities.clear();

  ifstream file(path);
  if (!file) {
    return false;
  }

  string line;
  while (getline(file, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    ActivityEntry entry;
    if (deserializeActivity(line, entry)) {
      activities.push_back(move(entry));
    }
  }

  return true;
}

bool saveActivities(const filesystem::path& path, const vector<ActivityEntry>& activities) {
  filesystem::create_directories(path.parent_path());

  ofstream file(path, ios::trunc);
  if (!file) {
    return false;
  }

  file << "# HIMS activity log\n";
  for (const auto& entry : activities) {
    file << serializeActivity(entry) << '\n';
  }
  return true;
}

void appendActivity(vector<ActivityEntry>& activities, const ActivityEntry& entry, size_t maxEntries) {
  activities.push_back(entry);
  if (activities.size() > maxEntries) {
    activities.erase(activities.begin(), activities.begin() + static_cast<ptrdiff_t>(activities.size() - maxEntries));
  }
}

ActivityEntry makeActivity(string kind, string message) {
  return {nowEpoch(), move(kind), move(message)};
}

}  // namespace hims
