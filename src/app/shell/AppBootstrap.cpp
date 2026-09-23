// Inventatory - Hardware Inventory Management System
// Application bootstrap helpers for data paths and database reuse.

#include "app/shell/AppBootstrap.h"

#include "platform/system/Environment.h"

#include <system_error>

namespace inventatory {

using namespace std;

filesystem::path documentsInventatoryPath() {
#ifdef _WIN32
  if (const auto profile = environmentValue("USERPROFILE"); profile.has_value() && !profile->empty()) {
    return filesystem::path(*profile) / "Documents" / "Inventatory";
  }
  return filesystem::current_path() / "Documents" / "Inventatory";
#else
  const auto home = environmentValue("HOME");
  const auto dataHome = environmentValue("XDG_DATA_HOME");
  if (dataHome.has_value() && !dataHome->empty() && filesystem::path(*dataHome).is_absolute()) {
    return filesystem::path(*dataHome) / "Inventatory";
  }
  if (home.has_value() && !home->empty()) return filesystem::path(*home) / ".local" / "share" / "Inventatory";
  return filesystem::current_path() / ".local" / "share" / "Inventatory";
#endif
}

filesystem::path discoverInventatoryDataPath() {
  error_code error;
  const auto defaultPath = documentsInventatoryPath();
  vector<filesystem::path> candidates = {defaultPath};
#ifdef _WIN32
  const auto addCandidate = [&](const char* envName) {
    if (const auto value = environmentValue(envName); value.has_value() && !value->empty()) {
      candidates.push_back(filesystem::path(*value) / "Documents" / "Inventatory");
    }
  };
  addCandidate("OneDrive");
  addCandidate("OneDriveConsumer");
  addCandidate("OneDriveCommercial");
#else
  // Keep a pre-existing Windows-era Documents workspace in place on the first
  // Linux launch. Never copy or migrate its contents implicitly.
  if (const auto home = environmentValue("HOME"); home.has_value() && !home->empty()) {
    candidates.push_back(filesystem::path(*home) / "Documents" / "Inventatory");
  }
#endif

  for (const auto& candidate : candidates) {
    const bool inventoryExists = filesystem::exists(candidate / "inventory.db", error);
    if (error) {
      // Preserve the candidate path so App::loadState() can surface the
      // inspection error instead of silently selecting an empty workspace.
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
