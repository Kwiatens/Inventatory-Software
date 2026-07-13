// HIMS - Hardware Inventory Management System
// Application bootstrap helpers for data paths and database reuse.

#include "App.h"

#include <cstdlib>
#include <system_error>

namespace hims {

using namespace std;

filesystem::path documentsHimsPath() {
  if (const char* profile = getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    return filesystem::path(profile) / "Documents" / "HIMS";
  }
  return filesystem::current_path() / "Documents" / "HIMS";
}

filesystem::path discoverHimsDataPath() {
  error_code error;
  const auto defaultPath = documentsHimsPath();
  vector<filesystem::path> candidates = {defaultPath};
  const auto addCandidate = [&](const char* envName) {
    if (const char* value = getenv(envName); value != nullptr && *value != '\0') {
      candidates.push_back(filesystem::path(value) / "Documents" / "HIMS");
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

}  // namespace hims
