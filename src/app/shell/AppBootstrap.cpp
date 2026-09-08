// Inventatory - Hardware Inventory Management System
// Application bootstrap helpers for data paths and database reuse.

#include "app/shell/AppBootstrap.h"

#include "platform/system/Environment.h"

#include <system_error>

namespace inventatory {

using namespace std;

filesystem::path documentsInventatoryPath() {
  if (const auto profile = environmentValue("USERPROFILE"); profile.has_value() && !profile->empty()) {
    return filesystem::path(*profile) / "Documents" / "Inventatory";
  }
  return filesystem::current_path() / "Documents" / "Inventatory";
}

filesystem::path discoverInventatoryDataPath() {
  error_code error;
  const auto defaultPath = documentsInventatoryPath();
  vector<filesystem::path> candidates = {defaultPath};
  const auto addCandidate = [&](const char* envName) {
    if (const auto value = environmentValue(envName); value.has_value() && !value->empty()) {
      candidates.push_back(filesystem::path(*value) / "Documents" / "Inventatory");
    }
  };
  addCandidate("OneDrive");
  addCandidate("OneDriveConsumer");
  addCandidate("OneDriveCommercial");

  for (const auto& candidate : candidates) {
    if (filesystem::exists(candidate / "inventory.db", error)) {
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
