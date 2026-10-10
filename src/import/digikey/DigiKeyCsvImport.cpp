// Inventatory - Hardware Inventory Management System
// DigiKey order CSV parsing and import candidate preparation.

#include "import/digikey/DigiKeyCsvImport.h"

#include "import/csv/CsvReader.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>
#include <optional>
#include <unordered_map>

namespace inventatory {

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
    const int digit = static_cast<int>(ch - '0');
    if (result > (numeric_limits<int>::max() - digit) / 10) {
      return nullopt;
    }
    result = result * 10 + digit;
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

// Lowercase alphanumeric words of a description: "IC BUF 5.5V SC70-5" -> ic, buf, 5, 5v, sc70, 5.
vector<string> descriptionWords(const string& description) {
  vector<string> words;
  string current;
  for (const unsigned char ch : description) {
    if (isalnum(ch) != 0) {
      current.push_back(static_cast<char>(tolower(ch)));
    } else if (!current.empty()) {
      words.push_back(move(current));
      current.clear();
    }
  }
  if (!current.empty()) words.push_back(move(current));
  return words;
}

// "100nf", "1uf" and "22pf" in a description are capacitor values; "uf" or "nf" inside another word
// (BUF, UFBGA, INFRARED) are not.
bool isCapacitanceWord(const string& word) {
  if (word.size() < 3) return false;
  const auto suffix = word.substr(word.size() - 2);
  if (suffix != "uf" && suffix != "nf" && suffix != "pf") return false;
  return all_of(word.begin(), word.end() - 2, [](unsigned char ch) { return isdigit(ch) != 0; });
}

// Matches whole words only, so SHIELDED, CONTROLLED and BUNDLED are not LEDs and BUF is not a capacitor.
string inferCategory(const string& description) {
  const auto words = descriptionWords(description);
  const auto hasWord = [&](initializer_list<const char*> candidates) {
    return any_of(words.begin(), words.end(), [&](const string& word) {
      return any_of(candidates.begin(), candidates.end(), [&](const char* candidate) { return word == candidate; });
    });
  };
  // A ferrite bead is rated in ohms but is an inductor-class part, not a resistor.
  if (hasWord({"ferrite", "bead", "beads"})) {
    return "Inductors";
  }
  if (hasWord({"res", "resistor", "resistors", "ohm", "ohms"})) {
    return "Resistors";
  }
  if (hasWord({"cap", "capacitor", "capacitors"}) || any_of(words.begin(), words.end(), isCapacitanceWord)) {
    return "Capacitors";
  }
  if (hasWord({"led", "leds"})) {
    return "Indicators";
  }
  if (hasWord({"conn", "connector", "connectors", "header", "headers"})) {
    return "Connectors";
  }
  if (hasWord({"ic", "ics", "mcu", "microcontroller", "microcontrollers"})) {
    return "Integrated Circuits";
  }
  const bool fixedInductor = [&] {
    for (size_t index = 0; index + 1 < words.size(); ++index) {
      if (words[index] == "fixed" && words[index + 1] == "ind") return true;
    }
    return false;
  }();
  if (hasWord({"inductor", "inductors"}) || fixedInductor) {
    return "Inductors";
  }
  if (hasWord({"fuse", "fuses"})) {
    return "Fuses";
  }
  if (hasWord({"diode", "diodes", "rectifier", "rectifiers"})) {
    return "Diodes";
  }
  if (hasWord({"switch", "switches"})) {
    return "Switches";
  }
  return "Unsorted";
}

string productSearchUrl(const string& digikeyPart) {
  const auto part = trim(digikeyPart);
  if (part.empty()) {
    return {};
  }

  string encoded;
  encoded.reserve(part.size());
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const unsigned char character : part) {
    const bool asciiAlphaNumeric = (character >= 'A' && character <= 'Z') ||
                                   (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    if (asciiAlphaNumeric || character == '-' || character == '_' || character == '.' || character == '~') {
      encoded.push_back(static_cast<char>(character));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[(character >> 4U) & 0x0FU]);
      encoded.push_back(kHex[character & 0x0FU]);
    }
  }
  return "https://www.digikey.com/en/products/result?keywords=" + encoded;
}

int addQuantities(int current, int incoming) {
  if (incoming > 0 && current > numeric_limits<int>::max() - incoming) {
    return numeric_limits<int>::max();
  }
  return max(0, current + incoming);
}

bool isRequiredColumnSetPresent(const ColumnMap& columns) {
  return columns.manufacturerPart >= 0 && columns.manufacturer >= 0 &&
         columns.description >= 0 && columns.quantity >= 0;
}

bool rowLooksLikeDigiKeyOrderLine(const vector<string>& row, const ColumnMap& columns) {
  const auto quantity = parsePositiveInt(csvCell(row, columns.quantity));
  if (!quantity || *quantity == 0) {
    return false;
  }

  return (looksLikeDigiKeyPart(csvCell(row, columns.digikeyPart)) ||
          looksLikeManufacturerPart(csvCell(row, columns.manufacturerPart))) &&
         looksLikeManufacturerPart(csvCell(row, columns.manufacturerPart)) &&
         !csvCell(row, columns.manufacturer).empty() &&
         !csvCell(row, columns.description).empty();
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

struct InventoryIndex {
  unordered_map<string, const InventoryItem*> byDigiKeyPart;
  unordered_map<string, const InventoryItem*> bySku;
};

InventoryIndex buildInventoryIndex(const vector<InventoryItem>& existingItems) {
  InventoryIndex index;
  index.byDigiKeyPart.reserve(existingItems.size());
  index.bySku.reserve(existingItems.size());
  for (const auto& item : existingItems) {
    const auto digikeyKey = toLower(trim(item.digikeyPartNumber));
    if (!digikeyKey.empty()) {
      index.byDigiKeyPart.emplace(digikeyKey, &item);
    }
    const auto skuKey = toLower(trim(item.sku));
    if (!skuKey.empty()) {
      index.bySku.emplace(skuKey, &item);
    }
  }
  return index;
}

void detectConflict(CsvImportCandidate& candidate, const InventoryIndex& index) {
  const auto digikeyKey = toLower(trim(candidate.item.digikeyPartNumber));
  if (!digikeyKey.empty()) {
    const auto it = index.byDigiKeyPart.find(digikeyKey);
    if (it != index.byDigiKeyPart.end()) {
      const auto* item = it->second;
      candidate.hasConflict = true;
      candidate.existingItemId = item->id;
      candidate.existingPartName = item->partName;
      candidate.existingQuantity = item->quantity;
      candidate.matchedField = "DigiKey part";
      return;
    }
  }

  const auto skuKey = toLower(trim(candidate.item.sku));
  if (!skuKey.empty()) {
    const auto it = index.bySku.find(skuKey);
    if (it != index.bySku.end()) {
      const auto* item = it->second;
      candidate.hasConflict = true;
      candidate.existingItemId = item->id;
      candidate.existingPartName = item->partName;
      candidate.existingQuantity = item->quantity;
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
                                    const InventoryIndex& index) {
  CsvImportCandidate candidate;
  candidate.sourceRow = sourceRow;

  const auto digikeyPart = csvCell(row, columns.digikeyPart);
  const auto manufacturerPart = csvCell(row, columns.manufacturerPart);
  const auto description = csvCell(row, columns.description);
  const auto quantity = parsePositiveInt(csvCell(row, columns.quantity)).value_or(0);

  candidate.item.id = makeImportedId(digikeyPart, manufacturerPart);
  candidate.item.partName = description;
  candidate.item.manufacturer = csvCell(row, columns.manufacturer);
  candidate.item.category = inferCategory(description);
  candidate.item.quantity = quantity;
  candidate.item.location = "Import Inbox";
  candidate.item.tags = {"digikey", "csv-import"};
  candidate.item.notes = "Imported from DigiKey CSV row " + to_string(sourceRow) + ".";
  candidate.item.digikeyPartNumber = digikeyPart;
  candidate.item.productUrl = productSearchUrl(digikeyPart);
  candidate.item.syncStatus = "needs_metadata";
  candidate.item.sku = manufacturerPart;
  candidate.item.lastUpdated = time(nullptr);
  candidate.item.createdAt = candidate.item.lastUpdated;

  addOptionalParameter(candidate.item.parameters, "Customer Reference", csvCell(row, columns.customerReference));
  addOptionalParameter(candidate.item.parameters, "Backorder Quantity", csvCell(row, columns.backorderQuantity));
  addOptionalParameter(candidate.item.parameters, "Unit Price", csvCell(row, columns.unitPrice));
  addOptionalParameter(candidate.item.parameters, "Line Value", csvCell(row, columns.lineValue));
  addOptionalParameter(candidate.item.parameters, "Source Row", to_string(sourceRow));

  detectConflict(candidate, index);
  return candidate;
}

}  // namespace

CsvImportResult parseDigiKeyCsvText(const string& text, const vector<InventoryItem>& existingItems) {
  CsvImportResult result;
  if (text.size() > kMaximumCsvInputBytes) {
    result.error = "CSV import exceeds the 25 MiB safety limit";
    return result;
  }
  string parseError;
  const auto cleaned = stripByteOrderMark(text);
  const auto rows = parseCsv(cleaned, sniffDelimiter(cleaned), parseError);
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

  const auto index = buildInventoryIndex(existingItems);

  size_t compatibleRows = 0;
  for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
    const auto& row = rows[rowIndex];
    if (rowLooksLikeDigiKeyOrderLine(row, columns)) {
      ++compatibleRows;
      result.candidates.push_back(candidateFromRow(row, columns, rowIndex + 1, index));
    } else if (any_of(row.begin(), row.end(), [](const string& value) { return !trim(value).empty(); })) {
      result.warnings.push_back("Skipped row " + to_string(rowIndex + 1) + ": not a valid DigiKey product line");
    }
  }

  if (compatibleRows == 0) {
    result.error = "CSV headers were recognized, but no valid DigiKey product rows were found";
    return result;
  }

  // DigiKey exports can repeat a line when an order was split across
  // shipments. Treat the CSV as one logical receipt: keep the first row's
  // review metadata and add quantities from later rows with the same DigiKey
  // number, falling back to the manufacturer number.
  vector<CsvImportCandidate> consolidated;
  unordered_map<string, size_t> byPart;
  for (auto& candidate : result.candidates) {
    const auto primary = trim(candidate.item.digikeyPartNumber).empty()
                             ? candidate.item.sku
                             : candidate.item.digikeyPartNumber;
    const auto key = toLower(trim(primary));
    const auto existing = byPart.find(key);
    if (key.empty() || existing == byPart.end()) {
      if (!key.empty()) byPart.emplace(key, consolidated.size());
      consolidated.push_back(move(candidate));
      continue;
    }

    auto& merged = consolidated[existing->second];
    merged.item.quantity = addQuantities(merged.item.quantity, candidate.item.quantity);
    merged.item.lastUpdated = time(nullptr);
    merged.item.notes += " Also received on CSV row " + to_string(candidate.sourceRow) + ".";
    merged.warnings.push_back("Merged duplicate part from row " + to_string(candidate.sourceRow));
  }
  result.candidates = move(consolidated);

  result.ok = true;
  return result;
}

CsvImportResult loadDigiKeyCsvFile(const filesystem::path& path, const vector<InventoryItem>& existingItems) {
  string decoded;
  CsvImportResult result;
  if (!readCsvFile(path, "CSV", decoded, result.error)) return result;
  return parseDigiKeyCsvText(decoded, existingItems);
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
  assignIfBlank(target.inventatoryId, source.inventatoryId);
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

}  // namespace inventatory
