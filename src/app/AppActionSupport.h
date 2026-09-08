#pragma once

// Private helpers shared by the focused App action translation units. This
// header is not part of the public application interface.

#include "App.h"
#include "platform/CredentialStore.h"
#include "platform/DigiKeyApi.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

namespace app_actions {

constexpr size_t kDeviceDebugWindowLines = 14;
constexpr size_t kDeviceStatusQueueLimit = 256;
constexpr size_t kDeviceDebugQueueLimit = 512;
constexpr size_t kScanQueueLimit = 256;
constexpr size_t kDeviceQuantityQueueLimit = 64;
constexpr uintmax_t kMaximumImportBytes = 25U * 1024U * 1024U;
constexpr const char* kInventatoryScanTokenCredential = "inventatory-scan-pairing-token";

enum class WorkspaceScannerCredentialStatus {
  Loaded,
  FreshCredential,
  RequiresPairing,
};

struct WorkspaceScannerCredentialResolution {
  WorkspaceScannerCredentialStatus status = WorkspaceScannerCredentialStatus::RequiresPairing;
  optional<string> token;
};

inline bool validScannerToken(const string& token) {
  return token.size() == 64U &&
         all_of(token.begin(), token.end(), [](unsigned char ch) { return isxdigit(ch) != 0; });
}

inline WorkspaceScannerCredentialResolution resolveWorkspaceScannerCredential(const filesystem::path& workspaceDirectory,
                                                                        const InventatoryScanConfig& config) {
  const auto scopedCredential =
      CredentialStore::readForWorkspace(workspaceDirectory, kInventatoryScanTokenCredential);
  if (scopedCredential.has_value() && validScannerToken(*scopedCredential)) {
    return {WorkspaceScannerCredentialStatus::Loaded, scopedCredential};
  }
  if (scopedCredential.has_value() || config.setupComplete || !trim(config.deviceId).empty()) {
    return {WorkspaceScannerCredentialStatus::RequiresPairing, nullopt};
  }
  return {WorkspaceScannerCredentialStatus::FreshCredential, nullopt};
}

inline filesystem::path resolveInventoryDatabasePath(const filesystem::path& selectedPath) {
  error_code error;
  if (selectedPath.empty()) {
    return {};
  }

  if (filesystem::is_regular_file(selectedPath, error) &&
      toLower(selectedPath.extension().string()) == ".db") {
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
  item.vendorMetadata = details.vendorMetadata;
  changed = true;

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
