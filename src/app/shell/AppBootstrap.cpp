// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System

#include "app/shell/AppBootstrap.h"

#include "platform/system/Environment.h"

#include <system_error>

namespace inventatory {

using namespace std;

filesystem::path documentsInventatoryPath() {
#ifdef _WIN32
  if (const auto profile = environmentValue("USERPROFILE"); profile.has_value() && !profile->empty()) {
    return filesystem::u8path(*profile) / "Documents" / "Inventatory";
  }
  return filesystem::current_path() / "Documents" / "Inventatory";
#else
  return xdgDataHome() / "Inventatory";
#endif
}

filesystem::path discoverInventatoryDataPath() {
  error_code error;
  const auto defaultPath = documentsInventatoryPath();
  vector<filesystem::path> candidates = {defaultPath};
#ifdef _WIN32
  const auto addCandidate = [&](const char* envName) {
    if (const auto value = environmentValue(envName); value.has_value() && !value->empty()) {
      candidates.push_back(filesystem::u8path(*value) / "Documents" / "Inventatory");
    }
  };
  addCandidate("OneDrive");
  addCandidate("OneDriveConsumer");
  addCandidate("OneDriveCommercial");
#else
  // Reuse a Windows-era Documents workspace. Its contents are never copied or migrated.
  if (const auto home = environmentValue("HOME"); home.has_value() && !home->empty()) {
    candidates.push_back(filesystem::path(*home) / "Documents" / "Inventatory");
  }
#endif

  for (const auto& candidate : candidates) {
    const bool inventoryExists = filesystem::exists(candidate / "inventory.db", error);
    if (error) {
      // Return the candidate so loadState() reports the error, not an empty workspace.
      return candidate;
    }
    if (inventoryExists) {
      return candidate;
    }
  }

  return defaultPath;
}

InventatoryDataPaths makeInventatoryDataPaths(const filesystem::path& dataDirectory) {
  return {dataDirectory, dataDirectory / "inventory.db", dataDirectory / "printer.conf",
          dataDirectory / "activity.tsv", dataDirectory / "inventatory_scan.conf"};
}

bool switchInventatoryDataPathsAfterSaving(InventatoryDataPaths& active, const filesystem::path& nextDataDirectory,
                                           const function<bool()>& saveCurrentState) {
  if (!saveCurrentState()) return false;
  active = makeInventatoryDataPaths(nextDataDirectory);
  return true;
}

bool onboardingRequired(bool startInBackground, bool settingsLoaded, int completedOnboardingVersion) {
  return !startInBackground && (!settingsLoaded || completedOnboardingVersion < 1);
}

}  // namespace inventatory
