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

// Keeps the BOM text, not a file reference, so the analysis can be rebuilt from current stock.
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
  map<string, string> enrichment;  // bomLineKey -> suggested supplier part, stored as bomEnrichmentEntry writes it
};

bool loadBomProjects(const filesystem::path& databasePath, vector<BomProject>& projects);
bool saveBomProjects(const filesystem::path& databasePath, const vector<BomProject>& projects);
// Read-only check of persisted BOM rows, used to validate a backup before activation.
bool validateBomProjects(SqliteConnection& connection, string* error = nullptr);

// How a supplier suggestion lookup for one BOM line ended.
enum class BomLookupOutcome {
  Found,    // a supplier returned a part
  NoMatch,  // the suppliers answered and have nothing for this line
  Failed,   // transport, credential, rate-limit or parse failure
};

// Enrichment value for a line the suppliers answered with no match.
inline constexpr const char* kBomEnrichmentNoMatch = "(no match)";

// Marks a suggestion whose supplier record fits its BOM line; without it, it is unverified.
inline constexpr const char* kBomEnrichmentVerified = "verified\x1e";

// Marks a suggestion from TME. No TME or Mouser prefix means DigiKey.
inline constexpr const char* kBomEnrichmentTme = "tme\x1e";

// Marks a suggestion from Mouser. Part numbers never contain the separator.
inline constexpr const char* kBomEnrichmentMouser = "mouser\x1e";

// nullopt when nothing may be cached. Failures are never cached, so the line is retried.
// Set at most one of fromTme and fromMouser; neither means DigiKey.
optional<string> bomEnrichmentEntry(BomLookupOutcome outcome, const string& suggestion, bool verified = false,
                                    bool fromTme = false, bool fromMouser = false);
// Part number of a stored suggestion, without prefixes. Empty for markers.
string bomEnrichmentPart(const string& stored);
bool bomEnrichmentVerified(const string& stored);
// Supplier of a stored suggestion: "DigiKey", "TME" or "Mouser". Empty for markers and failures.
string bomEnrichmentSupplier(const string& stored);

// True when the line already has an answer. Empty values and the legacy "-" do not count.
bool bomEnrichmentCached(const map<string, string>& enrichment, const string& key);

string bomEnrichmentExportText(const string& stored);

// Shortage list as CSV (CRLF rows, formula-safe cells). Markers and failures export as empty cells.
struct BomShortageExport {
  string text;
  size_t rows = 0;
};
BomShortageExport buildBomShortageCsv(const BomAnalysis& analysis, const map<string, string>& enrichment);

}  // namespace inventatory
