// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Scan code resolution for local, DigiKey and TME label flows.

#include "core/inventory/InventoryInternals.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kMaximumTmeLabelBytes = 512;
constexpr size_t kMaximumTmeLabelFieldBytes = 64;

// Symbols and part numbers use letters, digits and the separators that appear in TME and manufacturer codes.
bool isTmeLabelIdentifier(const string& value) {
  if (value.empty() || value.size() > kMaximumTmeLabelFieldBytes) return false;
  return all_of(value.begin(), value.end(), [](char character) {
    if (isalnum(static_cast<unsigned char>(character))) return true;
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

// A TME label is matched by its symbol. A new label creates a placeholder that carries the symbol, so the TME
// lookup can run after the scan is committed.
ScanResolution resolveTmeLabel(InventoryStore& store, const TmeLabelFields& label) {
  for (const auto& existing : store.items()) {
    if (tmeProductSymbol(existing) == label.symbol) return {true, false, existing.id, "Matched existing item"};
  }

  InventoryItem item;
  item.id = sanitizeIdPart(label.symbol) + "-" + makeId().substr(0, 8);
  item.partName = "Scanned TME Item";
  item.manufacturer = "Unknown";
  item.category = "Unsorted";
  item.quantity = 0;
  item.reorderThreshold = 0;
  item.location = "Scan Inbox";
  item.tags = {"scanned"};
  item.notes = "Created from a TME label scan.";
  item.sku = label.symbol;
  item.syncStatus = "needs_metadata";
  item.vendorMetadata.provider = "tme";
  item.vendorMetadata.providerProductNumber = label.symbol;
  item.vendorMetadata.manufacturerPartNumber = label.manufacturerPartNumber;
  item.vendorMetadata.productUrl = label.productUrl;
  item.lastUpdated = nowEpoch();
  item.createdAt = item.lastUpdated;
  store.items().push_back(move(item));
  return {true, true, store.items().back().id, "Created a placeholder item from the TME label"};
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

  if (!isTmeLabelIdentifier(label.symbol)) return nullopt;
  if (label.manufacturerPartNumber.empty() && !hasManufacturer) return nullopt;
  if (!label.manufacturerPartNumber.empty() && !isTmeLabelIdentifier(label.manufacturerPartNumber)) {
    return nullopt;
  }
  return label;
}

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

  if (const auto label = parseTmeLabelPayload(code); label.has_value()) {
    return resolveTmeLabel(store, *label);
  }

  if (toLower(code).rfind("inventatory:", 0) == 0) {
    return {false, false, {}, "Unknown Inventatory ID"};
  }

  if (auto* existing = store.findByCode(code)) {
    return {true, false, existing->id, "Matched existing item"};
  }

  InventoryItem item;
  item.id = sanitizeIdPart(code) + "-" + makeId().substr(0, 8);
  item.partName = "Scanned DigiKey Item";
  item.manufacturer = "Unknown";
  item.category = "Unsorted";
  item.quantity = 0;
  item.reorderThreshold = 0;
  item.location = "Scan Inbox";
  item.tags = {"scanned"};
  item.notes = "Created from a DigiKey code scan.";
  item.digikeyPartNumber = code;
  item.syncStatus = "needs_metadata";
  item.sku = code;
  item.lastUpdated = nowEpoch();
  item.createdAt = item.lastUpdated;
  store.items().push_back(move(item));
  return {true, true, store.items().back().id, "Created a placeholder item from the scanned code"};
}

}  // namespace inventatory
