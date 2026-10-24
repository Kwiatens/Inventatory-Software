// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Durable storage for pinned BOM projects.

#pragma once

#include "core/vendor/VendorPurchase.h"

#include <functional>
#include <optional>

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

// Marks a suggestion the user should look at before ordering: a variant was chosen for them, or the
// value moved to the nearest standard one. The note says which.
inline constexpr const char* kBomEnrichmentCheck = "check\x1e";

// Marks a part the user picked. Lookups never replace it, and other projects with the same line reuse it.
inline constexpr const char* kBomEnrichmentChosen = "chosen\x1e";

// "note\x1f<text>\x1e": why a suggestion is flagged or a line has none. The text holds neither separator.
inline constexpr const char* kBomEnrichmentNote = "note\x1f";

// Enrichment value for a line many parts fit (a header, an LED without a colour): the user picks one.
inline constexpr const char* kBomEnrichmentNeedsChoice = "(pick a part)";

// What is kept beside a stored suggestion. Values written before these flags read as unflagged.
struct BomEnrichmentFlags {
  bool verified = false;
  bool check = false;
  bool chosen = false;
  bool needsChoice = false;  // NoMatch only: store kBomEnrichmentNeedsChoice instead of kBomEnrichmentNoMatch
  string note;
};

// nullopt when nothing may be cached. Failures are never cached, so the line is retried.
// Set at most one of fromTme and fromMouser; neither means DigiKey.
optional<string> bomEnrichmentEntry(BomLookupOutcome outcome, const string& suggestion, bool verified = false,
                                    bool fromTme = false, bool fromMouser = false);
// The same with every flag. provider is a kVendorProvider* key; empty means DigiKey.
optional<string> bomEnrichmentEntry(BomLookupOutcome outcome, const string& suggestion, const BomEnrichmentFlags& flags,
                                    const string& provider);
BomEnrichmentFlags bomEnrichmentFlags(const string& stored);
// Part number of a stored suggestion, without prefixes. Empty for markers.
string bomEnrichmentPart(const string& stored);
bool bomEnrichmentVerified(const string& stored);
bool bomEnrichmentChosen(const string& stored);
bool bomEnrichmentNeedsChoice(const string& stored);
// The part another project's user picked for a line with this key, as stored there; empty when none.
string bomChosenElsewhere(const vector<BomProject>& projects, const string& exceptProjectId, const string& lineKey);
// Vendor provider key ("digikey", "tme" or "mouser") of a stored suggestion. Empty for markers and failures.
string bomEnrichmentProvider(const string& stored);
// Supplier of a stored suggestion: "DigiKey", "TME" or "Mouser". Empty for markers and failures.
string bomEnrichmentSupplier(const string& stored);

// Per-project vendor choice, kept in the enrichment map under keys no BOM line can have.
// "digikey", "tme" or "mouser" prefers that vendor, "any" means the user chose no preference,
// and empty means the project has not been asked yet.
inline constexpr const char* kBomPreferredVendorKey = "@vendor";
// The preference the saved suggestions were last matched against; differs from the choice until
// a lookup run has re-checked the lines that came from another vendor.
inline constexpr const char* kBomVendorAppliedKey = "@vendor-applied";
// The matcher version the saved suggestions came from. Suggestions from an older matcher are looked up
// again once, except parts the user picked.
inline constexpr const char* kBomMatcherVersionKey = "@matcher";
inline constexpr const char* kBomMatcherVersion = "2";
string bomPreferredVendor(const map<string, string>& enrichment);
// The vendor to prefer in lookups and purchases: empty for "any" or not asked.
string bomPreferredProvider(const map<string, string>& enrichment);

// True when the line already has an answer. Empty values and the legacy "-" do not count.
bool bomEnrichmentCached(const map<string, string>& enrichment, const string& key);

string bomEnrichmentExportText(const string& stored);
// How far a stored suggestion can be trusted, in words: "verified", "check: variants: N4, N8",
// "chosen by you", "pick a part", or the note of a line with no part. Empty when nothing is known.
string bomSuggestionCheckText(const string& stored);

// Shortage list as CSV (CRLF rows, formula-safe cells). Markers and failures export as empty cells.
struct BomShortageExport {
  string text;
  size_t rows = 0;
  size_t pricedRows = 0;           // Rows that got a vendor offer
  std::int64_t totalMicros = 0;    // The offers' order totals added up; one currency, see purchaseCurrency
  string purchaseCurrency;
};
// The offer to buy `toBuy` pieces of a line from, or nothing when none is known.
using BomPurchaseFinder = std::function<std::optional<PurchaseChoice>(const BomLine&, const BomMatch&, uint64_t toBuy)>;
// With a finder, each row also gets the vendor, its part number, the order quantity, the unit price, the order
// total, the currency, the product link and a note when another vendor is cheaper. Without one the columns
// are left out.
BomShortageExport buildBomShortageCsv(const BomAnalysis& analysis, const map<string, string>& enrichment,
                                      const BomPurchaseFinder& purchase = nullptr);

}  // namespace inventatory
