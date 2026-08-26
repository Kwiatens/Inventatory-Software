#pragma once

#include "core/PhysicalValue.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::filesystem::path;
using std::size_t;
using std::string;
using std::string_view;
using std::time_t;
using std::vector;

struct Parameter {
  string name;
  string value;
};

// Provider-neutral catalog metadata retained with an inventory item.  Provider
// adapters populate this shape; label resolution never depends on vendor prose
// outside the provider category and structured parameters.
struct VendorProductMetadata {
  string provider;
  string providerProductNumber;
  string manufacturerPartNumber;
  string categoryId;
  vector<string> categoryPath;
  string title;
  string detailedDescription;
  vector<Parameter> parameters;
  string productUrl;
  string locale;
};

enum class RackAssignmentMode {
  Automatic,
  Manual,
  Unassigned,
};

struct InventatoryRack {
  string id;
  string code;
  string componentType;
  int rows = 5;
  int columns = 5;
  time_t createdAt = 0;
};

struct InventoryItem {
  string id;
  string partName;
  string manufacturer;
  string category;
  int quantity = 0;
  // Retained only so older inventory databases and serialized snapshots round-trip.
  // Low-stock behavior is controlled by AppSettings::lowStockThreshold.
  int reorderThreshold = 0;
  string location;
  vector<string> tags;
  vector<Parameter> parameters;
  string notes;
  string digikeyPartNumber;
  string datasheetUrl;
  string productUrl;
  string syncStatus = "synced";
  string sku;
  time_t lastUpdated = 0;
  string inventatoryId;
  time_t createdAt = 0;
  string machineCode;
  string rackId;
  string rackSlot;
  RackAssignmentMode rackAssignment = RackAssignmentMode::Automatic;
  string labelOverride;
  VendorProductMetadata vendorMetadata;

  bool hasMissingMetadata() const;
  string searchableText() const;
};

struct ActivityEntry {
  time_t timestamp = 0;
  string kind;
  string message;
};

struct Summary {
  size_t itemCount = 0;
  size_t totalUnits = 0;
  size_t lowStockCount = 0;
  size_t missingMetadataCount = 0;
  size_t unsyncedCount = 0;
};

struct InventoryHistoryPoint {
  time_t timestamp = 0;
  size_t itemCount = 0;
  size_t totalUnits = 0;
  size_t lowStockCount = 0;
  size_t outOfStockCount = 0;
  size_t dataErrorCount = 0;
};

// A completed device event is persisted in the same SQLite transaction as the
// inventory snapshot so retrying a sync event can never repeat its stock effect.
struct DeviceEventCommit {
  string eventId;
  string resultId;
  string status;
  bool existing = false;
  string itemName;
  int requestedDelta = 0;
  int appliedDelta = 0;
  int quantity = 0;
  string location;
  string code;
  string message;
  time_t completedAt = 0;
};

struct ScanResolution {
  bool matched = false;
  bool created = false;
  string itemId;
  string message;
};

string trim(const string& value);
string toLower(string value);
string toUpper(string value);
string nowTimestampString(time_t value);
string makeId();
string inventatoryCategoryPrefix(const string& category);
string makeInventatoryId(const string& category, size_t sequence);
bool isInventatoryId(const string& value);
void ensureInventoryIdentifiers(vector<InventoryItem>& items);
string join(const vector<string>& values, char delimiter);
vector<string> split(const string& value, char delimiter);
vector<string> tokenizeQuery(const string& query);

bool containsInsensitive(string_view haystack, string_view needle);
bool isLowStock(const InventoryItem& item, int threshold);
bool matchesQuery(const InventoryItem& item, const string& query, int lowStockThreshold);
bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks,
                  int lowStockThreshold);
bool matchesQuery(const InventoryItem& item, const string& query, int lowStockThreshold,
                  const PhysicalValueTolerances& tolerances);
bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks,
                  int lowStockThreshold, const PhysicalValueTolerances& tolerances);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, int lowStockThreshold);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks, int lowStockThreshold);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, int lowStockThreshold,
                           const PhysicalValueTolerances& tolerances);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks, int lowStockThreshold,
                           const PhysicalValueTolerances& tolerances);
Summary summarize(const vector<InventoryItem>& items, int lowStockThreshold);
InventoryHistoryPoint makeInventoryHistoryPoint(const vector<InventoryItem>& items, int lowStockThreshold,
                                                time_t timestamp = 0);

// Compatibility overloads use the historical default only for callers that do
// not own application settings. The application always supplies its setting.
bool matchesQuery(const InventoryItem& item, const string& query);
bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks);
Summary summarize(const vector<InventoryItem>& items);
InventoryHistoryPoint makeInventoryHistoryPoint(const vector<InventoryItem>& items, time_t timestamp = 0);
bool loadInventoryHistory(const filesystem::path& path, vector<InventoryHistoryPoint>& history);
bool saveInventoryHistory(const filesystem::path& path, const vector<InventoryHistoryPoint>& history);
void appendInventoryHistory(vector<InventoryHistoryPoint>& history, const InventoryHistoryPoint& point,
                            size_t maxEntries = 180);

class InventoryStore {
 public:
  vector<InventoryItem>& items();
  const vector<InventoryItem>& items() const;
  vector<InventatoryRack>& racks();
  const vector<InventatoryRack>& racks() const;

  bool load(const filesystem::path& path);
  bool save(const filesystem::path& path) const;
  bool saveWithDeviceEvent(const filesystem::path& path, const DeviceEventCommit& event) const;

  InventoryItem* findById(const string& id);
  const InventoryItem* findById(const string& id) const;
  InventoryItem* findByCode(const string& code);
  const InventoryItem* findByCode(const string& code) const;
  InventoryItem* findByMachineCode(const string& machineCode);
  const InventoryItem* findByMachineCode(const string& machineCode) const;

 private:
  vector<InventoryItem> items_;
  vector<InventatoryRack> racks_;
};

string rackAssignmentModeName(RackAssignmentMode mode);
RackAssignmentMode parseRackAssignmentMode(const string& value);
string rackLocation(const InventoryItem& item, const vector<InventatoryRack>& racks);
bool isValidRackSlot(const string& slot);
bool reconcileRackAssignment(InventoryStore& store, InventoryItem& item);
bool reconcileRackAssignments(InventoryStore& store);
bool setManualRackLocation(InventoryStore& store, InventoryItem& item, const string& value, string& error);
int rackNumberFromCode(const string& code);
string rackSlotLabel(int row, int column);
size_t rackOccupiedSlotCount(const InventoryStore& store, const InventatoryRack& rack);
InventoryItem* itemAtRackSlot(InventoryStore& store, const string& rackId, const string& slot);
const InventoryItem* itemAtRackSlot(const InventoryStore& store, const string& rackId, const string& slot);
bool moveItemToRackSlot(InventoryStore& store, InventoryItem& item, const InventatoryRack& rack, const string& slot,
                        string& error);
bool unassignItemFromRack(InventoryItem& item);
bool restoreAutomaticRackAssignment(InventoryStore& store, InventoryItem& item);

bool loadActivities(const filesystem::path& path, vector<ActivityEntry>& activities);
bool saveActivities(const filesystem::path& path, const vector<ActivityEntry>& activities);
void appendActivity(vector<ActivityEntry>& activities, const ActivityEntry& entry, size_t maxEntries = 100);
ActivityEntry makeActivity(string kind, string message);

// Physical value helpers for unit-aware search.
double toleranceForType(const PhysicalValueTolerances& tolerances, PhysicalValueType type);

ScanResolution resolveScanCode(InventoryStore& store, const string& rawCode);

string serializeItem(const InventoryItem& item);
bool deserializeItem(const string& line, InventoryItem& item);
string serializeTagsForStorage(const vector<string>& tags);
vector<string> deserializeTagsFromStorage(const string& value);
string serializeParametersForStorage(const vector<Parameter>& parameters);
vector<Parameter> deserializeParametersFromStorage(const string& value);
string serializeActivity(const ActivityEntry& entry);
bool deserializeActivity(const string& line, ActivityEntry& entry);

}  // namespace inventatory
