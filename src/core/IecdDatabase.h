// Inventatory - Hardware Inventory Management System
// Read-only deterministic lookup for Inventatory Electronics Components Database snapshots.

#pragma once

#include "core/Inventory.h"

#include <filesystem>
#include <string>

namespace inventatory {

struct IecdRecord {
  std::string componentId;
  std::string databaseVersion;
  std::string manufacturer;
  std::string manufacturerPartNumber;
  std::string canonicalName;
  std::string purposeLabel;
  std::string printLabel;
  std::string category;
  std::string datasheetUrl;
};

struct IecdMatch {
  IecdMatchStatus status = IecdMatchStatus::NotFound;
  IecdRecord record;

  bool matched() const;
};

class IecdDatabase {
 public:
  bool open(const std::filesystem::path& path);
  bool available() const;
  const std::string& version() const;
  IecdMatch lookup(const std::string& manufacturer, const std::string& manufacturerPartNumber) const;

 private:
  std::filesystem::path path_;
  std::string version_;
  bool available_ = false;
};

bool applyIecdEnrichment(InventoryItem& item, const IecdMatch& match);
std::string effectiveDatasheetUrl(const InventoryItem& item);

}  // namespace inventatory
