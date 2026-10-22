// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Core inventory serialization helpers.

#include "core/inventory/InventoryInternals.h"

#include "core/storage/AtomicFile.h"

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
constexpr const char* kLegacyStructuredStoragePrefix = "v2:";
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

bool hasStructuredStoragePrefix(const string& value, size_t& prefixLength) {
  if (value.rfind(kStructuredStoragePrefix, 0) == 0) {
    prefixLength = strlen(kStructuredStoragePrefix);
    return true;
  }
  if (value.rfind(kLegacyStructuredStoragePrefix, 0) == 0) {
    prefixLength = strlen(kLegacyStructuredStoragePrefix);
    return true;
  }
  return false;
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

bool splitStrictEscapedStorageFields(const string& value, char delimiter, vector<string>& fields) {
  fields.clear();
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
  if (escaped) return false;
  fields.push_back(move(field));
  return true;
}

// As the strict split, but a trailing unpaired backslash, the one thing it refuses, is kept as text.
vector<string> splitEscapedStorageFields(const string& value, char delimiter) {
  vector<string> fields;
  if (!splitStrictEscapedStorageFields(value, delimiter, fields)) {
    splitStrictEscapedStorageFields(value.substr(0, value.size() - 1), delimiter, fields);
    fields.back().push_back('\\');
  }
  return fields;
}

bool deserializeTagsStrictImpl(const string& value, vector<string>& tags) {
  size_t prefixLength = 0;
  if (!hasStructuredStoragePrefix(value, prefixLength)) return false;
  vector<string> fields;
  if (!splitStrictEscapedStorageFields(value.substr(prefixLength), '|', fields)) return false;
  tags.clear();
  if (fields.size() == 1 && fields.front().empty()) return true;
  tags.reserve(fields.size());
  for (const auto& field : fields) tags.push_back(unescapeStorageField(field));
  return true;
}

bool deserializeParametersStrictImpl(const string& value, vector<Parameter>& parameters) {
  size_t prefixLength = 0;
  if (!hasStructuredStoragePrefix(value, prefixLength)) return false;
  vector<string> entries;
  if (!splitStrictEscapedStorageFields(value.substr(prefixLength), ';', entries)) return false;
  parameters.clear();
  if (entries.size() == 1 && entries.front().empty()) return true;
  parameters.reserve(entries.size());
  for (const auto& entry : entries) {
    const auto equalsPos = findUnescapedDelimiter(entry, '=');
    if (equalsPos == string::npos) return false;
    parameters.push_back({unescapeStorageField(entry.substr(0, equalsPos)),
                          unescapeStorageField(entry.substr(equalsPos + 1))});
  }
  return true;
}

vector<Parameter> parseLegacyParameters(const string& value) {
  vector<Parameter> parameters;
  for (const auto& entry : split(value, ';')) {
    const auto equalsPos = entry.find('=');
    if (equalsPos == string::npos) continue;
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
  size_t prefixLength = 0;
  if (!hasStructuredStoragePrefix(value, prefixLength)) return parseLegacyTags(value);

  vector<string> tags;
  for (const auto& field : splitEscapedStorageFields(value.substr(prefixLength), '|')) {
    if (field.empty() && value.size() == prefixLength) return {};
    tags.push_back(unescapeStorageField(field));
  }
  return tags;
}

bool deserializeTagsFromStorageStrict(const string& value, vector<string>& tags) {
  return deserializeTagsStrictImpl(value, tags);
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
  size_t prefixLength = 0;
  if (!hasStructuredStoragePrefix(value, prefixLength)) return parseLegacyParameters(value);

  vector<Parameter> parameters;
  for (const auto& entry : splitEscapedStorageFields(value.substr(prefixLength), ';')) {
    if (entry.empty() && value.size() == prefixLength) return {};
    const auto equalsPos = findUnescapedDelimiter(entry, '=');
    if (equalsPos == string::npos) {
      continue;
    }
    parameters.push_back({unescapeStorageField(entry.substr(0, equalsPos)),
                          unescapeStorageField(entry.substr(equalsPos + 1))});
  }
  return parameters;
}

bool deserializeParametersFromStorageStrict(const string& value, vector<Parameter>& parameters) {
  return deserializeParametersStrictImpl(value, parameters);
}

// Each source is one quoted record of the ten vendorMetadata fields. The list is a sequence
// of quoted strings, so it fits in an item record as one quoted field.
string serializeVendorSourcesForStorage(const vector<VendorProductMetadata>& sources) {
  ostringstream out;
  for (size_t index = 0; index < sources.size(); ++index) {
    const auto& source = sources[index];
    ostringstream record;
    record << quoted(source.provider) << '\t' << quoted(source.providerProductNumber) << '\t'
           << quoted(source.manufacturerPartNumber) << '\t' << quoted(source.categoryId) << '\t'
           << quoted(serializeTagsForStorage(source.categoryPath)) << '\t' << quoted(source.title) << '\t'
           << quoted(source.detailedDescription) << '\t' << quoted(serializeParametersForStorage(source.parameters))
           << '\t' << quoted(source.productUrl) << '\t' << quoted(source.locale);
    if (index > 0) out << ' ';
    out << quoted(record.str());
  }
  return out.str();
}

namespace {

// Reads one source record, leaving structured fields encoded for the strict and lenient readers.
bool readVendorSourceRecord(const string& record, VendorProductMetadata& source, string& categoryPath,
                            string& parameters) {
  istringstream input(record);
  if (!(input >> quoted(source.provider) >> quoted(source.providerProductNumber) >>
        quoted(source.manufacturerPartNumber) >> quoted(source.categoryId) >> quoted(categoryPath) >>
        quoted(source.title) >> quoted(source.detailedDescription) >> quoted(parameters) >>
        quoted(source.productUrl) >> quoted(source.locale))) {
    return false;
  }
  input >> ws;
  return input.eof();
}

}  // namespace

vector<VendorProductMetadata> deserializeVendorSourcesFromStorage(const string& value) {
  // Skips a damaged record so readable sources still load; the strict reader refuses it.
  istringstream list(value);
  vector<VendorProductMetadata> sources;
  for (list >> ws; !list.eof(); list >> ws) {
    string record;
    if (!(list >> quoted(record))) break;
    VendorProductMetadata source;
    string categoryPath;
    string parameters;
    if (!readVendorSourceRecord(record, source, categoryPath, parameters) || trim(source.provider).empty()) continue;
    source.categoryPath = deserializeTagsFromStorage(categoryPath);
    source.parameters = deserializeParametersFromStorage(parameters);
    sources.push_back(move(source));
  }
  return sources;
}

bool deserializeVendorSourcesFromStorageStrict(const string& value, vector<VendorProductMetadata>& sources) {
  istringstream list(value);
  vector<VendorProductMetadata> parsed;
  for (list >> ws; !list.eof(); list >> ws) {
    string record;
    if (!(list >> quoted(record))) return false;
    VendorProductMetadata source;
    string categoryPath;
    string parameters;
    if (!readVendorSourceRecord(record, source, categoryPath, parameters) || trim(source.provider).empty()) {
      return false;
    }
    if (!deserializeTagsFromStorageStrict(categoryPath, source.categoryPath) ||
        !deserializeParametersFromStorageStrict(parameters, source.parameters)) {
      return false;
    }
    parsed.push_back(move(source));
  }
  sources = move(parsed);
  return true;
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
       << quoted(item.vendorMetadata.productUrl) << '\t' << quoted(item.vendorMetadata.locale) << '\t'
       << quoted(serializeVendorSourcesForStorage(item.vendorSources));
  return out.str();
}

namespace {

bool deserializeItemImpl(const string& line, InventoryItem& item, bool strict) {
  istringstream input(line);
  InventoryItem parsed;
  string tags;
  string parameters;
  if (!(input >> quoted(parsed.id) >> quoted(parsed.partName) >> quoted(parsed.manufacturer) >> quoted(parsed.category) >>
        parsed.quantity >> parsed.reorderThreshold >> quoted(parsed.location) >> quoted(tags) >> quoted(parameters) >>
        quoted(parsed.notes) >> quoted(parsed.digikeyPartNumber) >> quoted(parsed.datasheetUrl) >>
        quoted(parsed.productUrl) >> quoted(parsed.syncStatus) >> quoted(parsed.sku) >> parsed.lastUpdated)) {
    return false;
  }

  if (strict) {
    if (!deserializeTagsFromStorageStrict(tags, parsed.tags) ||
        !deserializeParametersFromStorageStrict(parameters, parsed.parameters)) return false;
    string categoryPath;
    string vendorParameters;
    string rackMode;
    if (!(input >> quoted(parsed.inventatoryId) >> parsed.createdAt >> quoted(parsed.machineCode) >>
          quoted(parsed.rackId) >> quoted(parsed.rackSlot) >> quoted(rackMode) >> quoted(parsed.labelOverride) >>
          quoted(parsed.vendorMetadata.provider) >> quoted(parsed.vendorMetadata.providerProductNumber) >>
          quoted(parsed.vendorMetadata.manufacturerPartNumber) >> quoted(parsed.vendorMetadata.categoryId) >>
          quoted(categoryPath) >> quoted(parsed.vendorMetadata.title) >>
          quoted(parsed.vendorMetadata.detailedDescription) >> quoted(vendorParameters) >>
          quoted(parsed.vendorMetadata.productUrl) >> quoted(parsed.vendorMetadata.locale))) return false;
    const auto mode = toLower(trim(rackMode));
    if (mode == "manual") parsed.rackAssignment = RackAssignmentMode::Manual;
    else if (mode == "unassigned") parsed.rackAssignment = RackAssignmentMode::Unassigned;
    else if (mode == "automatic") parsed.rackAssignment = RackAssignmentMode::Automatic;
    else return false;
    if (!deserializeTagsFromStorageStrict(categoryPath, parsed.vendorMetadata.categoryPath) ||
        !deserializeParametersFromStorageStrict(vendorParameters, parsed.vendorMetadata.parameters)) return false;
    // Records written before vendor sources existed end here.
    string vendorSources;
    input >> ws;
    if (!input.eof() && !(input >> quoted(vendorSources))) return false;
    input >> ws;
    if (!input.eof()) return false;
    if (!deserializeVendorSourcesFromStorageStrict(vendorSources, parsed.vendorSources)) return false;
    item = move(parsed);
    return true;
  }

  parsed.tags = deserializeTagsFromStorage(tags);
  parsed.parameters = deserializeParametersFromStorage(parameters);
  parsed.machineCode.clear();

  if (parsed.lastUpdated == 0) {
    parsed.lastUpdated = nowEpoch();
  }
  parsed.createdAt = parsed.lastUpdated;
  if (input >> quoted(parsed.inventatoryId)) {
    if (!(input >> parsed.createdAt) || parsed.createdAt == 0) {
      parsed.createdAt = parsed.lastUpdated;
    }
    if (!(input >> quoted(parsed.machineCode))) {
      parsed.machineCode.clear();
    }
    string rackMode;
    if (input >> quoted(parsed.rackId) >> quoted(parsed.rackSlot) >> quoted(rackMode)) {
      parsed.rackAssignment = parseRackAssignmentMode(rackMode);
    }
    string categoryPath;
    string vendorParameters;
    if (input >> quoted(parsed.labelOverride) >> quoted(parsed.vendorMetadata.provider) >> quoted(parsed.vendorMetadata.providerProductNumber) >>
        quoted(parsed.vendorMetadata.manufacturerPartNumber) >> quoted(parsed.vendorMetadata.categoryId) >> quoted(categoryPath) >>
        quoted(parsed.vendorMetadata.title) >> quoted(parsed.vendorMetadata.detailedDescription) >> quoted(vendorParameters) >>
        quoted(parsed.vendorMetadata.productUrl) >> quoted(parsed.vendorMetadata.locale)) {
      parsed.vendorMetadata.categoryPath = deserializeTagsFromStorage(categoryPath);
      parsed.vendorMetadata.parameters = deserializeParametersFromStorage(vendorParameters);
      string vendorSources;
      input >> ws;
      if (!input.eof() && (input >> quoted(vendorSources))) {
        parsed.vendorSources = deserializeVendorSourcesFromStorage(vendorSources);
      }
    }
  }

  item = move(parsed);
  return true;
}

}  // namespace

bool deserializeItem(const string& line, InventoryItem& item) {
  return deserializeItemImpl(line, item, false);
}

bool deserializeItemStrict(const string& line, InventoryItem& item) {
  return deserializeItemImpl(line, item, true);
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
  // The whole line must be consumed; otherwise trailing bytes would be accepted and rewritten.
  input >> ws;
  return input.eof();
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

namespace {

// Keeps a record within the line limit loadActivities accepts. Quoting can double a message's size,
// so an oversized message is cut on a UTF-8 boundary and marked with "...".
string serializeActivityWithinLineLimit(const ActivityEntry& entry) {
  auto line = serializeActivity(entry);
  if (line.size() <= kMaxActivityLineBytes) return line;
  ActivityEntry shortened = entry;
  size_t keep = entry.message.size();
  while (line.size() > kMaxActivityLineBytes && keep > 0) {
    keep -= max<size_t>(1, keep / 8);
    size_t cut = keep;
    while (cut > 0 && (static_cast<unsigned char>(entry.message[cut]) & 0xC0U) == 0x80U) --cut;
    shortened.message = entry.message.substr(0, cut) + "...";
    line = serializeActivity(shortened);
  }
  return line;
}

}  // namespace

bool saveActivities(const filesystem::path& path, const vector<ActivityEntry>& activities) {
  ostringstream file;
  file << "# Inventatory activity log\n";
  for (const auto& entry : activities) {
    if (!validActivity(entry)) return false;
    file << serializeActivityWithinLineLimit(entry) << '\n';
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
