// Inventatory - Hardware Inventory Management System
// Inventory store persistence and database-backed item loading.

#include "core/InventoryInternals.h"
#include "core/InventoryStorageInternal.h"
#include "core/InventorySqlite.h"
#include "core/InventoryVersionInternal.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

namespace inventatory {

using namespace std;


vector<InventoryItem>& InventoryStore::items() {
  return items_;
}

vector<InventatoryRack>& InventoryStore::racks() { return racks_; }

const vector<InventatoryRack>& InventoryStore::racks() const { return racks_; }

const vector<InventoryItem>& InventoryStore::items() const {
  return items_;
}

bool InventoryStore::load(const filesystem::path& path) {
#ifdef _WIN32
  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  if (!ensureInventatoryTableSchema(connection)) {
    return false;
  }
  if (!validateInventoryCommitHistory(connection, nullptr)) {
    return false;
  }
  vector<InventatoryRack> loadedRacks;
  if (!loadRacks(connection, loadedRacks)) {
    return false;
  }

  vector<InventoryItem> inventatoryItems;
  if (!loadItemsFromInventatoryTable(connection, inventatoryItems)) return false;

  InventoryStore loadedStore;
  loadedStore.items() = move(inventatoryItems);
  loadedStore.racks() = move(loadedRacks);

  ensureInventoryIdentifiers(loadedStore.items());
  if (!validateInventoryIdentifiers(loadedStore.items(), loadedStore.racks())) return false;
  reconcileRackAssignments(loadedStore);
  items_ = move(loadedStore.items());
  racks_ = move(loadedStore.racks());
  return true;
#else
  vector<InventoryItem> loadedItems;
  ifstream file(path);
  if (!file) {
    return false;
  }

  string line;
  while (getline(file, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    InventoryItem item;
    if (deserializeItem(line, item)) {
      loadedItems.push_back(move(item));
    }
  }

  ensureInventoryIdentifiers(loadedItems);
  if (loadedItems.empty()) return false;
  items_ = move(loadedItems);
  racks_.clear();
  return true;
#endif
}

bool InventoryStore::save(const filesystem::path& path) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;

  SqliteConnection connection;
  if (!openDatabase(path, connection)) {
    return false;
  }

  return writeItemsToInventatoryTable(connection, items, racks_);
#else
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;

  filesystem::create_directories(path.parent_path());

  ofstream file(path, ios::trunc);
  if (!file) {
    return false;
  }

  file << "# Inventatory inventory data\n";
  for (const auto& item : items) {
    file << serializeItem(item) << '\n';
  }
  return true;
#endif
}

bool InventoryStore::saveWithMovements(const filesystem::path& path,
                                       const vector<InventoryMovement>& movements) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;
  SqliteConnection connection;
  if (!openDatabase(path, connection)) return false;
  return writeItemsToInventatoryTable(connection, items, racks_, nullptr, &movements);
#else
  (void)movements;
  return save(path);
#endif
}

bool InventoryStore::saveWithDeviceEvent(const filesystem::path& path, const DeviceEventCommit& event,
                                          const vector<InventoryMovement>& movements) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  if (!validateInventoryIdentifiers(items, racks_)) return false;
  SqliteConnection connection;
  if (!openDatabase(path, connection)) return false;
  return writeItemsToInventatoryTable(connection, items, racks_, &event, &movements);
#else
  (void)event;
  (void)movements;
  return save(path);
#endif
}

bool InventoryStore::saveWithCommit(const filesystem::path& path, const InventoryStore& previous,
                                    const InventoryCommitDraft& draft, const vector<InventoryMovement>& movements,
                                    const DeviceEventCommit* deviceEvent, InventoryCommit* committed) const {
#ifdef _WIN32
  auto items = items_;
  ensureInventoryIdentifiers(items);
  InventoryStore normalized;
  normalized.items() = items;
  normalized.racks() = racks_;
  if (!validateInventoryIdentifiers(normalized.items(), normalized.racks())) return false;
  InventoryStore normalizedPrevious = previous;
  ensureInventoryIdentifiers(normalizedPrevious.items());
  if (!validateInventoryIdentifiers(normalizedPrevious.items(), normalizedPrevious.racks())) return false;
  const auto changes = inventoryCommitDiff(normalizedPrevious, normalized);
  InventoryCommitDraft enriched = draft;
  enriched.changedItemCount = 0;
  enriched.changedRackCount = 0;
  unordered_set<string> changedItems;
  unordered_set<string> changedRacks;
  for (const auto& change : changes) {
    if (change.entityType == "item") changedItems.insert(change.entityId);
    if (change.entityType == "rack") changedRacks.insert(change.entityId);
  }
  enriched.changedItemCount = changedItems.size();
  enriched.changedRackCount = changedRacks.size();
  if (trim(enriched.message).empty()) {
    string operation = "Updated inventory";
    if (enriched.source == "import") operation = "Imported inventory";
    else if (enriched.source == "stocktake") operation = "Completed stocktake";
    else if (enriched.source == "bom_build") operation = "Built project inventory";
    else if (enriched.source == "scanner") operation = "Recorded scanner event";
    else if (enriched.source == "digikey") operation = "Applied DigiKey enrichment";
    else if (enriched.source == "undo") operation = "Undid inventory commit";
    else if (enriched.source == "revert") operation = "Corrected inventory history";
    else if (enriched.source == "checkpoint") operation = "Inventory checkpoint";
    enriched.message = operation;
    if (!enriched.reference.empty()) enriched.message += " [" + enriched.reference + "]";
    enriched.message += " · " + to_string(enriched.changedItemCount) +
                        (enriched.changedItemCount == 1 ? " part, " : " parts, ") +
                        to_string(enriched.changedRackCount) +
                        (enriched.changedRackCount == 1 ? " rack" : " racks");
  }

  const bool shouldCommit = !changes.empty() || draft.checkpoint;
  SqliteConnection connection;
  if (!openDatabase(path, connection)) return false;
  return writeItemsToInventatoryTable(connection, items, racks_, deviceEvent, &movements,
                                      shouldCommit ? &enriched : nullptr, committed, true,
                                      &normalizedPrevious.items(), &normalizedPrevious.racks());
#else
  (void)previous;
  (void)draft;
  (void)movements;
  (void)deviceEvent;
  (void)committed;
  return save(path);
#endif
}

vector<InventoryMovement> loadInventoryMovements(const filesystem::path& path, size_t limit) {
#ifdef _WIN32
  if (limit == 0) return {};
  error_code filesystemError;
  if (!filesystem::is_regular_file(path, filesystemError) || filesystemError) return {};
  SqliteConnection connection;
  if (!openDatabase(path, connection) || !tableExists(connection, "inventatory_stock_movements")) return {};

  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT movement_id, item_id, item_name, source, reference,
           quantity_before, delta, quantity_after, occurred_at
    FROM inventatory_stock_movements
    ORDER BY occurred_at DESC, movement_id DESC
    LIMIT ?
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) return {};
  const auto boundedLimit = min(limit, static_cast<size_t>(numeric_limits<int>::max()));
  sqliteApi().bind_int(statement.stmt, 1, static_cast<int>(boundedLimit));

  vector<InventoryMovement> movements;
  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    InventoryMovement movement;
    movement.id = sqliteText(statement.stmt, 0);
    movement.itemId = sqliteText(statement.stmt, 1);
    movement.itemName = sqliteText(statement.stmt, 2);
    movement.source = sqliteText(statement.stmt, 3);
    movement.reference = sqliteText(statement.stmt, 4);
    if (!sqliteInt32(statement.stmt, 5, movement.quantityBefore) || !sqliteInt32(statement.stmt, 6, movement.delta) ||
        !sqliteInt32(statement.stmt, 7, movement.quantityAfter) || !sqliteTime(statement.stmt, 8, movement.occurredAt)) {
      return {};
    }
    movements.push_back(move(movement));
  }
  return stepResult == SQLITE_DONE ? movements : vector<InventoryMovement>();
#else
  (void)path;
  (void)limit;
  return {};
#endif
}

InventoryItem* InventoryStore::findById(const string& id) {
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return item.id == id;
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findById(const string& id) const {
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return item.id == id;
  });
  return it == items_.end() ? nullptr : &(*it);
}

InventoryItem* InventoryStore::findByCode(const string& code) {
  const auto needle = toLower(trim(code));
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return toLower(item.id) == needle || toLower(item.inventatoryId) == needle || toLower(item.sku) == needle ||
           toLower(item.machineCode) == needle || toLower(item.digikeyPartNumber) == needle ||
           containsInsensitive(item.productUrl, needle) || containsInsensitive(item.datasheetUrl, needle);
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findByCode(const string& code) const {
  const auto needle = toLower(trim(code));
  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return toLower(item.id) == needle || toLower(item.inventatoryId) == needle || toLower(item.sku) == needle ||
           toLower(item.machineCode) == needle || toLower(item.digikeyPartNumber) == needle ||
           containsInsensitive(item.productUrl, needle) || containsInsensitive(item.datasheetUrl, needle);
  });
  return it == items_.end() ? nullptr : &(*it);
}

InventoryItem* InventoryStore::findByMachineCode(const string& machineCode) {
  const auto needle = normalizeMachineCode(machineCode);
  if (needle.empty()) {
    return nullptr;
  }

  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return normalizeMachineCode(item.machineCode) == needle;
  });
  return it == items_.end() ? nullptr : &(*it);
}

const InventoryItem* InventoryStore::findByMachineCode(const string& machineCode) const {
  const auto needle = normalizeMachineCode(machineCode);
  if (needle.empty()) {
    return nullptr;
  }

  const auto it = find_if(items_.begin(), items_.end(), [&](const InventoryItem& item) {
    return normalizeMachineCode(item.machineCode) == needle;
  });
  return it == items_.end() ? nullptr : &(*it);
}

}  // namespace inventatory
