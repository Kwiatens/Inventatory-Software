// Inventatory - Hardware Inventory Management System
// Application bootstrap helpers for data paths and database reuse.

#include "app/AppBootstrap.h"

#include "platform/Environment.h"

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
  vector<filesystem::path> legacyCandidates;
  if (const auto profile = environmentValue("USERPROFILE"); profile.has_value() && !profile->empty()) {
    legacyCandidates.push_back(filesystem::path(*profile) / "Documents" / "HIMS");
  } else {
    legacyCandidates.push_back(filesystem::current_path() / "Documents" / "HIMS");
  }
  const auto addCandidate = [&](const char* envName) {
    if (const auto value = environmentValue(envName); value.has_value() && !value->empty()) {
      candidates.push_back(filesystem::path(*value) / "Documents" / "Inventatory");
      legacyCandidates.push_back(filesystem::path(*value) / "Documents" / "HIMS");
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

  for (const auto& legacyCandidate : legacyCandidates) {
    if (!filesystem::exists(legacyCandidate / "inventory.db", error)) continue;
    filesystem::create_directories(defaultPath, error);
    if (error) break;
    filesystem::copy(legacyCandidate, defaultPath,
                     filesystem::copy_options::recursive | filesystem::copy_options::skip_existing, error);
    if (!error && filesystem::exists(defaultPath / "inventory.db", error)) return defaultPath;
    error.clear();
  }

  return defaultPath;
}

filesystem::path locateDotEnvFile() {
  error_code error;
  auto current = filesystem::current_path();
  for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
    const auto candidate = current / ".env";
    if (filesystem::exists(candidate, error)) {
      return candidate;
    }
    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  return {};
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

}  // namespace inventatory
