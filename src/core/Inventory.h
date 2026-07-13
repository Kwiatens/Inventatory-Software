#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hims {

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

enum class RackAssignmentMode {
  Automatic,
  Manual,
  Unassigned,
};

struct HimsRack {
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
  string himsId;
  time_t createdAt = 0;
  string machineCode;
  string rackId;
  string rackSlot;
  RackAssignmentMode rackAssignment = RackAssignmentMode::Automatic;

  bool lowStock() const;
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
string nowTimestampString(time_t value);
string makeId();
string himsCategoryPrefix(const string& category);
string makeHimsId(const string& category, size_t sequence);
bool isHimsId(const string& value);
void ensureInventoryIdentifiers(vector<InventoryItem>& items);
string join(const vector<string>& values, char delimiter);
vector<string> split(const string& value, char delimiter);
vector<string> tokenizeQuery(const string& query);

bool containsInsensitive(string_view haystack, string_view needle);
bool matchesQuery(const InventoryItem& item, const string& query);
bool matchesQuery(const InventoryItem& item, const string& query, const vector<HimsRack>& racks);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, const vector<HimsRack>& racks);
Summary summarize(const vector<InventoryItem>& items);
int categoryLowStockThreshold(const string& category);
bool lowStockByCategory(const InventoryItem& item);
InventoryHistoryPoint makeInventoryHistoryPoint(const vector<InventoryItem>& items, time_t timestamp = 0);
bool loadInventoryHistory(const filesystem::path& path, vector<InventoryHistoryPoint>& history);
bool saveInventoryHistory(const filesystem::path& path, const vector<InventoryHistoryPoint>& history);
void appendInventoryHistory(vector<InventoryHistoryPoint>& history, const InventoryHistoryPoint& point,
                            size_t maxEntries = 180);

class InventoryStore {
 public:
  vector<InventoryItem>& items();
  const vector<InventoryItem>& items() const;
  vector<HimsRack>& racks();
  const vector<HimsRack>& racks() const;

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
  vector<HimsRack> racks_;
};

string rackAssignmentModeName(RackAssignmentMode mode);
RackAssignmentMode parseRackAssignmentMode(const string& value);
string rackLocation(const InventoryItem& item, const vector<HimsRack>& racks);
bool isValidRackSlot(const string& slot);
bool reconcileRackAssignment(InventoryStore& store, InventoryItem& item);
bool reconcileRackAssignments(InventoryStore& store);
bool setManualRackLocation(InventoryStore& store, InventoryItem& item, const string& value, string& error);
int rackNumberFromCode(const string& code);
string rackSlotLabel(int row, int column);
size_t rackOccupiedSlotCount(const InventoryStore& store, const HimsRack& rack);
InventoryItem* itemAtRackSlot(InventoryStore& store, const string& rackId, const string& slot);
const InventoryItem* itemAtRackSlot(const InventoryStore& store, const string& rackId, const string& slot);
bool moveItemToRackSlot(InventoryStore& store, InventoryItem& item, const HimsRack& rack, const string& slot,
                        string& error);
bool unassignItemFromRack(InventoryItem& item);
bool restoreAutomaticRackAssignment(InventoryStore& store, InventoryItem& item);

bool loadActivities(const filesystem::path& path, vector<ActivityEntry>& activities);
bool saveActivities(const filesystem::path& path, const vector<ActivityEntry>& activities);
void appendActivity(vector<ActivityEntry>& activities, const ActivityEntry& entry, size_t maxEntries = 100);
ActivityEntry makeActivity(string kind, string message);

ScanResolution resolveScanCode(InventoryStore& store, const string& rawCode);

string serializeItem(const InventoryItem& item);
bool deserializeItem(const string& line, InventoryItem& item);
string serializeActivity(const ActivityEntry& entry);
bool deserializeActivity(const string& line, ActivityEntry& entry);

}  // namespace hims

