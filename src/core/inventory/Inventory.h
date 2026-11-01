// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/parts/PhysicalValue.h"

#include <ctime>
#include <cstdint>
#include <filesystem>
#include <optional>
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

// Provider-neutral catalog metadata. Labels use only the category and structured parameters,
// never vendor prose.
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
  // Low-stock level for this part. 0 uses AppSettings::lowStockThreshold.
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
  // Records from other providers. The selected source stays in vendorMetadata.
  vector<VendorProductMetadata> vendorSources;

  bool hasMissingMetadata() const;
  string searchableText() const;
};

// Sync status of a part added by hand. It has no vendor record to fetch, so it is never "unsynced".
inline constexpr const char* kManualSyncStatus = "manual";
bool needsVendorSync(const InventoryItem& item);
// True when the part came from, or names, a DigiKey, TME or Mouser product.
bool hasVendorRecord(const InventoryItem& item);

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

struct InventorySearchMatch {
  size_t itemIndex = 0;
  PhysicalValueMatchBand band = PhysicalValueMatchBand::None;
  double relativeDifference = 0.0;
  double signedRelativeDifference = 0.0;
  bool hasPhysicalComparison = false;
};

struct InventoryHistoryPoint {
  time_t timestamp = 0;
  size_t itemCount = 0;
  size_t totalUnits = 0;
  size_t lowStockCount = 0;
  size_t outOfStockCount = 0;
  size_t dataErrorCount = 0;
};

// Quantity deltas kept apart from the stock snapshot. Item fields are copied in, so history
// survives renames and deletes.
struct InventoryMovement {
  string id;
  string itemId;
  string itemName;
  string source;
  string reference;
  int quantityBefore = 0;
  int delta = 0;
  int quantityAfter = 0;
  time_t occurredAt = 0;
};

enum class InventoryRevertMode {
  Snapshot,
  Reverse,
};

struct InventoryCommitDraft {
  string source = "manual";
  string reference;
  string message;
  bool checkpoint = false;
  bool corrective = false;
  string revertedCommitId;
  size_t changedItemCount = 0;
  size_t changedRackCount = 0;
};

struct InventoryCommit {
  string id;
  string parentId;
  std::uint64_t sequence = 0;
  time_t timestamp = 0;
  string source;
  string reference;
  string message;
  bool checkpoint = false;
  bool corrective = false;
  string revertedCommitId;
  size_t changedItemCount = 0;
  size_t changedRackCount = 0;
};

struct InventoryFieldChange {
  string entityType;
  string entityId;
  string label;
  string field;
  string before;
  string after;
};

// Saved in the same transaction as the snapshot, so a retried sync event
// cannot repeat its stock effect.
struct DeviceEventCommit {
  string eventId;
  // Scanner that owns the inbox row. Completion must check it.
  string deviceId;
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
bool validateInventoryIdentifiers(const vector<InventoryItem>& items, const vector<InventatoryRack>& racks);
string join(const vector<string>& values, char delimiter);
vector<string> split(const string& value, char delimiter);
vector<string> tokenizeQuery(const string& query);

bool containsInsensitive(string_view haystack, string_view needle);
bool startsWithInsensitive(string_view value, string_view prefix);
bool endsWith(string_view value, string_view suffix);
int effectiveReorderThreshold(const InventoryItem& item, int globalThreshold);
bool isLowStock(const InventoryItem& item, int threshold);
bool matchesQuery(const InventoryItem& item, const string& query, int lowStockThreshold);
bool matchesQuery(const InventoryItem& item, const string& query, const vector<InventatoryRack>& racks,
                  int lowStockThreshold);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query, int lowStockThreshold);
vector<size_t> filterItems(const vector<InventoryItem>& items, const string& query,
                           const vector<InventatoryRack>& racks, int lowStockThreshold);
vector<InventorySearchMatch> rankedFilterItems(const vector<InventoryItem>& items, const string& query,
                                               const vector<InventatoryRack>& racks, int lowStockThreshold);
vector<InventorySearchMatch> findClosestPhysicalValues(const vector<InventoryItem>& items, const string& target);
Summary summarize(const vector<InventoryItem>& items, int lowStockThreshold);
InventoryHistoryPoint makeInventoryHistoryPoint(const vector<InventoryItem>& items, int lowStockThreshold,
                                                time_t timestamp = 0);

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

  // `normalized` reports whether loading repaired stored rows (identifiers, rack placement),
  // so the caller knows the database no longer matches memory.
  bool load(const filesystem::path& path, bool* normalized = nullptr);
  bool save(const filesystem::path& path) const;
  bool saveWithMovements(const filesystem::path& path, const vector<InventoryMovement>& movements) const;
  bool saveWithCommit(const filesystem::path& path, const InventoryStore& previous,
                      const InventoryCommitDraft& draft, const vector<InventoryMovement>& movements = {},
                      const DeviceEventCommit* deviceEvent = nullptr, InventoryCommit* committed = nullptr) const;

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

struct InventoryCommitDetail {
  InventoryCommit commit;
  bool hasParent = false;
  InventoryStore snapshot;
  InventoryStore parentSnapshot;
  vector<InventoryFieldChange> changes;
};

vector<InventoryFieldChange> inventoryCommitDiff(const InventoryStore& before, const InventoryStore& after);
// Counts distinct items and racks in `changes`. False if a change names another entity type.
bool countChangedEntities(const vector<InventoryFieldChange>& changes, size_t& itemCount, size_t& rackCount);
bool inventoryHasChanges(const InventoryStore& before, const InventoryStore& after);
bool prepareInventoryCommitReverse(const InventoryCommitDetail& detail, const InventoryStore& current,
                                   InventoryStore& reversed, string& conflict);
bool ensureInventoryCommitHistory(const filesystem::path& path, const InventoryStore& current);
// Lists commits newest first. Validates the whole history unless `validateHistory` is false, which
// is only for a caller that has just appended a commit and checks it itself.
bool loadInventoryCommits(const filesystem::path& path, vector<InventoryCommit>& commits,
                          bool validateHistory = true);
bool loadInventoryCommit(const filesystem::path& path, const string& id, InventoryCommitDetail& detail);

// Ctrl+Z restores the parent of the newest change that no later corrective commit has undone.
// `commits` is newest first. Returns nullptr when nothing is left to undo.
const InventoryCommit* latestUndoableInventoryCommit(const vector<InventoryCommit>& commits);

vector<InventoryMovement> inventoryMovementDiff(const InventoryStore& before, const InventoryStore& after,
                                               const string& source, const string& reference = {},
                                               time_t occurredAt = 0);
vector<InventoryMovement> loadInventoryMovements(const filesystem::path& path, size_t limit = 256);

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

// Storage: a part sits in a slot of a rack or organizer, at a named place, or nowhere yet.
// A rack's code is R<number> and its grid is at most 5x5. Any other code is an organizer: a
// user-named box with its own grid, up to 26 rows (A-Z) by 99 columns.
constexpr int kRackGridSize = 5;
// Organizers stay readable on screen and on a label sheet: at most 10 letters (A-J) by 10 numbers.
constexpr int kOrganizerMaxRows = 10;
constexpr int kOrganizerMaxColumns = 10;
constexpr size_t kStorageNameMaxLength = 40;
bool isOrganizer(const InventatoryRack& rack);
int storageGridRows(const InventatoryRack& rack);
int storageGridColumns(const InventatoryRack& rack);
bool storageHasSlot(const InventatoryRack& rack, const string& slot);
// `componentType` is optional: parts of that category are placed in the organizer by themselves.
bool createOrganizer(InventoryStore& store, const string& name, int rows, int columns, string& error,
                     const string& componentType = string());
// Reads an organizer size typed as rows x columns, as the Racks page shows it: numbered rows run
// down and lettered columns run across, so "4x6" is slots A1 to F4. `letters` and `numbers` are
// the stored row and column counts (a slot's letter is its stored row).
bool parseOrganizerSize(const string& text, int& letters, int& numbers);
// Checks an organizer name: not empty, not too long, not a rack code, unique. `exceptId` is the
// organizer being renamed.
bool validateStorageName(const InventoryStore& store, const string& name, const string& exceptId, string& error);

// Location texts that mean "not stored yet": blank, and the inboxes scans and imports write.
bool isPlaceholderLocation(const string& location);
// The slot ("R3-E3", "Red case-B4"), else the place text, else empty.
string storageLocation(const InventoryItem& item, const vector<InventatoryRack>& racks);

struct StorageChoice {
  enum class Kind { NotStored, Automatic, Slot, Place };
  Kind kind = Kind::NotStored;
  string rackId;
  string slot;
  string place;
  string display;          // what the field shows afterwards
  bool newPlace = false;   // no other part is at this place yet
  bool pickedFreeSlot = false;  // a storage name without a slot took its first free slot
};
// Reads what the user typed in the Storage field. `item` is the part being edited, so its own
// slot never counts as taken.
bool resolveStorageInput(const InventoryStore& store, const InventoryItem& item, const string& text,
                         StorageChoice& choice, string& error);
void applyStorageChoice(InventoryItem& item, const StorageChoice& choice);
// Splits "Tray2 A2" or "Tray2-A2" into the name and a slot such as A2. False when the text does
// not end with a slot.
bool splitTrailingSlot(const string& text, string& name, string& slot);
// Rack and organizer codes, then places in use, that start with or contain `typed`.
vector<string> storageSuggestions(const InventoryStore& store, const string& typed, size_t limit);
// Free slots of the rack or organizer with this code, in order. Empty when the code is unknown.
vector<string> freeStorageSlots(const InventoryStore& store, const string& code, const InventoryItem* except,
                                size_t limit);
const InventatoryRack* findStorageByCode(const InventoryStore& store, const string& code);

bool loadActivities(const filesystem::path& path, vector<ActivityEntry>& activities);
bool saveActivities(const filesystem::path& path, const vector<ActivityEntry>& activities);
void appendActivity(vector<ActivityEntry>& activities, const ActivityEntry& entry, size_t maxEntries = 100);
ActivityEntry makeActivity(string kind, string message);

ScanResolution resolveScanCode(InventoryStore& store, const string& rawCode);

// Fields of a TME bag label's QR text (PN, MPN, MFR, PO, QTY, product URL).
struct TmeLabelFields {
  string symbol;                  // PN: the TME symbol
  string manufacturerPartNumber;  // MPN
  string manufacturer;            // MFR, as printed on the label
  string orderNumber;             // PO
  string productUrl;              // the TME product link, when the label carries one
  int quantity = 0;               // QTY
};
// Needs PN and either MPN or MFR; other text gives nullopt.
std::optional<TmeLabelFields> parseTmeLabelPayload(const string& rawCode);

// TME symbol from the selected source or a TME record beside it; empty if none.
inline string tmeProductSymbol(const InventoryItem& item) {
  if (toLower(trim(item.vendorMetadata.provider)) == "tme" && !trim(item.vendorMetadata.providerProductNumber).empty()) {
    return trim(item.vendorMetadata.providerProductNumber);
  }
  for (const auto& source : item.vendorSources) {
    if (toLower(trim(source.provider)) == "tme" && !trim(source.providerProductNumber).empty()) {
      return trim(source.providerProductNumber);
    }
  }
  return {};
}

// Mouser part number from the selected source or a Mouser record beside it; empty if none.
inline string mouserPartNumberOf(const InventoryItem& item) {
  if (toLower(trim(item.vendorMetadata.provider)) == "mouser" &&
      !trim(item.vendorMetadata.providerProductNumber).empty()) {
    return trim(item.vendorMetadata.providerProductNumber);
  }
  for (const auto& source : item.vendorSources) {
    if (toLower(trim(source.provider)) == "mouser" && !trim(source.providerProductNumber).empty()) {
      return trim(source.providerProductNumber);
    }
  }
  return {};
}

// Mouser product page (ProductDetailUrl) of the record mouserPartNumberOf reads; empty if none.
inline string mouserProductUrlOf(const InventoryItem& item) {
  if (toLower(trim(item.vendorMetadata.provider)) == "mouser" &&
      !trim(item.vendorMetadata.providerProductNumber).empty()) {
    return trim(item.vendorMetadata.productUrl);
  }
  for (const auto& source : item.vendorSources) {
    if (toLower(trim(source.provider)) == "mouser" && !trim(source.providerProductNumber).empty()) {
      return trim(source.productUrl);
    }
  }
  return {};
}

string serializeItem(const InventoryItem& item);
bool deserializeItem(const string& line, InventoryItem& item);
// Strict read for commit snapshots: rejects truncated records, unknown trailing fields and
// malformed values. Records written before vendor sources existed read as having none.
bool deserializeItemStrict(const string& line, InventoryItem& item);
string serializeVendorSourcesForStorage(const vector<VendorProductMetadata>& sources);
vector<VendorProductMetadata> deserializeVendorSourcesFromStorage(const string& value);
bool deserializeVendorSourcesFromStorageStrict(const string& value, vector<VendorProductMetadata>& sources);
string serializeTagsForStorage(const vector<string>& tags);
vector<string> deserializeTagsFromStorage(const string& value);
bool deserializeTagsFromStorageStrict(const string& value, vector<string>& tags);
string serializeParametersForStorage(const vector<Parameter>& parameters);
vector<Parameter> deserializeParametersFromStorage(const string& value);
bool deserializeParametersFromStorageStrict(const string& value, vector<Parameter>& parameters);
string serializeActivity(const ActivityEntry& entry);
bool deserializeActivity(const string& line, ActivityEntry& entry);

}  // namespace inventatory
