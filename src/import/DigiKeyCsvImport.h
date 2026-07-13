// HIMS - Hardware Inventory Management System
// DigiKey order CSV parsing and import candidate preparation.

#pragma once

#include "core/Inventory.h"

#include <filesystem>
#include <string>
#include <vector>

namespace hims {

namespace filesystem = std::filesystem;
using std::filesystem::path;
using std::string;
using std::vector;

struct CsvImportCandidate {
  InventoryItem item;
  size_t sourceRow = 0;
  bool hasConflict = false;
  string existingItemId;
  string existingPartName;
  string matchedField;
  int existingQuantity = 0;
  vector<string> warnings;
};

struct CsvImportResult {
  bool ok = false;
  string error;
  vector<string> warnings;
  vector<CsvImportCandidate> candidates;
};

CsvImportResult parseDigiKeyCsvText(const string& text, const vector<InventoryItem>& existingItems);
CsvImportResult loadDigiKeyCsvFile(const filesystem::path& path, const vector<InventoryItem>& existingItems);

void mergeImportedMetadata(InventoryItem& target, const InventoryItem& source);

}  // namespace hims
