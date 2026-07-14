// Inventatory - Hardware Inventory Management System
// Application bootstrap helpers for data paths and database reuse.

#include "App.h"

#include <cstdlib>
#include <system_error>

namespace inventatory {

using namespace std;

filesystem::path documentsInventatoryPath() {
  if (const char* profile = getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    return filesystem::path(profile) / "Documents" / "Inventatory";
  }
  return filesystem::current_path() / "Documents" / "Inventatory";
}

filesystem::path discoverInventatoryDataPath() {
  error_code error;
  const auto defaultPath = documentsInventatoryPath();
  vector<filesystem::path> candidates = {defaultPath};
  vector<filesystem::path> legacyCandidates;
  if (const char* profile = getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    legacyCandidates.push_back(filesystem::path(profile) / "Documents" / "HIMS");
  } else {
    legacyCandidates.push_back(filesystem::current_path() / "Documents" / "HIMS");
  }
  const auto addCandidate = [&](const char* envName) {
    if (const char* value = getenv(envName); value != nullptr && *value != '\0') {
      candidates.push_back(filesystem::path(value) / "Documents" / "Inventatory");
      legacyCandidates.push_back(filesystem::path(value) / "Documents" / "HIMS");
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

filesystem::path legacyDatabasePath() {
  const auto githubRoot = filesystem::current_path().parent_path().parent_path();
  return githubRoot / "Kwiatens Stock Management System" / "KwiatensStockManagementSystem" / "data" / "kwiatens-stock.db";
}

void copyDatabaseSidecar(const filesystem::path& sourceBase, const filesystem::path& destinationBase,
                         const string& suffix) {
  const auto source = filesystem::path(sourceBase.string() + suffix);
  const auto destination = filesystem::path(destinationBase.string() + suffix);
  error_code error;
  if (filesystem::exists(source, error)) {
    filesystem::copy_file(source, destination, filesystem::copy_options::overwrite_existing, error);
  }
}

void ensureInventoryDatabaseCopied(const filesystem::path& localBase) {
  error_code error;
  if (filesystem::exists(localBase, error)) {
    return;
  }

  const auto sourceBase = legacyDatabasePath();
  if (!filesystem::exists(sourceBase, error)) {
    return;
  }

  filesystem::create_directories(localBase.parent_path(), error);
  filesystem::copy_file(sourceBase, localBase, filesystem::copy_options::overwrite_existing, error);
  copyDatabaseSidecar(sourceBase, localBase, "-wal");
  copyDatabaseSidecar(sourceBase, localBase, "-shm");
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

}  // namespace inventatory
