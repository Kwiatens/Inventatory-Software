// HIMS - Hardware Inventory Management System
// DigiKey order CSV parsing and import candidate preparation.

#include "import/DigiKeyCsvImport.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace hims {

using namespace std;

namespace {

struct ColumnMap {
  int digikeyPart = -1;
  int manufacturerPart = -1;
  int manufacturer = -1;
  int description = -1;
  int quantity = -1;
  int customerReference = -1;
  int backorderQuantity = -1;
  int unitPrice = -1;
  int lineValue = -1;
};

string normalizeHeader(string value) {
  value = toLower(trim(value));
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(ch));
    }
  }
  return normalized;
}

bool anyHeaderMatches(const string& header, initializer_list<const char*> aliases) {
  const auto normalized = normalizeHeader(header);
  for (const auto* alias : aliases) {
    if (normalized == normalizeHeader(alias)) {
      return true;
    }
  }
  return false;
}

vector<vector<string>> parseCsv(const string& text, string& error) {
  vector<vector<string>> rows;
  vector<string> row;
  string field;
  bool inQuotes = false;

  for (size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];

    if (inQuotes) {
      if (ch == '"') {
        if (index + 1 < text.size() && text[index + 1] == '"') {
          field.push_back('"');
          ++index;
        } else {
          inQuotes = false;
        }
      } else {
        field.push_back(ch);
      }
      continue;
    }

    if (ch == '"') {
      inQuotes = true;
    } else if (ch == ',') {
      row.push_back(trim(field));
      field.clear();
    } else if (ch == '\n') {
      row.push_back(trim(field));
      field.clear();
      if (!row.empty() && !(row.size() == 1 && row.front().empty())) {
        rows.push_back(move(row));
      }
      row.clear();
    } else if (ch != '\r') {
      field.push_back(ch);
    }
  }

  if (inQuotes) {
    error = "CSV has an unterminated quoted field";
    return {};
  }

  row.push_back(trim(field));
  if (!row.empty() && !(row.size() == 1 && row.front().empty())) {
    rows.push_back(move(row));
  }

  return rows;
}

optional<int> parsePositiveInt(const string& value) {
  const auto trimmed = trim(value);
  if (trimmed.empty()) {
    return nullopt;
  }

  int result = 0;
  for (unsigned char ch : trimmed) {
    if (!isdigit(ch)) {
      return nullopt;
    }
    result = result * 10 + static_cast<int>(ch - '0');
  }
  return result;
}

bool looksLikeDigiKeyPart(const string& value) {
  const auto lowered = toLower(trim(value));
  return lowered.find("-nd") != string::npos || lowered.find("digikey") != string::npos;
}

bool looksLikeManufacturerPart(const string& value) {
  const auto trimmed = trim(value);
  if (trimmed.size() < 2) {
    return false;
  }
  return any_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) {
    return isdigit(ch) != 0;
  });
}

int findColumn(const vector<string>& headers, initializer_list<const char*> aliases) {
  for (size_t index = 0; index < headers.size(); ++index) {
    if (anyHeaderMatches(headers[index], aliases)) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

ColumnMap mapColumns(const vector<string>& headers) {
  ColumnMap columns;
  columns.digikeyPart = findColumn(headers, {"Digi-Key Part Number", "DigiKey Part Number", "DigiKey Part",
                                             "Nr kat. DigiKey", "Digi-Key Part #"});
  columns.manufacturerPart = findColumn(headers, {"Manufacturer Part Number", "Mfr Part Number",
                                                  "Manufacturer Part #", "Nr producenta"});
  columns.manufacturer = findColumn(headers, {"Manufacturer", "Producent", "Mfr"});
  columns.description = findColumn(headers, {"Description", "Opis", "Product Description"});
  columns.quantity = findColumn(headers, {"Quantity", "Qty", "Ilość", "Ilosc"});
  columns.customerReference = findColumn(headers, {"Customer Reference", "Customer Ref",
                                                   "Numer referencyjny klienta"});
  columns.backorderQuantity = findColumn(headers, {"Backorder Quantity", "Backorder", "Niezrealizowana pozycja zamówienia",
                                                   "Niezrealizowana pozycja zamowienia"});
  columns.unitPrice = findColumn(headers, {"Unit Price", "Cena jednostkowa"});
  columns.lineValue = findColumn(headers, {"Extended Price", "Line Value", "Wartość", "Wartosc"});
  return columns;
}

string cell(const vector<string>& row, int index) {
  if (index < 0 || static_cast<size_t>(index) >= row.size()) {
    return {};
  }
  return trim(row[static_cast<size_t>(index)]);
}

string inferCategory(const string& description) {
  const auto text = toLower(description);
  if (text.find("res ") != string::npos || text.find("resistor") != string::npos ||
      text.find(" ohm") != string::npos) {
    return "Resistors";
  }
  if (text.find("cap ") != string::npos || text.find("capacitor") != string::npos ||
      text.find("uf") != string::npos || text.find("nf") != string::npos) {
    return "Capacitors";
  }
  if (text.find("led") != string::npos) {
    return "Indicators";
  }
  if (text.find("conn") != string::npos || text.find("header") != string::npos) {
    return "Connectors";
  }
  if (text.find("ic ") != string::npos || text.find("mcu") != string::npos ||
      text.find("microcontroller") != string::npos) {
    return "Integrated Circuits";
  }
  if (text.find("inductor") != string::npos || text.find("fixed ind") != string::npos) {
    return "Inductors";
  }
  if (text.find("fuse") != string::npos) {
    return "Fuses";
  }
  if (text.find("diode") != string::npos || text.find("rectifier") != string::npos) {
    return "Diodes";
  }
  if (text.find("switch") != string::npos) {
    return "Switches";
  }
  return "Unsorted";
}

string productSearchUrl(const string& digikeyPart) {
  const auto part = trim(digikeyPart);
  if (part.empty()) {
    return {};
  }
  return "https://www.digikey.com/en/products/result?keywords=" + part;
}

bool isRequiredColumnSetPresent(const ColumnMap& columns) {
  return columns.digikeyPart >= 0 && columns.manufacturerPart >= 0 && columns.manufacturer >= 0 &&
         columns.description >= 0 && columns.quantity >= 0;
}

bool rowLooksLikeDigiKeyOrderLine(const vector<string>& row, const ColumnMap& columns) {
  const auto quantity = parsePositiveInt(cell(row, columns.quantity));
  if (!quantity || *quantity == 0) {
    return false;
  }

  return looksLikeDigiKeyPart(cell(row, columns.digikeyPart)) &&
         looksLikeManufacturerPart(cell(row, columns.manufacturerPart)) &&
         !cell(row, columns.manufacturer).empty() &&
         !cell(row, columns.description).empty();
}

string makeImportedId(const string& digikeyPart, const string& manufacturerPart) {
  const auto source = !trim(digikeyPart).empty() ? digikeyPart : manufacturerPart;
  string cleaned;
  cleaned.reserve(source.size());
  for (unsigned char ch : source) {
    if (isalnum(ch)) {
      cleaned.push_back(static_cast<char>(tolower(ch)));
    } else if (!cleaned.empty() && cleaned.back() != '-') {
      cleaned.push_back('-');
    }
  }
  while (!cleaned.empty() && cleaned.back() == '-') {
    cleaned.pop_back();
  }
  if (cleaned.empty()) {
    cleaned = "digikey-import";
  }
  return cleaned + "-" + makeId().substr(0, 8);
}

bool sameCode(const string& lhs, const string& rhs) {
  return !trim(lhs).empty() && toLower(trim(lhs)) == toLower(trim(rhs));
}

void detectConflict(CsvImportCandidate& candidate, const vector<InventoryItem>& existingItems) {
  for (const auto& item : existingItems) {
    if (sameCode(candidate.item.digikeyPartNumber, item.digikeyPartNumber)) {
      candidate.hasConflict = true;
      candidate.existingItemId = item.id;
      candidate.existingPartName = item.partName;
      candidate.existingQuantity = item.quantity;
      candidate.matchedField = "DigiKey part";
      return;
    }
  }

  for (const auto& item : existingItems) {
    if (sameCode(candidate.item.sku, item.sku)) {
      candidate.hasConflict = true;
      candidate.existingItemId = item.id;
      candidate.existingPartName = item.partName;
      candidate.existingQuantity = item.quantity;
      candidate.matchedField = "Manufacturer part";
      return;
    }
  }
}

void addOptionalParameter(vector<Parameter>& parameters, const string& name, const string& value) {
  const auto trimmed = trim(value);
  if (!trimmed.empty()) {
    parameters.push_back({name, trimmed});
  }
}

CsvImportCandidate candidateFromRow(const vector<string>& row, const ColumnMap& columns, size_t sourceRow,
                                    const vector<InventoryItem>& existingItems) {
  CsvImportCandidate candidate;
  candidate.sourceRow = sourceRow;

  const auto digikeyPart = cell(row, columns.digikeyPart);
  const auto manufacturerPart = cell(row, columns.manufacturerPart);
  const auto description = cell(row, columns.description);
  const auto quantity = parsePositiveInt(cell(row, columns.quantity)).value_or(0);

  candidate.item.id = makeImportedId(digikeyPart, manufacturerPart);
  candidate.item.partName = description;
  candidate.item.manufacturer = cell(row, columns.manufacturer);
  candidate.item.category = inferCategory(description);
  candidate.item.quantity = quantity;
  candidate.item.reorderThreshold = categoryLowStockThreshold(candidate.item.category);
  candidate.item.location = "Import Inbox";
  candidate.item.tags = {"digikey", "csv-import"};
  candidate.item.notes = "Imported from DigiKey CSV row " + to_string(sourceRow) + ".";
  candidate.item.digikeyPartNumber = digikeyPart;
  candidate.item.productUrl = productSearchUrl(digikeyPart);
  candidate.item.syncStatus = "needs_metadata";
  candidate.item.sku = manufacturerPart;
  candidate.item.lastUpdated = time(nullptr);
  candidate.item.createdAt = candidate.item.lastUpdated;

  addOptionalParameter(candidate.item.parameters, "Customer Reference", cell(row, columns.customerReference));
  addOptionalParameter(candidate.item.parameters, "Backorder Quantity", cell(row, columns.backorderQuantity));
  addOptionalParameter(candidate.item.parameters, "Unit Price", cell(row, columns.unitPrice));
  addOptionalParameter(candidate.item.parameters, "Line Value", cell(row, columns.lineValue));
  addOptionalParameter(candidate.item.parameters, "Source Row", to_string(sourceRow));

  detectConflict(candidate, existingItems);
  return candidate;
}

}  // namespace

CsvImportResult parseDigiKeyCsvText(const string& text, const vector<InventoryItem>& existingItems) {
  CsvImportResult result;
  string parseError;
  const auto rows = parseCsv(text, parseError);
  if (!parseError.empty()) {
    result.error = parseError;
    return result;
  }
  if (rows.size() < 2) {
    result.error = "CSV does not contain a header and product rows";
    return result;
  }

  const auto columns = mapColumns(rows.front());
  if (!isRequiredColumnSetPresent(columns)) {
    result.error = "CSV is missing required DigiKey order columns";
    return result;
  }

  size_t compatibleRows = 0;
  for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
    const auto& row = rows[rowIndex];
    if (rowLooksLikeDigiKeyOrderLine(row, columns)) {
      ++compatibleRows;
      result.candidates.push_back(candidateFromRow(row, columns, rowIndex + 1, existingItems));
    } else if (any_of(row.begin(), row.end(), [](const string& value) { return !trim(value).empty(); })) {
      result.warnings.push_back("Skipped row " + to_string(rowIndex + 1) + ": not a valid DigiKey product line");
    }
  }

  if (compatibleRows == 0) {
    result.error = "CSV headers were recognized, but no valid DigiKey product rows were found";
    return result;
  }

  result.ok = true;
  return result;
}

CsvImportResult loadDigiKeyCsvFile(const filesystem::path& path, const vector<InventoryItem>& existingItems) {
  ifstream input(path, ios::binary);
  if (!input) {
    CsvImportResult result;
    result.error = "Unable to open CSV file";
    return result;
  }

  ostringstream buffer;
  buffer << input.rdbuf();
  return parseDigiKeyCsvText(buffer.str(), existingItems);
}

void mergeImportedMetadata(InventoryItem& target, const InventoryItem& source) {
  const auto assignIfBlank = [](string& targetValue, const string& sourceValue) {
    if (trim(targetValue).empty() && !trim(sourceValue).empty()) {
      targetValue = trim(sourceValue);
    }
  };

  assignIfBlank(target.manufacturer, source.manufacturer);
  assignIfBlank(target.category, source.category);
  assignIfBlank(target.location, source.location);
  assignIfBlank(target.digikeyPartNumber, source.digikeyPartNumber);
  assignIfBlank(target.productUrl, source.productUrl);
  assignIfBlank(target.sku, source.sku);
  assignIfBlank(target.himsId, source.himsId);
  assignIfBlank(target.machineCode, source.machineCode);
  if (target.createdAt == 0) {
    target.createdAt = source.createdAt == 0 ? target.lastUpdated : source.createdAt;
  }

  for (const auto& tag : source.tags) {
    const auto exists = any_of(target.tags.begin(), target.tags.end(), [&](const string& existing) {
      return toLower(existing) == toLower(tag);
    });
    if (!exists) {
      target.tags.push_back(tag);
    }
  }

  for (const auto& parameter : source.parameters) {
    const auto exists = any_of(target.parameters.begin(), target.parameters.end(), [&](const Parameter& existing) {
      return toLower(existing.name) == toLower(parameter.name);
    });
    if (!exists) {
      target.parameters.push_back(parameter);
    }
  }
}

}  // namespace hims
