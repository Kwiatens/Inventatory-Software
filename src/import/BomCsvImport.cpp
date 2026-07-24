// Inventatory - Hardware Inventory Management System
// Supplier-neutral BOM and order CSV parsing.

#include "import/BomCsvImport.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

namespace inventatory {

using namespace std;

namespace {

struct Columns {
  int mpn = -1;
  int quantity = -1;
  int manufacturer = -1;
  int name = -1;
  int datasheet = -1;
  int location = -1;
  int notes = -1;
  int tags = -1;
};

string normalizedHeader(const string& value) {
  string result;
  for (unsigned char ch : toLower(trim(value))) if (isalnum(ch)) result.push_back(static_cast<char>(ch));
  return result;
}

vector<vector<string>> parseCsv(const string& text, string& error) {
  vector<vector<string>> rows;
  vector<string> row;
  string field;
  bool quoted = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (quoted) {
      if (ch == '"' && i + 1 < text.size() && text[i + 1] == '"') { field.push_back('"'); ++i; }
      else if (ch == '"') quoted = false;
      else field.push_back(ch);
    } else if (ch == '"') quoted = true;
    else if (ch == ',') { row.push_back(trim(field)); field.clear(); }
    else if (ch == '\n') {
      row.push_back(trim(field)); field.clear();
      if (!(row.size() == 1 && row.front().empty())) rows.push_back(move(row));
      row.clear();
    } else if (ch != '\r') field.push_back(ch);
  }
  if (quoted) { error = "CSV has an unterminated quoted field"; return {}; }
  row.push_back(trim(field));
  if (!(row.size() == 1 && row.front().empty())) rows.push_back(move(row));
  return rows;
}

int findColumn(const vector<string>& headers, const string& explicitName, initializer_list<const char*> aliases) {
  vector<string> candidates;
  if (!trim(explicitName).empty()) candidates.push_back(normalizedHeader(explicitName));
  for (const auto* alias : aliases) candidates.push_back(normalizedHeader(alias));
  for (size_t i = 0; i < headers.size(); ++i) {
    if (find(candidates.begin(), candidates.end(), normalizedHeader(headers[i])) != candidates.end()) return static_cast<int>(i);
  }
  return -1;
}

Columns mapColumns(const vector<string>& headers, const CsvColumnMapping& mapping) {
  Columns c;
  c.mpn = findColumn(headers, mapping.manufacturerPartNumber,
                     {"Manufacturer Part Number", "Manufacturer Part #", "Mfr Part Number", "MPN", "Part Number"});
  c.quantity = findColumn(headers, mapping.quantity, {"Quantity", "Qty", "Count"});
  c.manufacturer = findColumn(headers, mapping.manufacturer, {"Manufacturer", "Mfr", "Maker"});
  c.name = findColumn(headers, mapping.partName, {"Part Name", "Description", "Name"});
  c.datasheet = findColumn(headers, mapping.datasheetUrl, {"Datasheet URL", "Datasheet", "Data Sheet"});
  c.location = findColumn(headers, mapping.location, {"Location", "Bin", "Storage Location"});
  c.notes = findColumn(headers, mapping.notes, {"Notes", "Comment", "Comments"});
  c.tags = findColumn(headers, mapping.tags, {"Tags", "Labels"});
  return c;
}

string cell(const vector<string>& row, int index) {
  return index < 0 || static_cast<size_t>(index) >= row.size() ? string() : trim(row[static_cast<size_t>(index)]);
}

optional<int> positiveInt(const string& value) {
  const auto input = trim(value);
  if (input.empty()) return nullopt;
  int result = 0;
  for (unsigned char ch : input) {
    if (!isdigit(ch) || result > (numeric_limits<int>::max() - (ch - '0')) / 10) return nullopt;
    result = result * 10 + (ch - '0');
  }
  return result > 0 ? optional<int>(result) : nullopt;
}

string importedId(const string& mpn) {
  string result;
  for (unsigned char ch : mpn) {
    if (isalnum(ch)) result.push_back(static_cast<char>(tolower(ch)));
    else if (!result.empty() && result.back() != '-') result.push_back('-');
  }
  while (!result.empty() && result.back() == '-') result.pop_back();
  if (result.empty()) result = "bom-import";
  return result + "-" + makeId().substr(0, 8);
}

vector<string> splitTags(const string& value) {
  vector<string> result;
  string current;
  for (char ch : value) {
    if (ch == ';' || ch == '|') { if (!trim(current).empty()) result.push_back(trim(current)); current.clear(); }
    else current.push_back(ch);
  }
  if (!trim(current).empty()) result.push_back(trim(current));
  return result;
}

void detectConflict(CsvImportCandidate& candidate, const vector<InventoryItem>& existing) {
  const auto mpn = toLower(trim(candidate.item.manufacturerPartNumber));
  const auto maker = toLower(trim(candidate.item.manufacturer));
  vector<const InventoryItem*> matches;
  for (const auto& item : existing) {
    if (mpn.empty() || toLower(trim(item.manufacturerPartNumber)) != mpn) continue;
    if (!maker.empty() && !trim(item.manufacturer).empty() && toLower(trim(item.manufacturer)) != maker) continue;
    matches.push_back(&item);
  }
  if (matches.size() == 1) {
    candidate.hasConflict = true;
    candidate.existingItemId = matches.front()->id;
    candidate.existingPartName = matches.front()->partName;
    candidate.existingQuantity = matches.front()->quantity;
    candidate.matchedField = maker.empty() ? "Unique manufacturer part number" : "Manufacturer and part number";
  } else if (matches.size() > 1) {
    candidate.hasConflict = true;
    candidate.existingPartName = "Multiple inventory items";
    candidate.matchedField = "Ambiguous MPN - add the manufacturer before accepting";
    candidate.warnings.push_back("Manufacturer part number matches multiple existing manufacturers");
  }
}

}  // namespace

CsvImportResult parseBomCsvText(const string& text, const vector<InventoryItem>& existingItems,
                                const CsvColumnMapping& mapping) {
  CsvImportResult result;
  string error;
  const auto rows = parseCsv(text, error);
  if (!error.empty()) { result.error = error; return result; }
  if (rows.size() < 2) { result.error = "CSV does not contain a header and component rows"; return result; }
  const auto columns = mapColumns(rows.front(), mapping);
  if (columns.mpn < 0 || columns.quantity < 0) {
    result.error = "CSV requires manufacturer part number and quantity columns";
    return result;
  }
  for (size_t index = 1; index < rows.size(); ++index) {
    const auto mpn = cell(rows[index], columns.mpn);
    const auto quantity = positiveInt(cell(rows[index], columns.quantity));
    if (mpn.empty() || !quantity) {
      result.warnings.push_back("Skipped row " + to_string(index + 1) + ": missing part number or positive quantity");
      continue;
    }
    CsvImportCandidate candidate;
    candidate.sourceRow = index + 1;
    candidate.item.id = importedId(mpn);
    candidate.item.manufacturerPartNumber = mpn;
    candidate.item.manufacturer = cell(rows[index], columns.manufacturer);
    candidate.item.partName = cell(rows[index], columns.name);
    if (candidate.item.partName.empty()) candidate.item.partName = mpn;
    candidate.item.category = "Unsorted";
    candidate.item.quantity = *quantity;
    candidate.item.location = cell(rows[index], columns.location);
    if (candidate.item.location.empty()) candidate.item.location = "Import Inbox";
    candidate.item.notes = cell(rows[index], columns.notes);
    candidate.item.tags = splitTags(cell(rows[index], columns.tags));
    candidate.item.tags.push_back("csv-import");
    candidate.item.datasheetUrl = cell(rows[index], columns.datasheet);
    candidate.item.catalogueStatus = "not_in_catalogue";
    candidate.item.createdAt = candidate.item.lastUpdated = time(nullptr);
    detectConflict(candidate, existingItems);
    result.candidates.push_back(move(candidate));
  }
  if (result.candidates.empty()) { result.error = "CSV contains no valid component rows"; return result; }
  result.ok = true;
  return result;
}

CsvImportResult loadBomCsvFile(const filesystem::path& path, const vector<InventoryItem>& existingItems,
                               const CsvColumnMapping& mapping) {
  ifstream input(path, ios::binary);
  if (!input) return {false, "Unable to open CSV file", {}, {}};
  ostringstream buffer;
  buffer << input.rdbuf();
  return parseBomCsvText(buffer.str(), existingItems, mapping);
}

void mergeImportedMetadata(InventoryItem& target, const InventoryItem& source) {
  const auto assignBlank = [](string& destination, const string& value) {
    if (trim(destination).empty() && !trim(value).empty()) destination = trim(value);
  };
  assignBlank(target.manufacturer, source.manufacturer);
  assignBlank(target.manufacturerPartNumber, source.manufacturerPartNumber);
  assignBlank(target.category, source.category);
  assignBlank(target.location, source.location);
  assignBlank(target.datasheetUrl, source.datasheetUrl);
  assignBlank(target.notes, source.notes);
  for (const auto& tag : source.tags) {
    if (none_of(target.tags.begin(), target.tags.end(), [&](const string& value) { return toLower(value) == toLower(tag); }))
      target.tags.push_back(tag);
  }
}

}  // namespace inventatory
