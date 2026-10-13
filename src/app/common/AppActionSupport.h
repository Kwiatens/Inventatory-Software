// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Private helpers shared by the focused App action translation units. This
// header is not part of the public application interface.

#include "App.h"
#include "platform/security/CredentialStore.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/tme/TmeApi.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

namespace app_actions {

constexpr size_t kDeviceStatusQueueLimit = 256;
constexpr uintmax_t kMaximumImportBytes = 25U * 1024U * 1024U;
constexpr const char* kInventatoryScanTokenCredential = "inventatory-scan-pairing-token";

// Delay before the oldest scanner event is committed again after a failed commit: 2 s, doubling up
// to 60 s.
inline chrono::seconds deviceSyncRetryDelay(int consecutiveFailures) {
  int seconds = 2;
  for (int step = 1; step < consecutiveFailures && seconds < 60; ++step) seconds *= 2;
  return chrono::seconds(min(seconds, 60));
}

// The names a scan gives a part until its supplier lookup fills in the details.
inline bool isScanPlaceholderName(const string& name) {
  const auto trimmed = trim(name);
  return trimmed == "Scanned DigiKey Item" || trimmed == "Scanned TME Item";
}

enum class WorkspaceScannerCredentialStatus {
  Loaded,
  FreshCredential,
  RequiresPairing,
};

struct WorkspaceScannerCredentialResolution {
  WorkspaceScannerCredentialStatus status = WorkspaceScannerCredentialStatus::RequiresPairing;
  optional<string> token;
  // The credential store could not answer (locked or missing keyring). The scanner stays disabled
  // exactly as for a missing credential, but the user is told the real reason.
  bool credentialStoreUnavailable = false;
};

inline bool validScannerToken(const string& token) {
  return token.size() == 64U &&
         all_of(token.begin(), token.end(), [](unsigned char ch) { return isxdigit(ch) != 0; });
}

inline WorkspaceScannerCredentialResolution resolveScannerCredential(const CredentialLookup& lookup,
                                                                     const InventatoryScanConfig& config) {
  if (lookup.status == CredentialReadStatus::Unavailable) {
    // Whether a token exists is unknown. Never mint a fresh one: it would replace the real token as
    // soon as the store answers again.
    return {WorkspaceScannerCredentialStatus::RequiresPairing, nullopt, true};
  }
  const auto& scopedCredential = lookup.secret;
  if (lookup.status == CredentialReadStatus::Found && scopedCredential.has_value() &&
      validScannerToken(*scopedCredential)) {
    return {WorkspaceScannerCredentialStatus::Loaded, scopedCredential, false};
  }
  if (lookup.status == CredentialReadStatus::Found || config.setupComplete || !trim(config.deviceId).empty()) {
    return {WorkspaceScannerCredentialStatus::RequiresPairing, nullopt, false};
  }
  return {WorkspaceScannerCredentialStatus::FreshCredential, nullopt, false};
}

inline WorkspaceScannerCredentialResolution resolveWorkspaceScannerCredential(const filesystem::path& workspaceDirectory,
                                                                        const InventatoryScanConfig& config) {
  return resolveScannerCredential(
      CredentialStore::lookupForWorkspace(workspaceDirectory, kInventatoryScanTokenCredential), config);
}

inline filesystem::path resolveInventoryDatabasePath(const filesystem::path& selectedPath) {
  error_code error;
  if (selectedPath.empty()) {
    return {};
  }

  if (filesystem::is_regular_file(selectedPath, error) &&
      toLower(selectedPath.extension().u8string()) == ".db") {
    return selectedPath;
  }

  return selectedPath / "inventory.db";
}

inline vector<string> splitFlexible(const string& text) {
  vector<string> values;
  string current;
  for (char ch : text) {
    if (ch == ',' || ch == ';' || ch == '\n') {
      current = trim(current);
      if (!current.empty()) {
        values.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }

  current = trim(current);
  if (!current.empty()) {
    values.push_back(current);
  }

  return values;
}

inline vector<Parameter> parseParameters(const string& text) {
  vector<Parameter> values;
  for (const auto& entry : splitFlexible(text)) {
    const auto equalsPos = entry.find('=');
    if (equalsPos == string::npos) {
      continue;
    }
    values.push_back({trim(entry.substr(0, equalsPos)), trim(entry.substr(equalsPos + 1))});
  }
  return values;
}

inline bool upsertParameter(vector<Parameter>& parameters, const string& name, const string& value) {
  const auto trimmedValue = trim(value);
  if (trimmedValue.empty()) {
    return false;
  }

  for (auto& parameter : parameters) {
    if (parameterLabelMatches(parameter.name, name)) {
      if (parameter.name.empty()) {
        parameter.name = name;
      }
      const bool changed = parameter.value != trimmedValue;
      parameter.value = trimmedValue;
      return changed;
    }
  }

  parameters.push_back({name, trimmedValue});
  return true;
}

// Field-by-field equality of two vendor records. VendorProductMetadata has no operator==, and parameter
// order is part of the record.
inline bool sameVendorRecord(const VendorProductMetadata& left, const VendorProductMetadata& right) {
  if (left.provider != right.provider || left.providerProductNumber != right.providerProductNumber ||
      left.manufacturerPartNumber != right.manufacturerPartNumber || left.categoryId != right.categoryId ||
      left.categoryPath != right.categoryPath || left.title != right.title ||
      left.detailedDescription != right.detailedDescription || left.productUrl != right.productUrl ||
      left.locale != right.locale || left.parameters.size() != right.parameters.size()) {
    return false;
  }
  for (size_t index = 0; index < left.parameters.size(); ++index) {
    if (left.parameters[index].name != right.parameters[index].name ||
        left.parameters[index].value != right.parameters[index].value) {
      return false;
    }
  }
  return true;
}

// Keeps a record from a provider other than the item's selected one, replacing that provider's earlier
// record. Returns true when the stored records changed. A record without a provider or a product number is
// ignored, because it cannot be looked up again.
inline bool upsertVendorSource(InventoryItem& item, const VendorProductMetadata& record) {
  const auto provider = toLower(trim(record.provider));
  if (provider.empty() || trim(record.providerProductNumber).empty()) return false;
  for (auto& existing : item.vendorSources) {
    if (toLower(trim(existing.provider)) != provider) continue;
    if (sameVendorRecord(existing, record)) return false;
    existing = record;
    return true;
  }
  item.vendorSources.push_back(record);
  return true;
}

// Applies a TME product. TME becomes the selected source when the item has none or already uses TME.
// Otherwise the TME record is kept beside the selected source, so neither provider's identity is replaced.
inline bool mergeTmeMetadata(InventoryItem& item, const TmeProductDetails& details) {
  VendorProductMetadata record = details.vendorMetadata;
  record.provider = "tme";
  if (trim(record.providerProductNumber).empty()) return false;
  const auto boundProvider = toLower(trim(item.vendorMetadata.provider));
  if (!boundProvider.empty() && boundProvider != "tme") {
    return upsertVendorSource(item, record);
  }

  bool changed = false;
  // Fills an empty field, or a placeholder the scan or the import left behind, as the DigiKey merge does.
  const auto assignIfUseful = [&](string& target, const string& value, bool replaceUnknown = false) {
    const auto trimmed = trim(value);
    if (trimmed.empty()) return;
    if (target.empty() || (replaceUnknown && (target == "Unknown" || target == "Unsorted" ||
                                              target == "Scanned TME Item"))) {
      target = trimmed;
      changed = true;
    }
  };

  if ((item.partName.empty() || item.partName == "Scanned TME Item") && !trim(details.productDescription).empty()) {
    item.partName = trim(details.productDescription);
    changed = true;
  }
  assignIfUseful(item.manufacturer, details.manufacturerName, true);
  assignIfUseful(item.category, details.categoryName, true);
  assignIfUseful(item.sku, details.vendorMetadata.manufacturerPartNumber);
  assignIfUseful(item.productUrl, details.productUrl);
  assignIfUseful(item.datasheetUrl, details.datasheetUrl);

  const auto replaceText = [&](string& target, const string& value) {
    if (!value.empty() && target != value) {
      target = value;
      changed = true;
    }
  };
  replaceText(item.vendorMetadata.provider, record.provider);
  replaceText(item.vendorMetadata.providerProductNumber, record.providerProductNumber);
  replaceText(item.vendorMetadata.manufacturerPartNumber, record.manufacturerPartNumber);
  replaceText(item.vendorMetadata.categoryId, record.categoryId);
  replaceText(item.vendorMetadata.title, record.title);
  replaceText(item.vendorMetadata.productUrl, record.productUrl);
  replaceText(item.vendorMetadata.locale, record.locale);
  if (!record.categoryPath.empty() && item.vendorMetadata.categoryPath != record.categoryPath) {
    item.vendorMetadata.categoryPath = record.categoryPath;
    changed = true;
  }
  const auto& storedParameters = item.vendorMetadata.parameters;
  const bool parametersDiffer =
      storedParameters.size() != record.parameters.size() ||
      !equal(storedParameters.begin(), storedParameters.end(), record.parameters.begin(),
             [](const Parameter& left, const Parameter& right) {
               return left.name == right.name && left.value == right.value;
             });
  if (!record.parameters.empty() && parametersDiffer) {
    item.vendorMetadata.parameters = record.parameters;
    changed = true;
  }
  // The product's parameters, and the package, stock and price TME gives, also appear on the item, as DigiKey's do.
  for (const auto& parameter : details.parameters) {
    if (upsertParameter(item.parameters, parameter.name, parameter.value)) changed = true;
  }
  if (!trim(details.packageName).empty() && upsertParameter(item.parameters, "Package", details.packageName)) {
    changed = true;
  }
  if (!trim(details.quantityAvailable).empty() &&
      upsertParameter(item.parameters, "Quantity Available", details.quantityAvailable)) {
    changed = true;
  }
  if (!trim(details.unitPrice).empty() && upsertParameter(item.parameters, "Unit Price", details.unitPrice)) {
    changed = true;
  }
  if (!trim(details.productStatus).empty() && upsertParameter(item.parameters, "Product Status", details.productStatus)) {
    changed = true;
  }

  // A matched TME record completes the item's metadata, as a DigiKey record does.
  if (item.syncStatus != "synced") {
    item.syncStatus = "synced";
    changed = true;
  }
  item.lastUpdated = time(nullptr);
  return changed;
}

// Sets or clears the TME symbol of an item. An item with no catalogue of its own becomes a TME part. An item
// bound to DigiKey keeps DigiKey as its selected source and holds the TME symbol beside it, as a merged record does.
inline bool setTmeSymbol(InventoryItem& item, const string& symbol) {
  const auto value = trim(symbol);
  if (value.size() > 64) return false;
  for (const char character : value) {
    const bool allowed = isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_' ||
                         character == '.' || character == '/' || character == '+' || character == '#';
    if (!allowed) return false;
  }
  const auto bound = toLower(trim(item.vendorMetadata.provider));
  if (value.empty()) {
    if (bound == "tme") {
      item.vendorMetadata.provider.clear();
      item.vendorMetadata.providerProductNumber.clear();
      item.vendorMetadata.productUrl.clear();
    }
    item.vendorSources.erase(remove_if(item.vendorSources.begin(), item.vendorSources.end(),
                                       [](const VendorProductMetadata& source) {
                                         return toLower(trim(source.provider)) == "tme";
                                       }),
                             item.vendorSources.end());
    return true;
  }
  if (bound.empty() || bound == "tme") {
    item.vendorMetadata.provider = "tme";
    item.vendorMetadata.providerProductNumber = value;
    item.vendorMetadata.productUrl = tmeProductPageUrl(value);
    return true;
  }
  VendorProductMetadata record;
  record.provider = "tme";
  record.providerProductNumber = value;
  record.productUrl = tmeProductPageUrl(value);
  upsertVendorSource(item, record);
  return true;
}

// What a TME refresh looks up for an item: a TME symbol the item already carries, else its manufacturer part
// number, else its SKU. A SKU is often a supplier order number (a DigiKey code, for example) that TME does not
// know, so the part number comes first. The result is matched exactly.
inline TmeIdentifier tmeRefreshIdentifier(const InventoryItem& item) {
  if (const auto symbol = tmeProductSymbol(item); !symbol.empty()) return {true, symbol};
  if (const auto partNumber = trim(item.vendorMetadata.manufacturerPartNumber); !partNumber.empty()) {
    return {false, partNumber};
  }
  return {false, trim(item.sku)};
}

inline bool mergeDigiKeyMetadata(InventoryItem& item, const DigiKeyProductDetails& details) {
  bool changed = false;

  const auto normalizePackageLabels = [&]() {
    for (auto& parameter : item.parameters) {
      if (parameterLabelMatches(parameter.name, "Package") && looksLikePackagingValue(parameter.value)) {
        parameter.name = "Packaging";
        changed = true;
      }
    }
  };
  normalizePackageLabels();

  const auto assignIfUseful = [&](string& target, const string& value, bool replaceUnknown = false) {
    const auto trimmed = trim(value);
    if (trimmed.empty()) {
      return;
    }
    if (target.empty() || (replaceUnknown && (target == "Unknown" || target == "Unsorted" ||
                                              target == "Scanned DigiKey Item"))) {
      target = trimmed;
      changed = true;
    }
  };

  if ((item.partName.empty() || item.partName == "Scanned DigiKey Item") && !trim(details.productDescription).empty()) {
    item.partName = trim(details.productDescription);
    changed = true;
  }

  assignIfUseful(item.manufacturer, details.manufacturerName, true);
  assignIfUseful(item.category, details.categoryName, true);
  assignIfUseful(item.sku, details.manufacturerPartNumber);
  assignIfUseful(item.productUrl, details.productUrl);
  assignIfUseful(item.datasheetUrl, details.datasheetUrl);

  // The retained provider record is the sole input for deterministic vendor
  // label resolution; direct item fields remain available for the UI.
  // An item already bound to another catalogue provider keeps that identity:
  // replacing it would swap the part its provider number points at. The DigiKey
  // record is kept beside it as a source instead.
  const auto boundProvider = toLower(trim(item.vendorMetadata.provider));
  const bool identityOwnedElsewhere = !boundProvider.empty() && boundProvider != "digikey";
  if (identityOwnedElsewhere) {
    VendorProductMetadata digiKeyRecord = details.vendorMetadata;
    digiKeyRecord.provider = "digikey";
    if (upsertVendorSource(item, digiKeyRecord)) changed = true;
  }
  const auto mergeVendorField = [&](string& target, const string& value) {
    if (identityOwnedElsewhere) {
      return;
    }
    if (!value.empty() && target != value) {
      target = value;
      changed = true;
    }
  };

  mergeVendorField(item.vendorMetadata.provider, details.vendorMetadata.provider);
  mergeVendorField(item.vendorMetadata.providerProductNumber, details.vendorMetadata.providerProductNumber);
  mergeVendorField(item.vendorMetadata.manufacturerPartNumber, details.vendorMetadata.manufacturerPartNumber);
  mergeVendorField(item.vendorMetadata.categoryId, details.vendorMetadata.categoryId);
  mergeVendorField(item.vendorMetadata.title, details.vendorMetadata.title);
  mergeVendorField(item.vendorMetadata.detailedDescription, details.vendorMetadata.detailedDescription);
  mergeVendorField(item.vendorMetadata.productUrl, details.vendorMetadata.productUrl);
  mergeVendorField(item.vendorMetadata.locale, details.vendorMetadata.locale);

  if (!identityOwnedElsewhere && !details.vendorMetadata.categoryPath.empty() &&
      item.vendorMetadata.categoryPath != details.vendorMetadata.categoryPath) {
    item.vendorMetadata.categoryPath = details.vendorMetadata.categoryPath;
    changed = true;
  }

  if (!identityOwnedElsewhere && !details.vendorMetadata.parameters.empty()) {
    const auto& newParams = details.vendorMetadata.parameters;
    const auto& oldParams = item.vendorMetadata.parameters;
    bool differ = oldParams.size() != newParams.size();
    if (!differ) {
      for (size_t i = 0; i < oldParams.size(); ++i) {
        if (oldParams[i].name != newParams[i].name || oldParams[i].value != newParams[i].value) {
          differ = true;
          break;
        }
      }
    }
    if (differ) {
      item.vendorMetadata.parameters = newParams;
      changed = true;
    }
  }

  for (const auto& parameter : details.parameters) {
    if (upsertParameter(item.parameters, parameter.name, parameter.value)) {
      changed = true;
    }
  }

  if (!trim(details.packagingType).empty()) {
    if (upsertParameter(item.parameters, "Packaging", details.packagingType)) {
      changed = true;
    }
  }
  if (!trim(details.packageName).empty()) {
    if (upsertParameter(item.parameters, "Package", details.packageName)) {
      changed = true;
    }
  }
  if (!trim(details.rohsStatus).empty()) {
    if (upsertParameter(item.parameters, "RoHS", details.rohsStatus)) {
      changed = true;
    }
  }
  if (!trim(details.leadStatus).empty()) {
    if (upsertParameter(item.parameters, "Lead Status", details.leadStatus)) {
      changed = true;
    }
  }
  if (!trim(details.productStatus).empty()) {
    if (upsertParameter(item.parameters, "Product Status", details.productStatus)) {
      changed = true;
    }
  }
  if (!trim(details.manufacturerLeadWeeks).empty()) {
    if (upsertParameter(item.parameters, "Lead Time", details.manufacturerLeadWeeks)) {
      changed = true;
    }
  }
  if (!trim(details.quantityAvailable).empty()) {
    if (upsertParameter(item.parameters, "Quantity Available", details.quantityAvailable)) {
      changed = true;
    }
  }
  if (!trim(details.unitPrice).empty()) {
    if (upsertParameter(item.parameters, "Unit Price", details.unitPrice)) {
      changed = true;
    }
  }
  if (!trim(details.detailedDescription).empty() && item.notes.empty()) {
    item.notes = trim(details.detailedDescription);
    changed = true;
  }
  if (!trim(details.lookupKey).empty()) {
    assignIfUseful(item.digikeyPartNumber, details.lookupKey);
  }

  if (item.syncStatus != "synced") {
    item.syncStatus = "synced";
    changed = true;
  }
  item.lastUpdated = time(nullptr);
  return changed;
}

struct DigiKeyApiHandle {
  unique_ptr<DigiKeyApiClient> client;
  string error;
};

inline DigiKeyApiHandle createDigiKeyApi() {
  const auto config = loadDigiKeyConfig();
  if (!config.valid()) {
    return {nullptr, "DigiKey API credentials are not configured"};
  }

  return {make_unique<DigiKeyApiClient>(config), {}};
}

inline string digiKeyRefreshLookup(const InventoryItem& item) {
  const auto provider = toLower(trim(item.vendorMetadata.provider));
  const bool taggedDigiKey = any_of(item.tags.begin(), item.tags.end(), [](const string& tag) {
    return toLower(trim(tag)) == "digikey";
  });

  if (!trim(item.digikeyPartNumber).empty()) {
    return trim(item.digikeyPartNumber);
  }
  if (provider == "digikey" && !trim(item.vendorMetadata.providerProductNumber).empty()) {
    return trim(item.vendorMetadata.providerProductNumber);
  }
  // Older imports may retain only the manufacturer/SKU field.  Use that as a
  // keyword lookup only when the item still carries an explicit DigiKey hint.
  if ((provider == "digikey" || taggedDigiKey) && !trim(item.sku).empty()) {
    return trim(item.sku);
  }
  return {};
}

}  // namespace app_actions

}  // namespace inventatory
