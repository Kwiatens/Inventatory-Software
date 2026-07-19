// Inventatory - Hardware Inventory Management System
// Supplier-neutral BOM and order CSV parsing.

#pragma once

#include "core/Inventory.h"

#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

struct CsvImportCandidate {
  InventoryItem item;
  size_t sourceRow = 0;
  bool hasConflict = false;
  std::string existingItemId;
  std::string existingPartName;
  std::string matchedField;
  int existingQuantity = 0;
  std::vector<std::string> warnings;
};

struct CsvImportResult {
  bool ok = false;
  std::string error;
  std::vector<std::string> warnings;
  std::vector<CsvImportCandidate> candidates;
};

struct CsvColumnMapping {
  std::string manufacturerPartNumber;
  std::string quantity;
  std::string manufacturer;
  std::string partName;
  std::string datasheetUrl;
  std::string location;
  std::string notes;
  std::string tags;
};

CsvImportResult parseBomCsvText(const std::string& text, const std::vector<InventoryItem>& existingItems,
                                const CsvColumnMapping& mapping = {});
CsvImportResult loadBomCsvFile(const std::filesystem::path& path,
                               const std::vector<InventoryItem>& existingItems,
                               const CsvColumnMapping& mapping = {});
void mergeImportedMetadata(InventoryItem& target, const InventoryItem& source);

}  // namespace inventatory
