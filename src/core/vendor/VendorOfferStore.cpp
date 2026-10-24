// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/vendor/VendorOfferStore.h"

#include "core/storage/InventorySqlite.h"

#include <limits>
#include <map>
#include <system_error>

namespace inventatory {

using namespace std;

namespace {

void setError(string* error, const string& message) {
  if (error != nullptr) *error = message;
}

bool bindText(sqlite3_stmt* statement, int index, const string& value) {
  return sqliteApi().bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

bool bindInt64(sqlite3_stmt* statement, int index, sqlite3_int64 value) {
  return sqliteApi().bind_int64(statement, index, value) == SQLITE_OK;
}

bool bindOptional(sqlite3_stmt* statement, int index, bool present, sqlite3_int64 value) {
  return present ? bindInt64(statement, index, value) : sqliteApi().bind_null(statement, index) == SQLITE_OK;
}

bool fitsInt64(uint64_t value) { return value <= static_cast<uint64_t>((numeric_limits<sqlite3_int64>::max)()); }

// Creates the tables in a new file, accepts this version, and refuses a newer one.
bool ensureSchema(SqliteConnection& connection, string* error) {
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db, "PRAGMA user_version", -1, &statement.stmt, nullptr) != SQLITE_OK ||
      sqliteApi().step(statement.stmt) != SQLITE_ROW) {
    setError(error, "Unable to read the saved vendor prices");
    return false;
  }
  const auto version = sqliteApi().column_int64(statement.stmt, 0);
  if (version == kVendorOfferSchemaVersion) return true;
  if (version != 0) {
    setError(error, "The saved vendor prices come from a newer Inventatory");
    return false;
  }
  const char* schema = R"SQL(
    BEGIN IMMEDIATE TRANSACTION;
    CREATE TABLE IF NOT EXISTS vendor_offers (
      provider TEXT NOT NULL, lookup_key TEXT NOT NULL, position INTEGER NOT NULL, sku TEXT NOT NULL,
      manufacturer TEXT NOT NULL, manufacturer_part_number TEXT NOT NULL, product_url TEXT NOT NULL,
      currency TEXT NOT NULL, stock INTEGER, minimum_order INTEGER NOT NULL, order_multiple INTEGER NOT NULL,
      packaging TEXT NOT NULL, reeling_fee_micros INTEGER, lifecycle TEXT NOT NULL, orderable INTEGER NOT NULL,
      fetched_at INTEGER NOT NULL, PRIMARY KEY (provider, lookup_key, position)
    );
    CREATE TABLE IF NOT EXISTS vendor_offer_breaks (
      provider TEXT NOT NULL, lookup_key TEXT NOT NULL, position INTEGER NOT NULL, quantity INTEGER NOT NULL,
      unit_micros INTEGER NOT NULL, PRIMARY KEY (provider, lookup_key, position, quantity)
    );
    PRAGMA user_version = 1;
    COMMIT;
  )SQL";
  if (!execSql(connection, schema)) {
    execSql(connection, "ROLLBACK");
    setError(error, "Unable to create the vendor price file");
    return false;
  }
  return true;
}

bool openStore(const filesystem::path& databasePath, SqliteConnection& connection, string* error) {
  if (!openDatabase(databasePath, connection)) {
    setError(error, "Unable to open the vendor price file");
    return false;
  }
  return ensureSchema(connection, error);
}

}  // namespace

filesystem::path vendorOfferDatabasePath(const filesystem::path& dataDirectory) {
  return dataDirectory / kVendorOfferDatabaseName;
}

bool saveVendorOffers(const filesystem::path& databasePath, const string& provider, const string& lookupKey,
                      const vector<VendorOffer>& offers, string* error) {
  if (provider.empty() || lookupKey.empty()) {
    setError(error, "A vendor price needs a vendor and a product number");
    return false;
  }
  SqliteConnection connection;
  if (!openStore(databasePath, connection, error)) return false;
  if (!execSql(connection, "BEGIN IMMEDIATE TRANSACTION")) {
    setError(error, "Unable to save the vendor prices");
    return false;
  }
  const auto fail = [&](const string& message) {
    execSql(connection, "ROLLBACK");
    setError(error, message);
    return false;
  };
  for (const char* sql : {"DELETE FROM vendor_offer_breaks WHERE provider = ?1 AND lookup_key = ?2",
                          "DELETE FROM vendor_offers WHERE provider = ?1 AND lookup_key = ?2"}) {
    SqliteStatement remove;
    if (sqliteApi().prepare_v2(connection.db, sql, -1, &remove.stmt, nullptr) != SQLITE_OK ||
        !bindText(remove.stmt, 1, provider) || !bindText(remove.stmt, 2, lookupKey) ||
        sqliteApi().step(remove.stmt) != SQLITE_DONE) {
      return fail("Unable to replace the saved vendor prices");
    }
  }
  SqliteStatement offerInsert;
  SqliteStatement breakInsert;
  if (sqliteApi().prepare_v2(connection.db, R"SQL(
        INSERT INTO vendor_offers (provider, lookup_key, position, sku, manufacturer, manufacturer_part_number,
          product_url, currency, stock, minimum_order, order_multiple, packaging, reeling_fee_micros, lifecycle,
          orderable, fetched_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)
      )SQL", -1, &offerInsert.stmt, nullptr) != SQLITE_OK ||
      sqliteApi().prepare_v2(connection.db,
                             "INSERT INTO vendor_offer_breaks (provider, lookup_key, position, quantity, unit_micros) "
                             "VALUES (?1, ?2, ?3, ?4, ?5)",
                             -1, &breakInsert.stmt, nullptr) != SQLITE_OK) {
    return fail("Unable to save the vendor prices");
  }
  for (size_t position = 0; position < offers.size(); ++position) {
    const auto& offer = offers[position];
    if (!isCurrencyCode(offer.currency) || offer.breaks.empty() || !fitsInt64(offer.minimumOrder) ||
        !fitsInt64(offer.multiple) || (offer.stock.has_value() && !fitsInt64(*offer.stock)) || offer.fetchedAt < 0) {
      return fail("A vendor price is incomplete and was not saved");
    }
    auto* statement = offerInsert.stmt;
    sqliteApi().reset(statement);
    sqliteApi().clear_bindings(statement);
    const bool bound =
        bindText(statement, 1, provider) && bindText(statement, 2, lookupKey) &&
        bindInt64(statement, 3, static_cast<sqlite3_int64>(position)) && bindText(statement, 4, offer.sku) &&
        bindText(statement, 5, offer.manufacturer) && bindText(statement, 6, offer.manufacturerPartNumber) &&
        bindText(statement, 7, offer.productUrl) && bindText(statement, 8, offer.currency) &&
        bindOptional(statement, 9, offer.stock.has_value(), static_cast<sqlite3_int64>(offer.stock.value_or(0))) &&
        bindInt64(statement, 10, static_cast<sqlite3_int64>(offer.minimumOrder)) &&
        bindInt64(statement, 11, static_cast<sqlite3_int64>(offer.multiple)) && bindText(statement, 12, offer.packaging) &&
        bindOptional(statement, 13, offer.reelingFeeMicros.has_value(), offer.reelingFeeMicros.value_or(0)) &&
        bindText(statement, 14, offer.lifecycle) && bindInt64(statement, 15, offer.orderable ? 1 : 0) &&
        bindInt64(statement, 16, static_cast<sqlite3_int64>(offer.fetchedAt));
    if (!bound || sqliteApi().step(statement) != SQLITE_DONE) return fail("Unable to save the vendor prices");
    for (const auto& entry : offer.breaks) {
      if (!fitsInt64(entry.quantity)) return fail("A vendor price is incomplete and was not saved");
      auto* row = breakInsert.stmt;
      sqliteApi().reset(row);
      sqliteApi().clear_bindings(row);
      if (!bindText(row, 1, provider) || !bindText(row, 2, lookupKey) ||
          !bindInt64(row, 3, static_cast<sqlite3_int64>(position)) ||
          !bindInt64(row, 4, static_cast<sqlite3_int64>(entry.quantity)) || !bindInt64(row, 5, entry.unitMicros) ||
          sqliteApi().step(row) != SQLITE_DONE) {
        return fail("Unable to save the vendor prices");
      }
    }
  }
  if (!execSql(connection, "COMMIT")) return fail("Unable to save the vendor prices");
  return true;
}

bool loadVendorOffers(const filesystem::path& databasePath, const string& provider, const string& lookupKey,
                      vector<VendorOffer>& offers, string* error) {
  offers.clear();
  error_code existsError;
  if (!filesystem::exists(databasePath, existsError)) return !existsError;
  SqliteConnection connection;
  if (!openStore(databasePath, connection, error)) return false;

  map<sqlite3_int64, vector<PriceBreak>> breaksByPosition;
  {
    SqliteStatement statement;
    if (sqliteApi().prepare_v2(connection.db,
                               "SELECT position, quantity, unit_micros FROM vendor_offer_breaks "
                               "WHERE provider = ?1 AND lookup_key = ?2",
                               -1, &statement.stmt, nullptr) != SQLITE_OK ||
        !bindText(statement.stmt, 1, provider) || !bindText(statement.stmt, 2, lookupKey)) {
      setError(error, "Unable to read the saved vendor prices");
      return false;
    }
    int result = SQLITE_OK;
    while ((result = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
      uint64_t quantity = 0;
      if (sqliteApi().column_type(statement.stmt, 0) != SQLITE_INTEGER || !sqliteUInt64(statement.stmt, 1, quantity) ||
          sqliteApi().column_type(statement.stmt, 2) != SQLITE_INTEGER) {
        continue;
      }
      breaksByPosition[sqliteApi().column_int64(statement.stmt, 0)].push_back(
          {quantity, sqliteApi().column_int64(statement.stmt, 2)});
    }
    if (result != SQLITE_DONE) {
      setError(error, "Unable to read the saved vendor prices");
      return false;
    }
  }

  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db, R"SQL(
        SELECT position, sku, manufacturer, manufacturer_part_number, product_url, currency, stock, minimum_order,
          order_multiple, packaging, reeling_fee_micros, lifecycle, orderable, fetched_at
        FROM vendor_offers WHERE provider = ?1 AND lookup_key = ?2 ORDER BY position
      )SQL", -1, &statement.stmt, nullptr) != SQLITE_OK ||
      !bindText(statement.stmt, 1, provider) || !bindText(statement.stmt, 2, lookupKey)) {
    setError(error, "Unable to read the saved vendor prices");
    return false;
  }
  int result = SQLITE_OK;
  while ((result = sqliteApi().step(statement.stmt)) == SQLITE_ROW) {
    auto* row = statement.stmt;
    VendorOffer offer;
    offer.provider = provider;
    offer.sku = sqliteText(row, 1);
    offer.manufacturer = sqliteText(row, 2);
    offer.manufacturerPartNumber = sqliteText(row, 3);
    offer.productUrl = sqliteText(row, 4);
    offer.currency = sqliteText(row, 5);
    uint64_t number = 0;
    if (sqliteApi().column_type(row, 6) != SQLITE_NULL) {
      if (!sqliteUInt64(row, 6, number)) continue;
      offer.stock = number;
    }
    if (!sqliteUInt64(row, 7, offer.minimumOrder) || !sqliteUInt64(row, 8, offer.multiple)) continue;
    offer.packaging = sqliteText(row, 9);
    if (sqliteApi().column_type(row, 10) != SQLITE_NULL) {
      if (sqliteApi().column_type(row, 10) != SQLITE_INTEGER) continue;
      offer.reelingFeeMicros = sqliteApi().column_int64(row, 10);
    }
    offer.lifecycle = sqliteText(row, 11);
    if (sqliteApi().column_type(row, 12) != SQLITE_INTEGER || !sqliteTime(row, 13, offer.fetchedAt)) continue;
    offer.orderable = sqliteApi().column_int64(row, 12) != 0;
    if (sqliteApi().column_type(row, 0) != SQLITE_INTEGER) continue;
    offer.breaks = breaksByPosition[sqliteApi().column_int64(row, 0)];
    if (!isCurrencyCode(offer.currency) || offer.sku.empty() || !normalizePriceBreaks(offer.breaks) ||
        offer.minimumOrder == 0 || offer.multiple == 0) {
      continue;
    }
    offers.push_back(move(offer));
  }
  if (result != SQLITE_DONE) {
    offers.clear();
    setError(error, "Unable to read the saved vendor prices");
    return false;
  }
  return true;
}

}  // namespace inventatory
