// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Private helpers for the App action source files. Not part of the public interface.

#include "App.h"
#include "core/inventory/VendorBagCode.h"
#include "core/inventory/VendorProvider.h"
#include "platform/security/CredentialStore.h"
#include "platform/digikey/DigiKeyApi.h"
#include "core/parts/PhysicalValue.h"
#include "platform/mouser/MouserApi.h"
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

namespace app_actions {

constexpr std::size_t kDeviceStatusQueueLimit = 256;
constexpr std::uintmax_t kMaximumImportBytes = 25U * 1024U * 1024U;
constexpr const char* kInventatoryScanTokenCredential = "inventatory-scan-pairing-token";

// Retry delay after a failed scanner commit: 2 s, doubling up to 60 s.
inline std::chrono::seconds deviceSyncRetryDelay(int consecutiveFailures) {
  int seconds = 2;
  for (int step = 1; step < consecutiveFailures && seconds < 60; ++step) seconds *= 2;
  return std::chrono::seconds(std::min(seconds, 60));
}

// The names a scan gives a part until its supplier lookup fills in the details.
inline bool isScanPlaceholderName(const std::string& name) {
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
  std::optional<std::string> token;
  // Store unavailable (locked or no keyring): the scanner stays disabled, but the reason is shown.
  bool credentialStoreUnavailable = false;
};

inline bool validScannerToken(const std::string& token) {
  return token.size() == 64U &&
         std::all_of(token.begin(), token.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

inline WorkspaceScannerCredentialResolution resolveScannerCredential(const CredentialLookup& lookup,
                                                                     const InventatoryScanConfig& config) {
  if (lookup.status == CredentialReadStatus::Unavailable) {
    // Token existence is unknown. Never mint a fresh one: it would replace the real token.
    return {WorkspaceScannerCredentialStatus::RequiresPairing, std::nullopt, true};
  }
  const auto& scopedCredential = lookup.secret;
  if (lookup.status == CredentialReadStatus::Found && scopedCredential.has_value() &&
      validScannerToken(*scopedCredential)) {
    return {WorkspaceScannerCredentialStatus::Loaded, scopedCredential, false};
  }
  if (lookup.status == CredentialReadStatus::Found || config.setupComplete || !trim(config.deviceId).empty()) {
    return {WorkspaceScannerCredentialStatus::RequiresPairing, std::nullopt, false};
  }
  return {WorkspaceScannerCredentialStatus::FreshCredential, std::nullopt, false};
}

inline WorkspaceScannerCredentialResolution resolveWorkspaceScannerCredential(
    const std::filesystem::path& workspaceDirectory, const InventatoryScanConfig& config) {
  return resolveScannerCredential(
      CredentialStore::lookupForWorkspace(workspaceDirectory, kInventatoryScanTokenCredential), config);
}

inline std::filesystem::path resolveInventoryDatabasePath(const std::filesystem::path& selectedPath) {
  std::error_code error;
  if (selectedPath.empty()) {
    return {};
  }

  if (std::filesystem::is_regular_file(selectedPath, error) &&
      toLower(selectedPath.extension().u8string()) == ".db") {
    return selectedPath;
  }

  return selectedPath / "inventory.db";
}

inline std::vector<std::string> splitFlexible(const std::string& text) {
  std::vector<std::string> values;
  std::string current;
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

inline std::vector<Parameter> parseParameters(const std::string& text) {
  std::vector<Parameter> values;
  for (const auto& entry : splitFlexible(text)) {
    const auto equalsPos = entry.find('=');
    if (equalsPos == std::string::npos) {
      continue;
    }
    values.push_back({trim(entry.substr(0, equalsPos)), trim(entry.substr(equalsPos + 1))});
  }
  return values;
}

// Names of the package case. A supplier's device package is a separate parameter.
inline bool isPackageParameterName(const std::string& name) {
  const auto key = normalizeKey(name);
  return key == "package" || key == "packagecase" || key == "casepackage";
}

// Only the same parameter is replaced: DigiKey's "Lens Color" must not overwrite "Color".
inline bool isSameParameterName(const std::string& stored, const std::string& name) {
  const auto key = normalizeKey(name);
  if (!key.empty() && normalizeKey(stored) == key) return true;
  return isPackageParameterName(stored) && isPackageParameterName(name);
}

inline bool upsertParameter(std::vector<Parameter>& parameters, const std::string& name, const std::string& value) {
  const auto trimmedValue = trim(value);
  if (trimmedValue.empty()) {
    return false;
  }

  for (auto& parameter : parameters) {
    if (isSameParameterName(parameter.name, name)) {
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

// VendorProductMetadata has no operator==. Parameter order is part of the record.
inline bool sameVendorRecord(const VendorProductMetadata& left, const VendorProductMetadata& right) {
  if (left.provider != right.provider || left.providerProductNumber != right.providerProductNumber ||
      left.manufacturerPartNumber != right.manufacturerPartNumber || left.categoryId != right.categoryId ||
      left.categoryPath != right.categoryPath || left.title != right.title ||
      left.detailedDescription != right.detailedDescription || left.productUrl != right.productUrl ||
      left.locale != right.locale || left.parameters.size() != right.parameters.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.parameters.size(); ++index) {
    if (left.parameters[index].name != right.parameters[index].name ||
        left.parameters[index].value != right.parameters[index].value) {
      return false;
    }
  }
  return true;
}

// Keeps one record per provider, replacing that provider's earlier record. Returns true if changed.
// Ignores a record without a provider or product number, as it cannot be looked up again.
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

// Merges a TME or Mouser catalogue record. The supplier becomes the item's source only if none is
// set or it already is; otherwise the record is kept as another source. scanPlaceholder is the part
// name a scan gives an unknown part, which the record may replace; empty when there is none.
template <typename Details>
bool mergeCatalogueMetadata(InventoryItem& item, const Details& details, const char* provider,
                            const std::string& scanPlaceholder) {
  VendorProductMetadata record = details.vendorMetadata;
  record.provider = provider;
  if (trim(record.providerProductNumber).empty()) return false;
  const auto boundProvider = toLower(trim(item.vendorMetadata.provider));
  if (!boundProvider.empty() && boundProvider != provider) {
    return upsertVendorSource(item, record);
  }

  bool changed = false;
  // Fills an empty field or a placeholder left by a scan or import.
  const auto assignIfUseful = [&](std::string& target, const std::string& value, bool replaceUnknown = false) {
    const auto trimmed = trim(value);
    if (trimmed.empty()) return;
    if (target.empty() ||
        (replaceUnknown && (target == "Unknown" || target == "Unsorted" || target == scanPlaceholder))) {
      target = trimmed;
      changed = true;
    }
  };

  if ((item.partName.empty() || item.partName == scanPlaceholder) && !trim(details.productDescription).empty()) {
    item.partName = trim(details.productDescription);
    changed = true;
  }
  assignIfUseful(item.manufacturer, details.manufacturerName, true);
  assignIfUseful(item.category, details.categoryName, true);
  assignIfUseful(item.sku, details.vendorMetadata.manufacturerPartNumber);
  assignIfUseful(item.productUrl, details.productUrl);
  assignIfUseful(item.datasheetUrl, details.datasheetUrl);

  const auto replaceText = [&](std::string& target, const std::string& value) {
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
      !std::equal(storedParameters.begin(), storedParameters.end(), record.parameters.begin(),
                  [](const Parameter& left, const Parameter& right) {
                    return left.name == right.name && left.value == right.value;
                  });
  if (!record.parameters.empty() && parametersDiffer) {
    item.vendorMetadata.parameters = record.parameters;
    changed = true;
  }
  // Also copies the record's parameters, package, stock and price onto the item.
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

  // A matched record marks the item as synced.
  if (item.syncStatus != "synced") {
    item.syncStatus = "synced";
    changed = true;
  }
  item.lastUpdated = time(nullptr);
  return changed;
}

inline bool mergeTmeMetadata(InventoryItem& item, const TmeProductDetails& details) {
  return mergeCatalogueMetadata(item, details, kVendorProviderTme, "Scanned TME Item");
}

// Sets or clears the TME symbol. An item bound to another provider keeps that as its source.
inline bool setTmeSymbol(InventoryItem& item, const std::string& symbol) {
  const auto value = trim(symbol);
  if (value.size() > 64) return false;
  for (const char character : value) {
    const bool allowed = std::isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_' ||
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
    item.vendorSources.erase(std::remove_if(item.vendorSources.begin(), item.vendorSources.end(),
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

// Lookup order: TME symbol, manufacturer part number, then SKU. A SKU is often a supplier order
// number that TME does not know, so it comes last.
inline TmeIdentifier tmeRefreshIdentifier(const InventoryItem& item) {
  // Only a part that came from TME is refreshed from TME.
  if (originProvider(item) != "tme") return {};
  if (const auto symbol = tmeProductSymbol(item); !symbol.empty()) return {true, symbol, {}};
  if (const auto partNumber = trim(item.vendorMetadata.manufacturerPartNumber); !partNumber.empty()) {
    return {false, partNumber, trim(item.manufacturer)};
  }
  return {false, trim(item.sku), trim(item.manufacturer)};
}

// A TME record's resistance, capacitance or inductance must match the stored value, or it is a
// different part with the same part number. Returns the disagreeing parameter's name, or empty.
inline std::string tmeRecordConflict(const InventoryItem& item, const TmeProductDetails& details) {
  for (const auto& parameter : details.parameters) {
    if (parameter.name != "Resistance" && parameter.name != "Capacitance" && parameter.name != "Inductance") continue;
    for (const auto& stored : item.parameters) {
      if (stored.name != parameter.name || trim(stored.value).empty()) continue;
      const auto comparison = comparePhysicalValues(parameter.value, stored.value);
      if (comparison && comparison->band == PhysicalValueMatchBand::None) return parameter.name;
    }
  }
  return {};
}

inline bool mergeMouserMetadata(InventoryItem& item, const MouserProductDetails& details) {
  return mergeCatalogueMetadata(item, details, kVendorProviderMouser, "");
}

// Lookup order: Mouser part number, manufacturer part number, then SKU.
inline std::string mouserRefreshLookup(const InventoryItem& item) {
  // Only a part that came from Mouser is refreshed from Mouser.
  if (originProvider(item) != kVendorProviderMouser) return {};
  if (const auto partNumber = mouserPartNumberOf(item); !partNumber.empty()) return partNumber;
  if (const auto partNumber = trim(item.vendorMetadata.manufacturerPartNumber); !partNumber.empty()) return partNumber;
  return trim(item.sku);
}

// A record with another manufacturer part number is a different part. Case and punctuation are
// ignored. Returns the disagreeing field, or empty if the numbers agree or either is missing.
inline std::string mouserRecordConflict(const InventoryItem& item, const MouserProductDetails& details) {
  const auto stored = normalizeKey(item.vendorMetadata.manufacturerPartNumber);
  const auto incoming = normalizeKey(details.vendorMetadata.manufacturerPartNumber);
  if (stored.empty() || incoming.empty() || stored == incoming) return {};
  return "manufacturer part number";
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

  const auto assignIfUseful = [&](std::string& target, const std::string& value, bool replaceUnknown = false) {
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

  // An item bound to another provider keeps that identity: replacing it would swap the part its
  // provider number points at. The DigiKey record is kept beside it as a source.
  const auto boundProvider = toLower(trim(item.vendorMetadata.provider));
  const bool identityOwnedElsewhere = !boundProvider.empty() && boundProvider != "digikey";
  if (identityOwnedElsewhere) {
    VendorProductMetadata digiKeyRecord = details.vendorMetadata;
    digiKeyRecord.provider = "digikey";
    if (upsertVendorSource(item, digiKeyRecord)) changed = true;
  }
  const auto mergeVendorField = [&](std::string& target, const std::string& value) {
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
      for (std::size_t i = 0; i < oldParams.size(); ++i) {
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
  std::unique_ptr<DigiKeyApiClient> client;
  std::string error;
};

inline DigiKeyApiHandle createDigiKeyApi() {
  const auto config = loadDigiKeyConfig();
  if (!config.valid()) {
    return {nullptr, "DigiKey API credentials are not configured"};
  }

  return {std::make_unique<DigiKeyApiClient>(config), {}};
}

inline std::string digiKeyRefreshLookup(const InventoryItem& item) {
  // Only a part that came from DigiKey is refreshed from DigiKey.
  if (originProvider(item) != "digikey") return {};
  const auto provider = toLower(trim(item.vendorMetadata.provider));
  if (!trim(item.digikeyPartNumber).empty()) {
    return trim(item.digikeyPartNumber);
  }
  if (provider == "digikey" && !trim(item.vendorMetadata.providerProductNumber).empty()) {
    return trim(item.vendorMetadata.providerProductNumber);
  }
  // Older imports may keep only the SKU, which is the DigiKey number.
  if (provider == "digikey" && !trim(item.sku).empty()) {
    return trim(item.sku);
  }
  return {};
}

}  // namespace app_actions

}  // namespace inventatory
