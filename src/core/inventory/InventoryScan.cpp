// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Scan code resolution for vendor bags (DigiKey and TME), machine codes and TME label payloads.

#include "core/inventory/InventoryInternals.h"
#include "core/inventory/VendorBagCode.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kMaximumTmeLabelBytes = 512;
constexpr size_t kMaximumTmeLabelFieldBytes = 64;

const char kUnrecognizedBagMessage[] = "This code is not from a supported vendor bag. Add the part manually";

// Symbols and part numbers use letters, digits and the separators of TME and manufacturer codes.
// A manufacturer part number may also carry '*' (CONNFLY's DS1023-1*40S21); a TME symbol may not.
bool isTmeLabelIdentifier(const string& value, bool allowAsterisk) {
  if (value.empty() || value.size() > kMaximumTmeLabelFieldBytes) return false;
  return all_of(value.begin(), value.end(), [allowAsterisk](char character) {
    if (isalnum(static_cast<unsigned char>(character))) return true;
    if (allowAsterisk && character == '*') return true;
    return character == '-' || character == '_' || character == '.' || character == '/' || character == '+' ||
           character == '#';
  });
}

bool parseTmeQuantity(const string& value, int& quantity) {
  if (value.empty() || value.size() > 6) return false;
  for (const char character : value) {
    if (!isdigit(static_cast<unsigned char>(character))) return false;
  }
  quantity = stoi(value);
  return true;
}

// The part a vendor identity names, matched by the vendor's own part number. A TME symbol kept
// beside a DigiKey part still matches, so its TME label does not create a duplicate.
InventoryItem* findVendorPart(InventoryStore& store, const VendorBagIdentity& identity) {
  const auto number = toLower(trim(identity.providerProductNumber));
  if (number.empty()) return nullptr;
  for (auto& item : store.items()) {
    if (identity.provider == "tme") {
      if (toLower(trim(tmeProductSymbol(item))) == number) return &item;
      continue;
    }
    if (toLower(trim(item.digikeyPartNumber)) == number ||
        (toLower(trim(item.vendorMetadata.provider)) == "digikey" &&
         toLower(trim(item.vendorMetadata.providerProductNumber)) == number)) {
      return &item;
    }
  }
  return nullptr;
}

// A TME label creates a placeholder with the symbol, so the lookup can run after the commit.
ScanResolution resolveTmeLabel(InventoryStore& store, const VendorBagIdentity& identity) {
  if (auto* existing = findVendorPart(store, identity)) {
    return {true, false, existing->id, "Matched existing item"};
  }

  InventoryItem item;
  item.id = sanitizeIdPart(identity.providerProductNumber) + "-" + makeId().substr(0, 8);
  item.partName = "Scanned TME Item";
  item.manufacturer = "Unknown";
  item.category = "Unsorted";
  item.quantity = 0;
  item.reorderThreshold = 0;
  item.location = "Scan Inbox";
  item.tags = {"scanned"};
  item.notes = "Created from a TME label scan.";
  item.sku = identity.providerProductNumber;
  item.syncStatus = "needs_metadata";
  item.vendorMetadata.provider = "tme";
  item.vendorMetadata.providerProductNumber = identity.providerProductNumber;
  item.vendorMetadata.manufacturerPartNumber = identity.manufacturerPartNumber;
  item.vendorMetadata.productUrl = identity.productUrl;
  item.lastUpdated = nowEpoch();
  item.createdAt = item.lastUpdated;
  store.items().push_back(move(item));
  return {true, true, store.items().back().id, "Created a placeholder item from the TME label"};
}

// A DigiKey bag creates a placeholder with the part number, so the lookup can run after the commit.
ScanResolution resolveDigiKeyBag(InventoryStore& store, const VendorBagIdentity& identity) {
  if (auto* existing = findVendorPart(store, identity)) {
    return {true, false, existing->id, "Matched existing item"};
  }

  InventoryItem item;
  item.id = sanitizeIdPart(identity.providerProductNumber) + "-" + makeId().substr(0, 8);
  item.partName = "Scanned DigiKey Item";
  item.manufacturer = "Unknown";
  item.category = "Unsorted";
  item.quantity = 0;
  item.reorderThreshold = 0;
  item.location = "Scan Inbox";
  item.tags = {"scanned"};
  item.notes = "Created from a DigiKey bag scan.";
  item.digikeyPartNumber = identity.providerProductNumber;
  item.sku = identity.providerProductNumber;
  item.syncStatus = "needs_metadata";
  item.vendorMetadata.provider = "digikey";
  item.vendorMetadata.providerProductNumber = identity.providerProductNumber;
  item.vendorMetadata.manufacturerPartNumber = identity.manufacturerPartNumber;
  item.lastUpdated = nowEpoch();
  item.createdAt = item.lastUpdated;
  store.items().push_back(move(item));
  return {true, true, store.items().back().id, "Created a placeholder item from the DigiKey bag"};
}

}  // namespace

optional<TmeLabelFields> parseTmeLabelPayload(const string& rawCode) {
  const auto text = trim(rawCode);
  if (text.empty() || text.size() > kMaximumTmeLabelBytes) return nullopt;

  TmeLabelFields label;
  bool hasManufacturer = false;
  istringstream tokens(text);
  string token;
  while (tokens >> token) {
    if (token.rfind("https://www.tme.eu/", 0) == 0) {
      label.productUrl = token;
      continue;
    }
    const auto colon = token.find(':');
    if (colon == string::npos || colon == 0) continue;
    const auto key = toLower(token.substr(0, colon));
    const auto value = token.substr(colon + 1);
    if (key == "pn") {
      label.symbol = value;
    } else if (key == "mpn") {
      label.manufacturerPartNumber = value;
    } else if (key == "mfr") {
      if (value.size() > kMaximumTmeLabelFieldBytes) return nullopt;
      label.manufacturer = value;
      hasManufacturer = true;
    } else if (key == "po") {
      label.orderNumber = value;
    } else if (key == "qty") {
      if (!parseTmeQuantity(value, label.quantity)) return nullopt;
    }
  }

  if (!isTmeLabelIdentifier(label.symbol, false)) return nullopt;
  if (label.manufacturerPartNumber.empty() && !hasManufacturer) return nullopt;
  if (!label.manufacturerPartNumber.empty() && !isTmeLabelIdentifier(label.manufacturerPartNumber, true)) {
    return nullopt;
  }
  return label;
}

// Resolves a scanner code. Only a supported vendor bag's main code can create a part; machine codes
// find the part they label.
ScanResolution resolveScanCode(InventoryStore& store, const string& rawCode) {
  const auto code = trim(rawCode);
  if (code.empty()) {
    return {false, false, {}, "Empty scan code"};
  }

  if (isMachineCode(code)) {
    if (auto* existing = store.findByMachineCode(code)) {
      return {true, false, existing->id, "Matched existing item"};
    }
    return {false, false, {}, "Unknown machine code"};
  }

  if (toLower(code).rfind("inventatory:", 0) == 0) {
    return {false, false, {}, "Unknown Inventatory ID"};
  }

  const auto identity = recognizeVendorBagCode(code);
  if (!identity) {
    return {false, false, {}, kUnrecognizedBagMessage};
  }
  if (identity->provider == "tme") return resolveTmeLabel(store, *identity);
  return resolveDigiKeyBag(store, *identity);
}

}  // namespace inventatory
