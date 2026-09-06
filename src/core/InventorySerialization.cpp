// Inventatory - Hardware Inventory Management System
// Core inventory serialization helpers.

#include "core/InventoryInternals.h"

#include "core/AtomicFile.h"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <utility>

namespace inventatory {

using namespace std;

namespace {

constexpr const char* kStructuredStoragePrefix = "v1:";
constexpr size_t kMaxActivityFileBytes = 16U * 1024U * 1024U;
constexpr size_t kMaxActivityLineBytes = 64U * 1024U;
constexpr size_t kMaxActivityKindBytes = 256U;
constexpr size_t kMaxActivityMessageBytes = 64U * 1024U;

bool validActivityText(const string& value, size_t maximum) {
  return value.size() <= maximum &&
         all_of(value.begin(), value.end(), [](unsigned char character) {
           return character != 0 && character != '\r' && character != '\n';
         });
}

bool validActivity(const ActivityEntry& entry) {
  return validActivityText(entry.kind, kMaxActivityKindBytes) &&
         validActivityText(entry.message, kMaxActivityMessageBytes);
}

bool activityFileWithinLimit(const filesystem::path& path) {
  error_code error;
  const auto size = filesystem::file_size(path, error);
  return !error && size <= kMaxActivityFileBytes;
}

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
    return {};
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
    return {};
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
      << '\t' << quoted(item.notes) << '\t' << quoted(item.digikeyPartNumber) << '\t' << quoted(item.datasheetUrl)
      << '\t' << quoted(item.productUrl) << '\t' << quoted(item.syncStatus) << '\t' << quoted(item.sku) << '\t'
       << item.lastUpdated << '\t' << quoted(item.inventatoryId) << '\t' << item.createdAt << '\t'
       << quoted(item.machineCode) << '\t' << quoted(item.rackId) << '\t' << quoted(item.rackSlot) << '\t'
       << quoted(rackAssignmentModeName(item.rackAssignment)) << '\t' << quoted(item.labelOverride) << '\t'
       << quoted(item.vendorMetadata.provider) << '\t' << quoted(item.vendorMetadata.providerProductNumber) << '\t'
       << quoted(item.vendorMetadata.manufacturerPartNumber) << '\t' << quoted(item.vendorMetadata.categoryId) << '\t'
       << quoted(serializeTagsForStorage(item.vendorMetadata.categoryPath)) << '\t'
       << quoted(item.vendorMetadata.title) << '\t' << quoted(item.vendorMetadata.detailedDescription) << '\t'
       << quoted(serializeParametersForStorage(item.vendorMetadata.parameters)) << '\t'
       << quoted(item.vendorMetadata.productUrl) << '\t' << quoted(item.vendorMetadata.locale);
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
    string categoryPath;
    string vendorParameters;
    if (input >> quoted(item.labelOverride) >> quoted(item.vendorMetadata.provider) >> quoted(item.vendorMetadata.providerProductNumber) >>
        quoted(item.vendorMetadata.manufacturerPartNumber) >> quoted(item.vendorMetadata.categoryId) >> quoted(categoryPath) >>
        quoted(item.vendorMetadata.title) >> quoted(item.vendorMetadata.detailedDescription) >> quoted(vendorParameters) >>
        quoted(item.vendorMetadata.productUrl) >> quoted(item.vendorMetadata.locale)) {
      item.vendorMetadata.categoryPath = deserializeTagsFromStorage(categoryPath);
      item.vendorMetadata.parameters = deserializeParametersFromStorage(vendorParameters);
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
  if (!activityFileWithinLimit(path)) return false;
  ifstream file(path);
  if (!file) {
    return false;
  }

  vector<ActivityEntry> loaded;
  string line;
  while (getline(file, line)) {
    if (line.size() > kMaxActivityLineBytes) return false;
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    ActivityEntry entry;
    if (!deserializeActivity(line, entry) || !validActivity(entry)) return false;
    loaded.push_back(move(entry));
  }

  if (file.bad()) return false;
  activities = move(loaded);
  return true;
}

bool saveActivities(const filesystem::path& path, const vector<ActivityEntry>& activities) {
  ostringstream file;
  file << "# Inventatory activity log\n";
  for (const auto& entry : activities) {
    if (!validActivity(entry)) return false;
    file << serializeActivity(entry) << '\n';
    if (!file || static_cast<size_t>(file.tellp()) > kMaxActivityFileBytes) return false;
  }
  const auto text = file.str();
  if (text.size() > kMaxActivityFileBytes) return false;
  string error;
  return writeFileAtomically(path, text, &error);
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
