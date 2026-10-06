# Slice D+F: SQLite storage/history and inventory domain logic

## Coverage
Read in full: src/core/storage/{InventorySqlite.h,.cpp, InventorySqliteSchema.cpp, InventoryStorage.cpp,
InventoryStorageSqlite.cpp, AtomicFile.cpp}; src/core/history/{InventoryVersion.cpp, InventoryVersionHistory.cpp,
InventoryVersionDiff.cpp, InventoryHistory.cpp}; src/app/persistence/{AppPersistence.cpp, AppHistoryPersistence.cpp};
src/app/workspace/AppWorkspace.cpp; src/core/inventory/{Inventory.h, InventoryHelpers.cpp, InventoryIdentifiers.cpp,
InventorySerialization.cpp, InventoryScan.cpp, InventoryMovements.cpp}; src/core/query/{InventoryQuery.cpp,
InventoryQueryMatching.cpp}; src/core/parts/{PhysicalValue.cpp, DecimalParse.h, PartDescriptor.cpp,
PartDescriptorContext.cpp}; src/core/racks/RackAllocation.cpp; src/core/bom/{BomMatch.cpp, BomMatchHelpers.cpp,
BomProjectStore.cpp}; src/app/inventory/{AppInventoryEdit.cpp, AppInventorySelection.cpp};
src/app/racks/{AppRackActions.cpp, AppRackPlacementActions.cpp (lines 1-145)}.
Also read (outside slice, for tracing callers): src/app/scanner/AppDeviceActions.cpp (processDeviceSyncEvents),
src/core/scanner/DeviceSyncStore.cpp (completeDeviceSyncEvent), src/app/common/AppActionSupport.h
(mergeDigiKeyMetadata), src/ui/pages/stock/StockPageInput.cpp (inline reload), src/app/shell/AppInput.cpp:131-134.
Skipped: PartDescriptorRules.cpp (393 lines of label rules; only grepped for non-ASCII fragments and unchecked
substr/index use, none found); small private headers; AtomicFile.h.
Verified empirically (standalone compile in scratchpad against PhysicalValue.cpp): the 4u7/4n7/M3/U1 results in
finding 4.
Locale: main.cpp calls setlocale(LC_ALL,"") but never std::locale::global, so iostream parsing in this slice
(quoted()/operator>>) stays classic; ctype calls are all unsigned-char-cast; I found no new locale bug here.
Architecture note used throughout: Windows tray process and the Linux systemd service are the *same* App binary and
are mutually exclusive with the foreground UI (main.cpp takeOverFromBackgroundService + single-instance lock), so
the "two processes writing one DB" scenario is mostly excluded. The remaining concurrency is inside one process
(UI thread vs scanner events processed on the UI loop vs HTTP worker threads that open the DB read/write).

## Findings

### [S2] Scanner-event commit uses the in-memory store as diff baseline; after a failed save it writes a commit whose change counts contradict its snapshots, which makes history validation fail ("recovery required")
- Location: src/app/scanner/AppDeviceActions.cpp:214 ; src/core/scanner/DeviceSyncStore.cpp:410-418 ; src/core/history/InventoryVersionHistory.cpp:236-245 ; src/core/storage/InventoryStorage.cpp:155-203
- Category: persistence
- Failure scenario: A normal edit fails to save (e.g. "database is locked" after the 3 s busy timeout, disk full). `store_` now holds the edit, `persistedStore_` does not (AppPersistence.cpp:262-267 keeps it in memory for retry). Before the user presses R, a Scan R1 event arrives. `processDeviceSyncEvents` copies `store_` (including the unsaved edit), applies the scan, and calls `completeDeviceSyncEvent(candidate, ..., &store_)`, so the `previous` argument of `saveWithCommit` is the unsaved in-memory store, not the last committed snapshot. `saveWithCommit` counts `changedItemCount` from previous->candidate (1 item), but the snapshot it stores contains the unsaved edit too. `validateInventoryCommitHistory` recomputes counts from the stored parent/child snapshots (2 items) and fails with "Inventory commit change counts do not match its snapshots". `refreshInventoryCommits()` runs immediately after (AppDeviceActions.cpp:222), calls `loadInventoryCommits` -> validation fails -> `inventoryRecoveryRequired_ = true`, so every later save is refused, and the next launch refuses to load the workspace (`InventoryStore::load` calls `validateInventoryCommitHistory`, InventoryStorage.cpp:43). Also `persistedStore_ = store_` is then set (AppDeviceActions.cpp:221), so the pending commit draft for the unsaved edit is silently dropped and its movements are never recorded.
- Evidence:
  ```
  if (!completeDeviceSyncEvent(candidate, context->paths.inventory, result, &store_)) {   // AppDeviceActions.cpp:214
  const auto& movementBaseline = previousStore == nullptr ? persistedStore : *previousStore; // DeviceSyncStore.cpp:410
  if (commit.changedItemCount != changedItems.size() || ...) return fail("Inventory commit change counts do not match its snapshots"); // VersionHistory.cpp:244
  ```
- Confidence: medium (needs a pending failed save, and I did not run it; the code path is unambiguous).
- Proposed test: build a store, commit it, mutate item A in memory without saving (persistedStore stays old), call `completeDeviceSyncEvent` for item B with `previousStore=&store`, then assert `loadInventoryCommits` still succeeds and the last commit's counts equal the snapshot diff.
- Proposed fix (sketch): have the app pass `persistedStore_` (last durable state) as the baseline, or refuse to process device events while `hasPendingPersistence()`; independently, make `saveWithCommit` derive counts from the DB's latest snapshot inside the same BEGIN IMMEDIATE transaction instead of trusting the caller's `previous`.

### [S2] Editing a part overwrites quantity changes applied by the scanner while the edit form was open (lost update)
- Location: src/app/inventory/AppInventoryEdit.cpp:89,217-219 ; src/app/scanner/AppDeviceActions.cpp:127-232
- Category: correctness
- Failure scenario: User opens Edit on part X (qty 10): `workingCopy_.item = *current` is a full copy. While the user types, a Scan R1 receive/adjust event for X is processed by the same event loop (`processDeviceSyncEvents` runs regardless of `inputMode_`) and X becomes qty 15 in `store_` and the DB. The user saves: `store_.items()[originalIndex] = workingCopy_.item` replaces the whole record with qty 10. `saveInventoryState` then diffs persistedStore_ (15) against store_ (10) and records a -5 "manual" movement. The scanned stock silently vanishes; the ledger just shows an apparent manual decrement.
- Evidence:
  ```
  workingCopy_.item = *current;                                   // :89
  store_.items()[workingCopy_.originalIndex] = workingCopy_.item; // :218
  ```
- Confidence: medium-high (device events are drained every loop; no version/lastUpdated check on save).
- Proposed test: App-level is hard; unit-level test the save helper: capture original, mutate store quantity, apply working-copy save, assert quantity change is preserved or the save is rejected with a conflict message.
- Proposed fix (sketch): remember the original item snapshot at begin-edit and on save apply only the fields the user actually changed (field-wise merge), or compare `lastUpdated`/serialized original and show "part changed while editing" instead of overwriting. Same applies to originalIndex being a raw vector index (also stale if the vector is reordered by undo/reload while editing).

### [S2] Every commit stores a full snapshot of all items; history validation loads the entire history into memory on every load, every save and every history navigation
- Location: src/core/history/InventoryVersion.cpp:91-99 ; src/core/history/InventoryVersionHistory.cpp:196-258 ; src/core/storage/InventoryStorage.cpp:43 ; src/app/persistence/AppPersistence.cpp:93-94,270-272 ; src/app/persistence/AppHistoryPersistence.cpp:24-42,58-75
- Category: perf / persistence
- Failure scenario: Each edit, scan event or quantity change writes one `inventatory_inventory_commit_items` row per item (full `serializeItem`, incl. vendor description/parameters, ~1-3 KB). With 2,000 parts and 100 commits/day the DB grows by ~200 MB/day with no pruning or compaction. Worse, `validateInventoryCommitHistory` (called from `InventoryStore::load`, `loadInventoryCommits`, and therefore after every save via `refreshInventoryCommits`) reads every commit, strictly deserializes every item of every snapshot, keeps all of them simultaneously in `vector<InventoryStore> snapshots`, and diffs each adjacent pair. Memory and time are O(commits x items) on the UI thread: at 500 commits x 2,000 items that is 1M item deserializations (via istringstream+quoted) and about 1.5 GB of resident snapshots before the app is usable, and the same cost again after every save. `loadInventoryCommit` (each arrow key in the History page, HistoryPage.cpp:236/248) additionally opens the DB, re-runs the full schema/data validation (finding below), and deserializes two full snapshots.
- Evidence:
  ```
  vector<InventoryStore> snapshots;  ...  snapshots.push_back(move(snapshot));   // VersionHistory.cpp:204,223
  for (const auto& item : items) { const auto data = serializeItem(item); ... INSERT ... }  // Version.cpp:91-99
  ```
- Confidence: high on the O(K x N) behaviour; actual thresholds depend on data size.
- Proposed test: generate 300 commits over 1,000 items in a temp DB and assert `loadInventoryCommits` completes under a bound and peak memory stays flat (validate streaming); add a test for a compaction/prune API once it exists.
- Proposed fix (sketch): validate incrementally (keep only previous+current snapshot while iterating), store per-commit deltas (or snapshot only changed rows plus periodic checkpoints), validate full history only on restore/backup, and page the commit list. At minimum stop calling full validation after our own successful save.

### [S2] "4u7"/"4n7" are classified as resistances, and "M3"/"U1"/"K4"/"G1" parse as resistor values, so searches return the wrong parts
- Location: src/core/parts/PhysicalValue.cpp:212-266 (parseRkmValue), 243-266 ; used by src/core/query/InventoryQueryMatching.cpp:137-148,202-230,305-309
- Category: correctness
- Failure scenario (verified by compiling PhysicalValue.cpp): `parsePhysicalValue("4u7")` -> Resistance 4.7e-6, `"4n7"` -> Resistance 4.7e-9, `"M3"` -> Resistance 300000, `"U1"` -> Resistance 1e-7. (a) A user searching the common capacitor notation "4u7" or "4n7" never matches a 4.7uF/4.7nF capacitor (type mismatch) and the token also short-circuits to a physical comparison instead of a text match. (b) Searching "M3" (screw size, uppercase is kept because the query uses `rawToken`) ranks every 300 kOhm resistor as an Exact match (`comparePhysicalValues("300 kOhms","M3")` band = Exact) and the text fallback is skipped because `physicalMatch.has_value()` -> `continue`. Same for M2/M4/M5/M6/M8, K-prefixed or G-prefixed codes, U-numbered designators. (c) `partNamePhysicalComparison` applies the same parse to every word of every part name, so "M3 Standoff" items rank as 300 kOhm exact matches for a resistor search of "300k".
- Evidence:
  ```
  const bool knownMultiplier = parsePrefix(marker, multiplier);   // p n u m k g accepted as RKM markers
  ...
  return PhysicalValue{value * multiplier, PhysicalValueType::Resistance};
  ```
- Confidence: high (executed).
- Proposed test: assert `parsePhysicalValue("4u7")` is Capacitance 4.7e-6 and "4n7" is Capacitance 4.7e-9, "4p7" Capacitance, and that "M3", "U1", "K4" return nullopt (or a text-only match); assert `filterItems(items,"M3")` finds an item named "M3x8 screw" and not a 300k resistor.
- Proposed fix (sketch): RKM letters are type-specific: R/K/M/G/T -> resistance, p/n/u -> capacitance, and H-family for inductance; require the whole token to be digits+marker+digits and at least 2 significant chars before the marker, and do not let an RKM-only parse of a bare designator-looking token suppress the substring fallback.

### [S2] BOM line matching produces false candidates from package codes and bare numbers in part names (chip codes collide with E96 resistor values)
- Location: src/core/bom/BomMatchHelpers.cpp:184-221 (itemValueFor free-text fallback), 251-278 (scoreItem), src/core/bom/BomMatch.cpp:99-123
- Category: correctness
- Failure scenario: For a Resistance BOM line, `itemValueFor` falls back to parsing every token of `partName + notes` with `parseElectricalValue`. A bare number with no unit is accepted as ohms. Package codes are bare numbers: "0603" -> 603 ohm, "0402" -> 402, "1206" -> 1206, "2010" -> 2010, "0805" -> 805. These sit within the 1% tolerance of real E96 values: 604 ohm (0.17%), 402 ohm, 1.21 kohm (0.33%), 806 ohm (0.12%), 2 kohm, 2.49 kohm. A capacitor/LED/diode named "CAP CER 100NF 16V X7R 0603" therefore becomes a candidate for a BOM line "604" (0603). `packageMatches("0603","0603")` yields score 90, the same as a genuine resistor, and ties are broken by larger stock, so the build screen can auto-select the capacitor and then "deduct" it. Any other bare-number token ("10" in "CONN HEADER 10 POS") matches a 10 ohm BOM line with score 70 when the package is empty/unknown.
- Evidence:
  ```
  for (const auto& token : tokenizeQuery(item.partName + " " + item.notes)) {
    ... parseElectricalValue(token, parsedKind) ... parsedKind == kind   // kind Resistance accepts a bare "0603"
  ```
- Confidence: medium-high (logic traced end to end; not executed because BomMatch pulls in UI headers).
- Proposed test: item "CAP CER 100NF 16V X7R 0603" (no Resistance parameter) against BOM line {designation "604", footprint "R_0603_1608Metric"} must yield no candidate; likewise item "CONN HEADER 10 POS" vs line "10".
- Proposed fix (sketch): in the free-text fallback require an explicit unit/prefix (no unit-less numbers, or only accept a unitless number when the item's category says resistor), and exclude tokens that equal a chip code in `kChipCodes`.

### [S2] Scanner-created placeholder parts are permanently marked "Unassigned" and never get an automatic rack slot after DigiKey enrichment
- Location: src/core/racks/RackAllocation.cpp:178-189 ; src/core/inventory/InventoryScan.cpp:35-38 ; src/app/common/AppActionSupport.h:150 (assignIfUseful category) ; src/app/scanner/AppDeviceActions.cpp:197
- Category: correctness
- Failure scenario: A scan creates a placeholder with category "Unsorted". `reconcileRackAssignment` finds no component type and rewrites the item to `RackAssignmentMode::Unassigned` (the same value used for "user intentionally unassigned"). Later `mergeDigiKeyMetadata` replaces category "Unsorted" with e.g. "Capacitors". `saveInventoryState` only reconciles items whose mode is `Automatic` (line 179 early return), so the item is never racked until the user runs the manual "AUTO" action per item. Items from the Scan R1 flow, the primary auto-intake path, silently end up outside the rack system.
- Evidence:
  ```
  if (!componentType) {
    ...
    item.rackAssignment = RackAssignmentMode::Unassigned;     // RackAllocation.cpp:187
  assignIfUseful(item.category, details.categoryName, true);   // replaces "Unsorted"; no rack re-evaluation
  ```
- Confidence: medium (there may be intent to keep "unclassifiable" parts unassigned, but the flag conflates classifier-failure with user choice; no test covers the scan->enrich->rack sequence).
- Proposed test: resolveScanCode creates item, reconcile, then apply an enrichment that sets category "Resistors" + SMD package, run `reconcileRackAssignments`, expect a rack slot.
- Proposed fix (sketch): do not persist `Unassigned` from the classifier (leave it `Automatic` with empty rack), or call `restoreAutomaticRackAssignment` from the enrichment merge when category/vendor data changed on an item that was auto-unassigned.

### [S3] "r" (reload) in Stock silently discards unsaved in-memory changes that "Press R to retry" is supposed to preserve
- Location: src/app/persistence/AppPersistence.cpp:158-217 ; src/ui/pages/stock/StockPageInput.cpp:207-240 ; src/ui/ActionRegistry.cpp:127-140
- Category: persistence
- Failure scenario: After a failed save the banner says "changes remain in memory. Press R to retry". Lowercase `r` is the reload action and uppercase `R` is retry. A user pressing the wrong case runs `reloadInventoryState`, which does `store_ = move(loadedStore); persistedStore_ = store_; persistenceError_.clear()` with no check of `persistedStore_ != store_` or `hasPendingPersistence()`. The unsaved edits are gone; `pendingCommitDraftValid_` stays true so the app still believes something is pending. Undo (AppInventorySelection.cpp:29) and History (AppHistoryPersistence.cpp:100) do guard against this ("Save the current inventory before..."); reload does not.
- Evidence:
  ```
  store_ = move(loadedStore);
  persistedStore_ = store_;
  persistedStoreValid_ = true;      // AppPersistence.cpp:199-201, no unsaved-changes guard
  ```
- Confidence: medium-high.
- Proposed test: unit-level on a small helper: with `inventoryCommitDiff(persistedStore_, store_)` non-empty, reload must refuse (or ask) and keep `store_`.
- Proposed fix (sketch): reuse the guard from undo/History, clear `pendingCommitDraft_` when a reload is confirmed, and delete the duplicated inline reload in StockPageInput.cpp.

### [S3] Startup save normalizes the DB without a commit, leaving the latest snapshot out of sync with the table; the next commit can then fail history validation
- Location: src/app/persistence/AppPersistence.cpp:132-134,151-153 ; src/core/storage/InventoryStorage.cpp:58-60 ; src/core/inventory/InventoryIdentifiers.cpp:254-320 ; src/core/racks/RackAllocation.cpp:240-244
- Category: persistence
- Failure scenario: `InventoryStore::load` runs `ensureInventoryIdentifiers` and `reconcileRackAssignments` (which can assign Inventatory IDs/machine codes, normalize slot case, even create racks). `loadState` then calls `store_.save()` (full rewrite, no commit) and sets `persistedStore_ = store_`. The last commit snapshot still holds the pre-normalization data. The next user edit commits with counts computed from `persistedStore_` (1 item) while the snapshot diff against the previous commit also contains all normalized items, so `validateInventoryCommitHistory` rejects the history (same failure mode as the first finding). Reachable with any DB whose rows lack IDs/rack data (older database, manual SQL, restore from a bundle created before normalization rules changed). Also: this save rewrites every row on every launch even when nothing changed (AGENTS "files rewritten when unchanged").
- Evidence:
  ```
  inventorySaved = store_.save(inventoryPath_);     // AppPersistence.cpp:133 (always, no change check)
  persistedStore_ = store_;                          // :152
  ```
- Confidence: low-medium (requires un-normalized rows; I did not construct one).
- Proposed test: insert an item row with empty `inventatory_id`/`machine_code` into a DB that already has commits, run load+save, then `saveWithCommit` an edit and assert `loadInventoryCommits` succeeds.
- Proposed fix (sketch): skip the startup save when `inventoryCommitDiff(loaded-from-db, normalized)` is empty; when normalization does change data, write it as an explicit "system" commit.

### [S3] Rack auto-allocation is cubic in the number of parts (slot occupancy re-scans all items for every slot of every rack)
- Location: src/core/racks/RackAllocation.cpp:113-119,135-145,198-214 ; called from AppPersistence.cpp:225 and InventoryStorage.cpp:60
- Category: perf
- Failure scenario: Importing N parts that all land in `Automatic` mode runs `reconcileRackAssignments` once. For each item it walks every compatible rack and every slot, and each `slotOccupied` is an `any_of` over all N items. With k full racks an item costs roughly 25*k*N/2 comparisons; summed over the batch this is about N^3/4. At 3,000 parts (120 full racks) that is ~7e9 string-compare operations, i.e. tens of seconds with the UI frozen (the save is synchronous); at 1,000 parts ~1 second. It also happens on load for any Automatic rows.
- Evidence:
  ```
  return any_of(store.items().begin(), store.items().end(), ...candidate.rackId == rackId && toUpper(trim(candidate.rackSlot)) == normalizedSlot);
  for (auto* rack : compatible) { const auto slot = firstFreeSlot(store, *rack, &item); ...
  ```
- Confidence: medium (algorithmic; not benchmarked).
- Proposed test: benchmark-style test placing 2,000 SMD parts and asserting it completes within a few hundred ms and produces the same slots as today.
- Proposed fix (sketch): build `unordered_set<rackId \x1f slot>` once in `reconcileRackAssignments`, keep a per-rack "first free" cursor, and update the set as you assign.

### [S3] Every storage entry point re-runs the full schema + data-value validation (table scans) before doing its work
- Location: src/core/storage/InventorySqliteSchema.cpp:316-339 (ensureInventoryDatabaseSchema -> validateCurrentSchema -> validateDataValues) ; callers: InventoryStorageSqlite.cpp:143 (every save), InventoryVersion.cpp:20-22, BomProjectStore.cpp:75-77, InventoryHistory.cpp:40-42
- Category: perf
- Failure scenario: An existing DB takes the `hasUserTables` branch which executes ~60 `PRAGMA table_info` queries plus `validateDataValues`: full scans of items, racks, `inventatory_stock_movements` (unbounded append-only ledger), device events, commits, and five `GROUP BY lower(trim(...))` duplicate checks. This happens before the BEGIN IMMEDIATE, on every save, every `loadBomProjects`, every history load; one edit triggers it ~3 times. Cost grows with the movements table (never pruned) and item count, on the UI thread. It is also outside the write transaction, so it protects nothing that the INSERTs do not already enforce through constraints.
- Evidence:
  ```
  if (hasUserTables) { if (version != kInventoryDatabaseSchemaVersion || !validateCurrentSchema(connection, error)) ...
  ```
- Confidence: high.
- Proposed test: with 200k movement rows assert that `ensureInventoryDatabaseSchema` on a valid DB returns in constant time (columns only), data validation reserved for `validateInventoryDatabase` (restore/backup).
- Proposed fix (sketch): split cheap structural check (user_version + required tables/columns, memoized per connection/process) from the expensive data validation, and call the latter only on load/restore/backup.

### [S3] sqlite3_open_v2 failure leaks the connection handle
- Location: src/core/storage/InventorySqlite.cpp:85-89,99-102
- Category: quality (resource leak on error path)
- Failure scenario: SQLite allocates a handle even when open fails (e.g. SQLITE_CANTOPEN for a read-only data dir, SQLITE_NOTADB is later); the documented contract requires `sqlite3_close()` on it. The code sets `connection.db = nullptr` without closing, so `SqliteConnection::~SqliteConnection` has nothing to close. A UI that retries (Press R on a read-only or unmounted data directory, device-event lookups) leaks a handle per attempt.
- Evidence:
  ```
  if (api.open_v2(utf8Path.c_str(), &connection.db, ...) != SQLITE_OK) { connection.db = nullptr; return false; }
  ```
- Confidence: high.
- Proposed test: open a path in a non-writable directory 1000 times under a leak/handle counter.
- Proposed fix (sketch): leave `connection.db` set so the destructor closes it (or call `api.close` before nulling).

### [S3] Applying an unreadable path throws: filesystem::exists() used without error_code in the folder picker
- Location: src/app/workspace/AppWorkspace.cpp:42,88
- Category: linux-portability
- Failure scenario: `filesystem::exists(selectedPath / "manifest.tsv")` and `filesystem::exists(selectedInventoryPath)` use the throwing overloads. On Linux selecting a directory whose child cannot be stat'ed (EACCES on a dir without x permission, ELOOP, a stale network mount returning EIO) raises `filesystem_error`, which is not caught here and terminates the UI. Neighbouring code in the same function correctly uses the `error_code` overload.
- Evidence:
  ```
  if (filesystem::exists(selectedPath / "manifest.tsv")) {          // :42
  if (filesystem::exists(selectedInventoryPath)) {                    // :88
  ```
- Confidence: medium (exact errno set depends on libstdc++ version; exception type is standard-mandated).
- Proposed test: call with a directory chmod 000 under a temp dir (skip when root).
- Proposed fix (sketch): use `error_code` overloads and report "cannot inspect folder".

### [S3] findByCode matches scanned codes as substrings of product/datasheet URLs
- Location: src/core/storage/InventoryStorage.cpp:271-289 ; used by src/core/inventory/InventoryScan.cpp:29
- Category: correctness
- Failure scenario: Any non-numeric, non-"Inventatory:" scan code is looked up with `containsInsensitive(item.productUrl, needle) || containsInsensitive(item.datasheetUrl, needle)`. A short or generic scanned token (e.g. "TE", "SMD", a 3-5 character vendor code, "datasheet") matches the first item whose URL happens to contain that substring and resolves the scan to the wrong existing part (stock is added to it) instead of creating a placeholder. The whole method also lowercases both operands per item per field (many allocations) inside a linear scan.
- Evidence:
  ```
  containsInsensitive(item.productUrl, needle) || containsInsensitive(item.datasheetUrl, needle)
  ```
- Confidence: medium.
- Proposed test: store with item URL ".../en/products/detail/te-connectivity/..." and scan "te"; expect a new placeholder, not the existing item.
- Proposed fix (sketch): match URLs only when the code is a full URL, or compare the final path segment exactly; require a minimum length.

### [S3] Integer-overflow and quantity edge cases
- Location: src/app/racks/AppRackActions.cpp:362 ; :225 ; src/core/inventory/InventoryMovements.cpp:27 ; src/core/query/InventoryQueryMatching.cpp:150-173
- Category: correctness
- Failure scenario: (a) `adjustSelectedRackItemQuantity` does `max(0, item->quantity + delta)` in `int`; at quantity INT_MAX with delta>0 this is signed overflow (UB), whereas `adjustQuantity` (AppInventoryEdit.cpp:238) correctly widens to long long and clamps. (b) `createRackWithType` computes `rackNumberFromCode(rack.code) + 1`; a rack renamed to "R2147483647" (allowed by `renameSelectedRack`) makes this INT_MAX+1, UB; `reconcileRackAssignment` guards this, the UI path does not. (c) `inventoryMovementDiff` does `quantityAfter - quantityBefore` in `int`: DB validation allows negative quantities, so -5 -> INT_MAX overflows. (d) `qty>5abc`, `qty=1e3`, `qty> 7` go through `stoi`, which accepts trailing garbage and leading blanks, so "qty=1e3" means qty==1 (silently).
- Evidence:
  ```
  item->quantity = max(0, item->quantity + delta);
  nextNumber = max(nextNumber, rackNumberFromCode(rack.code) + 1);
  const int delta = quantityAfter - quantityBefore;
  return item.quantity >= stoi(token.substr(5));
  ```
- Confidence: high that the code is as quoted; impact low (hard to reach).
- Proposed test: quantity INT_MAX +1; rack "R2147483647" + create; qty tokens with garbage return no match.
- Proposed fix (sketch): do the arithmetic in long long/int64 with clamping (reuse one helper), parse qty with `from_chars` and full-consumption check.

### [S3] Duplicate (valid-format) Inventatory IDs are never repaired and make saving fail permanently
- Location: src/core/inventory/InventoryIdentifiers.cpp:285-302,322-334
- Category: persistence
- Failure scenario: `ensureInventoryIdentifiers` keeps any `inventatoryId` that `isInventatoryId` accepts, including duplicates (it only de-duplicates *generated* IDs and machine codes). If two items carry the same "Inventatory:R-00005" (CSV/legacy import, restored data, copy of an item), `validateInventoryIdentifiers` returns false, so `InventoryStore::save/saveWithCommit` return false every time and `load` returns false (workspace unreadable) with no repair path or specific message ("Could not save inventory").
- Evidence:
  ```
  if (!normalizedId.empty() && isInventatoryId(normalizedId)) { item.inventatoryId = normalizedId; }   // no uniqueness check
  const auto inventatoryId = toLower(trim(item.inventatoryId)); if (... !inventatoryIds.insert(...).second) return false;
  ```
- Confidence: medium (depends on an entry path that creates duplicates; I did not find a UI "duplicate part" command).
- Proposed test: two items with the same ID -> `ensureInventoryIdentifiers` then `validateInventoryIdentifiers` must be true and IDs distinct.
- Proposed fix (sketch): track seen IDs in the first loop and regenerate the second occurrence like machine codes.

### [S3] Activity log: a record the app writes can exceed the line-length limit the loader enforces (whole log then rejected)
- Location: src/core/inventory/InventorySerialization.cpp:24-27,386,393,402-414
- Category: persistence
- Failure scenario: `validActivity` allows a message up to 64 KiB, but `loadActivities` returns false for any physical line over 64 KiB. A 64 KiB message that contains quotes/backslashes (doubled by `std::quoted`) plus the timestamp/kind prefix serializes to more than 64 KiB, so `saveActivities` succeeds and the next launch treats the file as unreadable (`activityPersistenceBlocked_`, original preserved, history frozen). Long import/error messages are the likely source.
- Evidence:
  ```
  constexpr size_t kMaxActivityLineBytes = 64U * 1024U;  ... if (line.size() > kMaxActivityLineBytes) return false;
  ```
- Confidence: medium-low (needs a very long message).
- Proposed test: round trip an entry whose message is 64 KiB of `"` characters.
- Proposed fix (sketch): cap the message so the serialized line fits, or apply the same check in `saveActivities`.

### [S3] Physical value parser: overflow and Ohm-sign handling give wrong values instead of "no value"
- Location: src/core/parts/PhysicalValue.cpp:44-50,148 ; 143-162
- Category: correctness
- Failure scenario: `parseClassicDouble` returns 0.0 on any stream failure, including overflow ("1e999"), so `parsePhysicalValue("1e999 k")` yields Resistance 0 (verified), which compares Exact to a 0-ohm target. The unit check `unit == "\xCE\xA9"` accepts only exactly one Greek capital Omega and nothing after it: "10\xE2\x84\xA6" (U+2126 OHM SIGN, common in copy-pasted datasheets) and "10 \xCE\xA9 1%" return no value (verified). Since `unit[0]=='R'/'F'/'H'` is also accepted as a prefix test, "10 hours"/"5 ft"/"2 rad" parse as inductance/capacitance/resistance.
- Evidence:
  ```
  return stream.fail() ? 0.0 : value;
  if (unit == "\xCE\xA9" || startsWithInsensitive(unit, "ohm") || unit[0] == 'R' || unit[0] == 'r')
  ```
- Confidence: high (executed).
- Proposed test: parsePhysicalValue("1e999k") is nullopt; "10\xE2\x84\xA6" and "10 \xCE\xA9 1%" parse to 10 ohm; "5 hours" is nullopt.
- Proposed fix (sketch): return optional from `parseClassicDouble` and propagate; normalize U+2126 to U+03A9 and compare unit as a prefix; require the unit to match a full word.

### [S3] POSIX atomic write does not fsync the directory and treats EINTR as failure
- Location: src/core/storage/AtomicFile.cpp:126-132,206-212
- Category: linux-portability
- Failure scenario: Windows uses MOVEFILE_WRITE_THROUGH; the POSIX branch fsyncs the temp file but not the parent directory after `rename`, so after power loss the rename (and thus the new settings/activity/quick-label content) may be lost although the call reported success. `write()` returning -1/EINTR (signal from the Linux service, SIGCHLD from the printer helper) hits `written <= 0` and aborts the whole save with "Unable to write temporary file".
- Evidence:
  ```
  if (written <= 0) { setError(error, ...); success = false; break; }
  filesystem::rename(temporary, destination, filesystemError);    // no directory fsync
  ```
- Confidence: medium.
- Proposed test: not unit-testable for power loss; for EINTR use a signal-interrupt fixture or review-only.
- Proposed fix (sketch): retry on EINTR and `fsync` the parent directory descriptor after rename.

### [S3] Dangling rack pointer after saveState() in changeSelectedRackType
- Location: src/app/racks/AppRackActions.cpp:208-214
- Category: correctness
- Failure scenario: `rack` points into `store_.racks()`; `saveState()` -> `saveInventoryState` -> `reconcileRackAssignments` may `push_back` a new rack (any item still in `Automatic` mode), invalidating `rack`, and line 214 then reads `rack->code`. Needs an Automatic item at that moment (rare, but pending imports/scan events are such cases).
- Evidence:
  ```
  saveState();
  syncRackSelection();
  setMessage(rack->code + " type updated", 2);
  ```
- Confidence: low-medium.
- Proposed test: ASan run with an Automatic, unplaced part present while changing a rack type.
- Proposed fix (sketch): copy `rack->code` before `saveState()` (as `renameSelectedRack` already does with `code`).

## Test gaps
- Scanner event completion while `store_` differs from the last persisted snapshot (finding 1) -> assert history remains valid and counts match snapshot diff.
- Commit history scale: N commits x M items load/validate time and memory -> bounded-resource test or streaming validation test.
- `ensureInventoryDatabaseSchema` cost on a DB with many movement rows; open failure handle release.
- Failure path of `writeItemsToInventatoryTable` when a mid-way INSERT fails (e.g. duplicate rack code): assert ROLLBACK leaves DB byte-identical and the in-memory retry succeeds after the cause is fixed (only restore paths appear to be covered).
- Second connection holding BEGIN IMMEDIATE (busy_timeout 3 s): `save` must return false quickly, leave state intact, and a retry must succeed (test at tests line ~5855 covers restore only).
- loadState normalization: DB rows with empty `inventatory_id`/`machine_code`/Automatic rack mode followed by an edit commit.
- RKM/prefix parsing: "4u7", "4n7", "M3", "U1", "1e999k", U+2126 Ohm sign, "10 \xCE\xA9 1%".
- BOM: chip-code and bare-number tokens in part names vs E96 resistor values; multiple BOM lines sharing a part with overrides.
- `resolveScanCode`/`findByCode` with short generic codes against URLs; scan placeholder -> enrichment -> rack assignment.
- `qty` tokens with trailing garbage / exponent notation; int overflow in rack quantity/rack numbering/movement delta.
- Activity log round trip at the line-length limit; activity text with embedded `"` and `\`.
- Workspace switch to a folder with an unreadable child (Linux permissions).
- Edit-while-scanning lost update (field-wise merge or conflict detection).
- Reload while a retry-save is pending.

## Quality (S4)

### [S4] Per-keystroke search work is heavily redundant (re-tokenizing and re-parsing per item; searchableText allocation per item per token; sort comparator allocates)
- Location: src/core/query/InventoryQueryMatching.cpp:232-316 ; src/core/inventory/InventoryHelpers.cpp:85-109 ; src/app/inventory/AppInventorySelection.cpp:130-206
- Category: perf
- `evaluateQueryWithRack` calls `tokenizeQuery(query)`, `toLower`, `parsePhysicalValue(rawToken)` (several times) and `item.searchableText()` (ostringstream + ~40 appends + toLower) for every item and every token; `stockSearchMatches()` then sorts with a comparator that builds `toLower(displayCategory())`/`toLower(partName)` copies on every comparison. `selectedIndex()`, `selectedItem()`, `filteredIndices()`, `moveSelection()`, `syncSelectionToFilter()` and `currentActions()` (`hasItem = selectedItem() != nullptr`) each re-run the whole search, so one frame at 10k parts does the full scan many times. Fix: compile the query once (tokens + parsed physical needle), cache lowered searchable text per item (or per revision), cache the match vector keyed on (query, store revision, filters), pre-lower sort keys.

### [S4] Commit change listing and message templating are duplicated, and field-diff order is nondeterministic
- Location: src/core/storage/InventoryStorage.cpp:169-196 vs src/app/persistence/AppPersistence.cpp:231-253 ; src/core/history/InventoryVersionDiff.cpp:136-166
- `saveWithCommit` and `saveInventoryState` both compute changed-item sets and build the same "Updated inventory ... N parts, M racks" message. `inventoryCommitDiff` iterates `unordered_set` ids, so the field-change list shown in History differs run to run and between libstdc++/MSVC. Sort by id/label (or iterate the ordered inputs) and share one helper. `joinTags`/`joinParameters` in the same file use unescaped ',' / ';', so tag sets {"a,b"} and {"a","b"} compare equal in the diff but not in `serializeItem` (a change with no visible field diff, and therefore no commit if it is the only change).

### [S4] makeId() carries no entropy beyond the clock and its "random" half is a function of the timestamp
- Location: src/core/inventory/InventoryHelpers.cpp:152-160
- The second half is the first output of an `mt19937_64` seeded by the same `stamp`, so two calls with the same clock tick (coarse clocks, tests, two processes) collide on both halves; the first half is masked to 36 bits but printed with setw(10), and `resolveScanCode` further truncates to 8 chars (InventoryScan.cpp:35). IDs are PKs for items, racks, commits, movements. Use `std::random_device`/a process-wide generator plus an atomic counter.

### [S4] componentTypeFor/normalizeInventatoryPrefix use fragile substring heuristics
- Location: src/core/racks/RackAllocation.cpp:60-93 ; src/core/inventory/InventoryIdentifiers.cpp:35-45
- Substrings such as "sma" (matches "small signal"), "sop", "csp", "smb", "led " ("Shielded cable"), "ic " ("Magnetic", "Plastic enclosures" -> prefix "U" before the sensor rule), "fet" ("Safety"), "led" in the ID prefix ("Controlled ...") pick the wrong rack type or ID prefix. Use word-boundary/token matching; unit tests currently only cover the happy classes.

### [S4] Smaller items
- src/core/storage/InventorySqlite.cpp:206-209 empty anonymous namespace; unused local helpers noted by the owner are excluded as already known.
- src/core/bom/BomMatch.cpp:228-233 tie-break comparator does a linear `find_if` over all items per comparison (O(candidates log candidates x items) per BOM line); precompute quantities into the candidate struct.
- src/core/bom/BomMatchHelpers.cpp:135-137 treats the letter 'e' as an RKM decimal marker ("1e3" -> 1.3); scientific notation is silently misread.
- src/app/workspace/AppWorkspace.cpp:88-99 loads the candidate DB three times (validate, `candidate.load`, then `loadState`) and `candidate` is unused afterwards.
- src/core/racks/RackAllocation.cpp:121-133 clamps racks to 5x5 while schema/validation (InventorySqliteSchema.cpp:215, InventoryVersionHistory.cpp:39-51) accept up to 10000 rows/columns and letters beyond 'E'; the two limits should be one named constant.
- src/app/common/AppActionSupport.h:~170 `mergeDigiKeyMetadata` sets `changed = true` unconditionally after assigning `vendorMetadata`, so enrichment always reports a change.
- src/core/history/InventoryHistory.cpp: `saveInventoryHistory` deletes and re-inserts all points and is only reached from the reload actions (AppPersistence.cpp:191, StockPageInput.cpp:227); the history-point table is otherwise never appended during normal edits, so it is effectively stale data.
- src/core/storage/InventoryStorage.cpp:213-255 `loadInventoryMovements` opens the DB read/write (CREATE) for a pure read; use `openDatabaseReadOnly`.
- src/core/storage/InventoryStorage.cpp:33-63 `InventoryStore::load` reads validation, racks and items in separate statements without one read transaction (`BEGIN`/`COMMIT`), so a concurrent in-process writer (device-event or HTTP worker thread opening the DB) could yield racks from one state and items from another, which `reconcileRackAssignments` would then "fix" in memory.
