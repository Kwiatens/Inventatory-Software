// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Durable storage for pinned BOM projects.

#pragma once

#include "core/bom/BomMatch.h"
#include "core/inventory/Inventory.h"

#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::map;
using std::optional;
using std::string;
using std::time_t;
using std::vector;

struct SqliteConnection;

// A pinned project keeps the BOM text rather than a file reference, so the
// analysis can be rebuilt against current stock without the original file.
struct BomProject {
  string id;
  string name;
  string sourcePath;
  int boards = 1;
  time_t createdAt = 0;
  time_t lastOpened = 0;
  time_t lastBuilt = 0;
  string bomText;
  map<string, string> overrides;   // bomLineKey -> chosen item id
  map<string, string> enrichment;  // bomLineKey -> suggested DigiKey part
};

bool loadBomProjects(const filesystem::path& databasePath, vector<BomProject>& projects);
bool saveBomProjects(const filesystem::path& databasePath, const vector<BomProject>& projects);
// Validate the persisted BOM rows without migrating or mutating the database.
// Backup validation uses this read-only seam before a bundle can be activated.
bool validateBomProjects(SqliteConnection& connection, string* error = nullptr);

// How a DigiKey suggestion lookup for one BOM line ended.
enum class BomLookupOutcome {
  Found,    // DigiKey returned a part
  NoMatch,  // DigiKey answered and has nothing for this line
  Failed,   // transport, credential, rate-limit or parse failure
};

// Stored in a project's enrichment map for a line DigiKey answered "no match" for.
inline constexpr const char* kBomEnrichmentNoMatch = "(no match)";

// Prefix of a stored suggestion whose supplier record fits its BOM line. A suggestion without the prefix was stored
// before verification existed, and is treated as unverified: shown and exported as "check before ordering".
inline constexpr const char* kBomEnrichmentVerified = "verified\x1e";

// Prefix of a stored suggestion that came from TME. It follows the verification prefix. A suggestion without it came
// from DigiKey, which was the only supplier before TME was asked as well.
inline constexpr const char* kBomEnrichmentTme = "tme\x1e";

// The enrichment entry a finished lookup may persist, or nullopt when nothing may be cached. A
// failure is never cached: it is transient, and a cached value would stop the line from being retried.
optional<string> bomEnrichmentEntry(BomLookupOutcome outcome, const string& suggestion, bool verified = false,
                                    bool fromTme = false);
// The part number a stored suggestion names, without the verification prefix. Empty for markers.
string bomEnrichmentPart(const string& stored);
// True when a stored suggestion was verified against its BOM line.
bool bomEnrichmentVerified(const string& stored);
// The supplier a stored suggestion names: "DigiKey" or "TME". Empty for markers and failures.
string bomEnrichmentSupplier(const string& stored);

// True when the project already holds an answer for the line and it need not be looked up again.
// Empty values and the "-" that earlier versions stored for every failure do not count.
bool bomEnrichmentCached(const map<string, string>& enrichment, const string& key);

// The text a shortage export may show for a stored entry: the suggested part, never a marker.
string bomEnrichmentExportText(const string& stored);

// The shortage list of an analysis as spreadsheet-ready CSV text (CRLF rows, formula-safe cells). The
// suggested part and its supplier come from `enrichment`; markers and failures are exported as empty cells.
struct BomShortageExport {
  string text;
  size_t rows = 0;
};
BomShortageExport buildBomShortageCsv(const BomAnalysis& analysis, const map<string, string>& enrichment);

}  // namespace inventatory
