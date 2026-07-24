// Inventatory - Hardware Inventory Management System
// Core inventory serialization helpers.

#include "core/InventoryInternals.h"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace inventatory {

using namespace std;

namespace {

constexpr const char* kStructuredStoragePrefix = "v2:";

string escapeStorageField(const string& value, const string& delimiters) {
  string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\\' || delimiters.find(ch) != string::npos) {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return escaped;
}

vector<string> splitEscapedStorageFields(const string& value, char delimiter) {
  vector<string> fields;
  string field;
  bool escaped = false;
  for (const char ch : value) {
    if (escaped) {
      field.push_back('\\');
      field.push_back(ch);
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == delimiter) {
      fields.push_back(move(field));
      field.clear();
    } else {
      field.push_back(ch);
    }
  }
  if (escaped) {
    field.push_back('\\');
  }
  fields.push_back(move(field));
  return fields;
}

string unescapeStorageField(const string& value) {
  string unescaped;
  unescaped.reserve(value.size());
  bool escaped = false;
  for (const char ch : value) {
    if (escaped) {
      unescaped.push_back(ch);
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else {
      unescaped.push_back(ch);
    }
  }
  if (escaped) {
    unescaped.push_back('\\');
  }
  return unescaped;
}

size_t findUnescapedDelimiter(const string& value, char delimiter) {
  bool escaped = false;
  for (size_t index = 0; index < value.size(); ++index) {
    if (escaped) {
      escaped = false;
    } else if (value[index] == '\\') {
      escaped = true;
    } else if (value[index] == delimiter) {
      return index;
    }
  }
  return string::npos;
}

vector<Parameter> parseLegacyParameters(const string& value) {
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

vector<string> parseLegacyTags(const string& value) {
  return split(value, '|');
}

}  // namespace

string serializeTagsForStorage(const vector<string>& tags) {
  vector<string> encoded;
  encoded.reserve(tags.size());
  for (const auto& tag : tags) {
    encoded.push_back(escapeStorageField(tag, "|"));
  }
  return string(kStructuredStoragePrefix) + join(encoded, '|');
}

vector<string> deserializeTagsFromStorage(const string& value) {
  if (value.rfind(kStructuredStoragePrefix, 0) != 0) {
    return parseLegacyTags(value);
  }

  vector<string> tags;
  for (const auto& field : splitEscapedStorageFields(value.substr(strlen(kStructuredStoragePrefix)), '|')) {
    tags.push_back(unescapeStorageField(field));
  }
  return tags;
}

string serializeParametersForStorage(const vector<Parameter>& parameters) {
  vector<string> encoded;
  encoded.reserve(parameters.size());
  for (const auto& parameter : parameters) {
    encoded.push_back(escapeStorageField(parameter.name, ";=") + "=" +
                      escapeStorageField(parameter.value, ";="));
  }
  return string(kStructuredStoragePrefix) + join(encoded, ';');
}

vector<Parameter> deserializeParametersFromStorage(const string& value) {
  if (value.rfind(kStructuredStoragePrefix, 0) != 0) {
    return parseLegacyParameters(value);
  }

  vector<Parameter> parameters;
  for (const auto& entry : splitEscapedStorageFields(value.substr(strlen(kStructuredStoragePrefix)), ';')) {
    const auto equalsPos = findUnescapedDelimiter(entry, '=');
    if (equalsPos == string::npos) {
      continue;
    }
    parameters.push_back({unescapeStorageField(entry.substr(0, equalsPos)),
                          unescapeStorageField(entry.substr(equalsPos + 1))});
  }
  return parameters;
}

string serializeItem(const InventoryItem& item) {
  ostringstream out;
  out << quoted(item.id) << '\t' << quoted(item.partName) << '\t' << quoted(item.manufacturer) << '\t'
       << quoted(item.category) << '\t' << item.quantity << '\t' << item.reorderThreshold << '\t'
       << quoted(item.location) << '\t' << quoted(serializeTagsForStorage(item.tags)) << '\t'
       << quoted(serializeParametersForStorage(item.parameters))
      << '\t' << quoted(item.notes) << '\t' << quoted(item.manufacturerPartNumber) << '\t' << quoted(item.datasheetUrl)
      << '\t' << quoted(item.catalogueStatus) << '\t' << quoted(item.cataloguePartId) << '\t'
      << quoted(item.catalogueSnapshot) << '\t' << quoted(item.catalogueName) << '\t'
      << quoted(item.cataloguePurposeLabel) << '\t' << quoted(item.cataloguePrintLabel) << '\t'
      << quoted(item.catalogueCategory) << '\t' << quoted(item.catalogueDatasheetUrl) << '\t'
      << item.lastUpdated << '\t' << quoted(item.inventatoryId) << '\t' << item.createdAt << '\t'
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
        quoted(item.notes) >> quoted(item.manufacturerPartNumber) >> quoted(item.datasheetUrl) >>
        quoted(item.catalogueStatus) >> quoted(item.cataloguePartId) >> quoted(item.catalogueSnapshot) >>
        quoted(item.catalogueName) >> quoted(item.cataloguePurposeLabel) >> quoted(item.cataloguePrintLabel) >>
        quoted(item.catalogueCategory) >> quoted(item.catalogueDatasheetUrl) >> item.lastUpdated)) {
    return false;
  }

  item.tags = deserializeTagsFromStorage(tags);
  item.parameters = deserializeParametersFromStorage(parameters);
  item.machineCode.clear();

  if (item.lastUpdated == 0) {
    item.lastUpdated = nowEpoch();
  }
  item.createdAt = item.lastUpdated;
  if (input >> quoted(item.inventatoryId)) {
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

  file << "# Inventatory activity log\n";
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

}  // namespace inventatory
