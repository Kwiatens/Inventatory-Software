// Inventatory - local manufacturer parametric catalogue storage and lookup.
#pragma once

#include "core/Inventory.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

enum class DownloadMode { BrowserGuided, Direct, ManualFileOnly };
enum class CatalogueMatchStatus { ExactMatch, AliasMatch, Ambiguous, NotFound, DatabaseUnavailable };

struct ManufacturerSource {
  std::string id, manufacturer, displayName, officialDownloadPage;
  std::vector<std::string> supportedFormats, supportedCategories;
  DownloadMode downloadMode = DownloadMode::BrowserGuided;
  bool redistributionAllowed = false;
  bool automaticFetchAllowed = false;
  std::string profileId, instructions;
};

struct PropertyMapping { std::vector<std::string> columns; std::string canonicalName, unit, qualifier; };
struct CatalogueProfile {
  std::string id, version, manufacturer, category, worksheetPattern;
  std::vector<std::string> filenamePatterns, mpnColumns, basePartColumns, aliasColumns, packageColumns,
      seriesColumns, descriptionColumns, statusColumns;
  std::vector<PropertyMapping> properties;
  bool seriesOnly = false;
};

struct EngineeringValue {
  std::optional<double> nominal, minimum, typical, maximum, tolerance;
  std::string unit, condition, qualifier, rawValue, rawUnit, originalText, warning;
};

struct CatalogueProperty {
  std::string name, sourceColumn, rawValue, originalUnit, mappingStatus, condition, qualifier;
  EngineeringValue value;
};

struct CatalogueRecord {
  std::string componentId, databaseVersion, manufacturer, manufacturerPartNumber, basePart, canonicalName,
      purposeLabel, printLabel, category, packageVariant, packagingVariant, series, datasheetUrl, sourceIdentity;
  std::vector<std::string> aliases;
  std::vector<CatalogueProperty> properties;
};

struct CatalogueMatch {
  CatalogueMatchStatus status = CatalogueMatchStatus::NotFound;
  CatalogueRecord record;
  bool matched() const;
};

struct CatalogueImportStats {
  std::int64_t snapshotId = 0;
  std::size_t rows = 0, parts = 0, aliases = 0, properties = 0, warnings = 0, rejected = 0;
  bool duplicate = false, cancelled = false;
  std::string profileId, fileHash, error;
};

struct CatalogueImportPreview {
  std::string filename, format, sheetName, profileId, profileVersion, error;
  std::size_t fileSize = 0, headerRow = 0, rows = 0;
  char delimiter = ',';
  std::vector<std::string> headers, mappedColumns, unmappedColumns, warnings;
  std::vector<std::vector<std::string>> sampleRows;
  bool valid() const { return error.empty(); }
};

struct CatalogueImportOptions {
  std::atomic_bool* cancel = nullptr;
  std::function<void(std::size_t, std::size_t, const std::string&)> progress;
};

const std::vector<ManufacturerSource>& manufacturerSources();
const std::vector<CatalogueProfile>& catalogueProfiles();
const CatalogueProfile* detectCatalogueProfile(const std::filesystem::path& file,
                                                const std::vector<std::string>& headers = {});
EngineeringValue parseEngineeringValue(const std::string& text, const std::string& columnUnit = {});
std::string normalizeMpn(const std::string& value);

class CatalogueDatabase {
 public:
  bool open(const std::filesystem::path& path);
  bool available() const;
  const std::string& version() const;
  CatalogueMatch lookup(const std::string& manufacturer, const std::string& manufacturerPartNumber) const;
  CatalogueImportPreview previewFile(const std::filesystem::path& path, const CatalogueProfile* profile = nullptr,
                                     std::atomic_bool* cancel = nullptr) const;
  std::size_t reprocessSource(const CatalogueProfile& profile);
  CatalogueImportStats importFile(const std::filesystem::path& path, const CatalogueProfile* profile = nullptr,
                                  const CatalogueImportOptions& options = {});
  bool removeSource(const std::string& sourceId);
  std::vector<CatalogueImportStats> snapshots() const;
 private:
  std::filesystem::path path_;
  std::string version_;
  bool available_ = false;
};

// A detector exists only for the lifetime of a user-started guided session.
// poll() never walks outside the selected Downloads directory and ignores
// browser partial-file suffixes until size and modification time are stable.
class CatalogueDownloadSession {
 public:
  bool start(const ManufacturerSource& source, const std::filesystem::path& downloads = {});
  std::optional<std::filesystem::path> poll();
  void cancel();
  bool active() const;
  const std::filesystem::path& watchedDirectory() const;
 private:
  ManufacturerSource source_;
  std::filesystem::path directory_;
  std::chrono::system_clock::time_point started_{};
  std::map<std::filesystem::path, std::pair<std::uintmax_t, int>> candidates_;
  bool active_ = false;
};

std::filesystem::path standardDownloadsFolder();

bool applyCatalogueEnrichment(InventoryItem& item, const CatalogueMatch& match);
std::string effectiveDatasheetUrl(const InventoryItem& item);

}  // namespace inventatory
