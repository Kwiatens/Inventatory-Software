// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Durable storage for pinned BOM projects.

#include "core/bom/BomProjectStore.h"

#include "core/inventory/VendorProvider.h"
#include "core/storage/InventorySqlite.h"
#include "core/transfer/CsvExport.h"

#include <algorithm>

namespace inventatory {

using namespace std;

optional<string> bomEnrichmentEntry(BomLookupOutcome outcome, const string& suggestion, bool verified, bool fromTme,
                                    bool fromMouser) {
  switch (outcome) {
    case BomLookupOutcome::Found: {
      const auto trimmed = trim(suggestion);
      // A part without a usable number gives the user nothing to act on.
      if (trimmed.empty() || trimmed == "-") return string(kBomEnrichmentNoMatch);
      // DigiKey parts are stored without a supplier prefix.
      const string stored = fromTme      ? string(kBomEnrichmentTme) + trimmed
                            : fromMouser ? string(kBomEnrichmentMouser) + trimmed
                                         : trimmed;
      return verified ? string(kBomEnrichmentVerified) + stored : stored;
    }
    case BomLookupOutcome::NoMatch:
      return string(kBomEnrichmentNoMatch);
    case BomLookupOutcome::Failed:
      break;
  }
  return nullopt;
}

optional<string> bomEnrichmentEntry(BomLookupOutcome outcome, const string& suggestion, const BomEnrichmentFlags& flags,
                                    const string& provider) {
  if (outcome == BomLookupOutcome::Failed) return nullopt;
  string note;
  for (const char ch : flags.note) {
    if (ch != '\x1e' && ch != '\x1f') note.push_back(ch);
  }
  note = trim(note);
  const string notePrefix = note.empty() ? string() : string(kBomEnrichmentNote) + note + "\x1e";
  if (outcome == BomLookupOutcome::NoMatch) {
    return notePrefix + (flags.needsChoice ? kBomEnrichmentNeedsChoice : kBomEnrichmentNoMatch);
  }
  const auto stored = bomEnrichmentEntry(outcome, suggestion, false, provider == kVendorProviderTme,
                                         provider == kVendorProviderMouser);
  if (!stored.has_value() || *stored == kBomEnrichmentNoMatch) return stored;
  // The verified mark stays first, so values written before the other flags read the same.
  return string(flags.verified ? kBomEnrichmentVerified : "") + (flags.chosen ? kBomEnrichmentChosen : "") +
         (flags.check ? kBomEnrichmentCheck : "") + notePrefix + *stored;
}

string bomPreferredVendor(const map<string, string>& enrichment) {
  const auto found = enrichment.find(kBomPreferredVendorKey);
  return found == enrichment.end() ? string() : trim(found->second);
}

string bomPreferredProvider(const map<string, string>& enrichment) {
  const auto value = bomPreferredVendor(enrichment);
  return value == kVendorProviderDigiKey || value == kVendorProviderTme || value == kVendorProviderMouser ? value
                                                                                                          : string();
}

bool bomEnrichmentCached(const map<string, string>& enrichment, const string& key) {
  const auto found = enrichment.find(key);
  if (found == enrichment.end()) return false;
  const auto value = trim(found->second);
  return !value.empty() && value != "-";
}

namespace {

// The stored value without its flags and note, and the flags it carried.
string withoutFlags(const string& stored, BomEnrichmentFlags* flags = nullptr) {
  string value = stored;
  for (bool stripped = true; stripped;) {
    stripped = false;
    for (const auto& [prefix, field] : {pair<const char*, bool BomEnrichmentFlags::*>{kBomEnrichmentVerified, &BomEnrichmentFlags::verified},
                                        {kBomEnrichmentChosen, &BomEnrichmentFlags::chosen},
                                        {kBomEnrichmentCheck, &BomEnrichmentFlags::check}}) {
      if (value.rfind(prefix, 0) == 0) {
        value = value.substr(string(prefix).size());
        if (flags != nullptr) flags->*field = true;
        stripped = true;
      }
    }
    if (value.rfind(kBomEnrichmentNote, 0) == 0) {
      const auto end = value.find('\x1e');
      if (end == string::npos) break;
      if (flags != nullptr) flags->note = value.substr(string(kBomEnrichmentNote).size(), end - string(kBomEnrichmentNote).size());
      value = value.substr(end + 1);
      stripped = true;
    }
  }
  if (flags != nullptr) flags->needsChoice = trim(value) == kBomEnrichmentNeedsChoice;
  return value;
}


// Supplier prefix of a stored suggestion and the vendor it names. No prefix means DigiKey.
struct SupplierPrefix {
  const char* prefix;
  const char* provider;
};
constexpr SupplierPrefix kSupplierPrefixes[] = {
    {kBomEnrichmentTme, kVendorProviderTme},
    {kBomEnrichmentMouser, kVendorProviderMouser},
};

// nullptr when the suggestion has no supplier prefix.
const SupplierPrefix* supplierPrefixOf(const string& value) {
  for (const auto& entry : kSupplierPrefixes) {
    if (value.rfind(entry.prefix, 0) == 0) return &entry;
  }
  return nullptr;
}

}  // namespace

string bomEnrichmentPart(const string& stored) {
  auto value = withoutFlags(stored);
  if (const auto* supplier = supplierPrefixOf(value)) value = value.substr(string(supplier->prefix).size());
  const auto trimmed = trim(value);
  return trimmed == "-" || trimmed == kBomEnrichmentNoMatch || trimmed == kBomEnrichmentNeedsChoice ? string() : trimmed;
}

BomEnrichmentFlags bomEnrichmentFlags(const string& stored) {
  BomEnrichmentFlags flags;
  withoutFlags(stored, &flags);
  return flags;
}

bool bomEnrichmentChosen(const string& stored) { return bomEnrichmentFlags(stored).chosen && !bomEnrichmentPart(stored).empty(); }

bool bomEnrichmentNeedsChoice(const string& stored) { return bomEnrichmentFlags(stored).needsChoice; }

string bomChosenElsewhere(const vector<BomProject>& projects, const string& exceptProjectId, const string& lineKey) {
  for (const auto& project : projects) {
    if (project.id == exceptProjectId) continue;
    const auto found = project.enrichment.find(lineKey);
    if (found != project.enrichment.end() && bomEnrichmentChosen(found->second)) return found->second;
  }
  return {};
}

string bomEnrichmentSupplier(const string& stored) {
  if (bomEnrichmentPart(stored).empty()) return {};
  const auto* supplier = supplierPrefixOf(withoutFlags(stored));
  return vendorDisplayName(supplier == nullptr ? kVendorProviderDigiKey : supplier->provider);
}

string bomEnrichmentProvider(const string& stored) {
  if (bomEnrichmentPart(stored).empty()) return {};
  const auto* supplier = supplierPrefixOf(withoutFlags(stored));
  return supplier == nullptr ? kVendorProviderDigiKey : supplier->provider;
}

bool bomEnrichmentVerified(const string& stored) { return bomEnrichmentFlags(stored).verified; }

string bomSuggestionCheckText(const string& stored) {
  const auto flags = bomEnrichmentFlags(stored);
  if (bomEnrichmentPart(stored).empty()) {
    // Notes for these lines already end in what to do, such as "many parts fit: pick one".
    if (flags.needsChoice) return flags.note.empty() ? "pick a part" : flags.note;
    return flags.note;
  }
  if (flags.chosen) return "chosen by you";
  if (flags.check) return flags.note.empty() ? "check before ordering" : "check: " + flags.note;
  if (!flags.verified) return "check before ordering";
  return flags.note.empty() ? "verified" : "verified: " + flags.note;
}

string bomEnrichmentExportText(const string& stored) {
  return bomEnrichmentPart(stored);
}

namespace {

// "TME 1.20 EUR (12% less)" for an offer cheaper than the chosen one; empty when there is none.
string cheaperElsewhereNote(const PurchaseChoice& choice) {
  if (!choice.cheaperElsewhere || choice.cost.totalMicros <= 0) return {};
  const auto saved = choice.cost.totalMicros - choice.cheaperCost.totalMicros;
  const auto percent = static_cast<int>(saved * 100 / choice.cost.totalMicros);
  return vendorDisplayName(choice.cheaperOffer.provider) + " " + formatMicros(choice.cheaperCost.totalMicros) + " " +
         choice.cheaperOffer.currency + " (" + to_string(max(1, percent)) + "% less)";
}

}  // namespace

BomShortageExport buildBomShortageCsv(const BomAnalysis& analysis, const map<string, string>& enrichment,
                                      const BomPurchaseFinder& purchase) {
  BomShortageExport result;
  result.text = "Designation,Footprint,Package,Designators,Needed,On hand,Suggested part,Supplier,"
                "Suggestion check";
  if (purchase) result.text += ",Vendor,Vendor part number,Order quantity,Unit price,Order total,Currency,Link,Cheaper elsewhere";
  result.text += "\r\n";
  for (const auto& match : analysis.matches) {
    if (match.sufficient || match.lineIndex >= analysis.lines.size()) {
      continue;
    }
    const auto& line = analysis.lines[match.lineIndex];
    const auto suggestion = enrichment.find(bomLineKey(line));
    const auto part = suggestion == enrichment.end() ? string() : bomEnrichmentPart(suggestion->second);
    const auto supplier = suggestion == enrichment.end() ? string() : bomEnrichmentSupplier(suggestion->second);
    // A suggestion that did not fit the line is never presented as a verified part.
    const string check = suggestion == enrichment.end() ? string() : bomSuggestionCheckText(suggestion->second);
    // A line awaiting confirmation counts nothing on hand: its stock is not yet the accepted part.
    const int onHand = match.confirmed ? match.available : 0;
    result.text += csvTextCell(line.designation) + ',' + csvTextCell(line.footprint) + ',' +
                   csvTextCell(packageFromFootprint(line.footprint)) + ',' + csvTextCell(join(line.designators, ' ')) +
                   ',' + to_string(match.needed) + ',' + to_string(onHand) + ',' + csvTextCell(part) + ',' +
                   csvTextCell(supplier) + ',' + csvTextCell(check);
    if (purchase) {
      const auto toBuy = static_cast<uint64_t>(max(0, match.needed - onHand));
      const auto choice = purchase(line, match, toBuy);
      if (choice.has_value()) {
        const auto& offer = choice->offer;
        result.text += ',' + csvTextCell(vendorDisplayName(offer.provider)) + ',' + csvTextCell(offer.sku) + ',' +
                       to_string(choice->cost.orderQuantity) + ',' + formatPriceMicros(choice->cost.unitMicros) + ',' +
                       formatMicros(choice->cost.totalMicros) + ',' + csvTextCell(offer.currency) + ',' +
                       csvTextCell(vendorPurchaseUrl(offer)) + ',' + csvTextCell(cheaperElsewhereNote(*choice));
        ++result.pricedRows;
        // One currency per list; offers in another currency are not added up.
        if (result.purchaseCurrency.empty()) result.purchaseCurrency = offer.currency;
        if (offer.currency == result.purchaseCurrency) result.totalMicros += choice->cost.totalMicros;
      } else {
        result.text += ",,,,,,,,";
      }
    }
    result.text += "\r\n";
    ++result.rows;
  }
  return result;
}

namespace {

bool parseLegacyBomMap(const string& value, map<string, string>& values) {
  size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(';', start);
    const auto entry = value.substr(start, end == string::npos ? string::npos : end - start);
    const auto equals = entry.find('=');
    if (equals == string::npos ||
        (entry.find(':') != string::npos && entry.find(':') < equals)) return false;
    const auto key = trim(entry.substr(0, equals));
    if (key.empty() || !values.emplace(key, trim(entry.substr(equals + 1))).second) return false;
    if (end == string::npos) break;
    start = end + 1;
  }
  return true;
}

bool deserializeBomMapChecked(const string& value, map<string, string>& values) {
  if (value.empty()) {
    values.clear();
    return true;
  }
  const bool structured = value.rfind("v1:", 0) == 0 || value.rfind("v2:", 0) == 0;
  if (structured) {
    vector<Parameter> parameters;
    if (!deserializeParametersFromStorageStrict(value, parameters)) return false;
    map<string, string> parsed;
    for (const auto& parameter : parameters) {
      if (parameter.name.empty() || !parsed.emplace(parameter.name, parameter.value).second) return false;
    }
    values = move(parsed);
    return true;
  }
  map<string, string> parsed;
  if (!parseLegacyBomMap(value, parsed)) return false;
  values = move(parsed);
  return true;
}

string serializeBomMap(const map<string, string>& values) {
  // Reuses the parameter encoding so escaping rules stay in one place.
  vector<Parameter> parameters;
  parameters.reserve(values.size());
  for (const auto& entry : values) {
    parameters.push_back({entry.first, entry.second});
  }
  return serializeParametersForStorage(parameters);
}

bool insertProject(SqliteConnection& connection, const BomProject& project) {
  SqliteStatement statement;
  const char* sql = R"SQL(
    INSERT INTO inventatory_bom_projects
      (id, name, source_path, boards, created_at, last_opened, last_built, bom_text, overrides, enrichment)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  const auto overrides = serializeBomMap(project.overrides);
  const auto enrichment = serializeBomMap(project.enrichment);
  sqliteApi().bind_text(statement.stmt, 1, project.id.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 2, project.name.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 3, project.sourcePath.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_int(statement.stmt, 4, max(1, project.boards));
  sqliteApi().bind_int64(statement.stmt, 5, static_cast<sqlite3_int64>(project.createdAt));
  sqliteApi().bind_int64(statement.stmt, 6, static_cast<sqlite3_int64>(project.lastOpened));
  sqliteApi().bind_int64(statement.stmt, 7, static_cast<sqlite3_int64>(project.lastBuilt));
  sqliteApi().bind_text(statement.stmt, 8, project.bomText.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 9, overrides.c_str(), -1, SQLITE_TRANSIENT);
  sqliteApi().bind_text(statement.stmt, 10, enrichment.c_str(), -1, SQLITE_TRANSIENT);
  return sqliteApi().step(statement.stmt) == SQLITE_DONE;
}

}  // namespace

bool loadBomProjects(const filesystem::path& databasePath, vector<BomProject>& projects) {
  vector<BomProject> loadedProjects;
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection)) {
    return false;
  }

  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, name, source_path, boards, created_at, last_opened, last_built, bom_text, overrides, enrichment
    FROM inventatory_bom_projects ORDER BY last_opened DESC, created_at DESC
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    BomProject project;
    project.id = sqliteText(statement.stmt, 0);
    project.name = sqliteText(statement.stmt, 1);
    project.sourcePath = sqliteText(statement.stmt, 2);
    if (!sqliteInt32(statement.stmt, 3, project.boards) || project.boards <= 0 ||
        !sqliteTime(statement.stmt, 4, project.createdAt) || !sqliteTime(statement.stmt, 5, project.lastOpened) ||
        !sqliteTime(statement.stmt, 6, project.lastBuilt)) return false;
    project.bomText = sqliteText(statement.stmt, 7);
    if (!deserializeBomMapChecked(sqliteText(statement.stmt, 8), project.overrides) ||
        !deserializeBomMapChecked(sqliteText(statement.stmt, 9), project.enrichment)) return false;
    loadedProjects.push_back(move(project));
  }
  if (stepResult != SQLITE_DONE) return false;
  projects = move(loadedProjects);
  return true;
}

bool validateBomProjects(SqliteConnection& connection, string* error) {
  if (connection.db == nullptr) {
    if (error != nullptr) *error = "SQLite connection is not open";
    return false;
  }
  SqliteStatement statement;
  const char* sql = R"SQL(
    SELECT id, name, source_path, boards, created_at, last_opened, last_built, bom_text, overrides, enrichment
    FROM inventatory_bom_projects
  )SQL";
  if (sqliteApi().prepare_v2(connection.db, sql, -1, &statement.stmt, nullptr) != SQLITE_OK) {
    if (error != nullptr) *error = "Unable to read BOM projects for validation";
    return false;
  }

  int stepResult = SQLITE_OK;
  while ((stepResult = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    int boards = 0;
    time_t createdAt = 0;
    time_t lastOpened = 0;
    time_t lastBuilt = 0;
    map<string, string> ignored;
    if (sqliteText(statement.stmt, 0).empty() || sqliteText(statement.stmt, 1).empty() ||
        !sqliteInt32(statement.stmt, 3, boards) || boards <= 0 ||
        !sqliteTime(statement.stmt, 4, createdAt) || !sqliteTime(statement.stmt, 5, lastOpened) ||
        !sqliteTime(statement.stmt, 6, lastBuilt) ||
        !deserializeBomMapChecked(sqliteText(statement.stmt, 8), ignored) ||
        !deserializeBomMapChecked(sqliteText(statement.stmt, 9), ignored)) {
      if (error != nullptr) *error = "BOM project data is malformed";
      return false;
    }
  }
  if (stepResult != SQLITE_DONE) {
    if (error != nullptr) *error = "Unable to finish reading BOM projects for validation";
    return false;
  }
  return true;
}

bool saveBomProjects(const filesystem::path& databasePath, const vector<BomProject>& projects) {
  SqliteConnection connection;
  if (!openDatabase(databasePath, connection) || !ensureInventoryDatabaseSchema(connection) ||
      !execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    return false;
  }

  // Small table, rewritten wholesale like the other stores.
  bool ok = execSql(connection, "DELETE FROM inventatory_bom_projects");
  for (const auto& project : projects) {
    if (!ok) {
      break;
    }
    ok = insertProject(connection, project);
  }

  if (!ok || !execSql(connection, "COMMIT")) {
    execSql(connection, "ROLLBACK");
    return false;
  }
  return true;
}

}  // namespace inventatory
