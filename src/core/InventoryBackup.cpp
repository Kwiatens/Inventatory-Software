// Inventatory - Inventory backup creation and validation.

// Inventatory - Inventory export and backup/restore workflows.

#include "core/InventoryTransferPrivate.h"

#include "app/AppSettings.h"
#include "core/BomProjectStore.h"
#include "core/InventorySqlite.h"
#include "label_printer/LabelPrinter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#endif

namespace inventatory {
using namespace std;

using namespace inventory_transfer_detail;

bool createInventatoryBackup(const filesystem::path& dataDirectory, const filesystem::path& appSettingsPath,
                             const filesystem::path& destinationDirectory, const string& applicationVersion,
                             string& error, const InventoryTransferTestHooks* testHooks) {
  error.clear();
  error_code filesystemError;
  if (!filesystem::is_directory(dataDirectory, filesystemError) || filesystemError) {
    error = "Inventatory data folder does not exist: " + dataDirectory.string();
    return false;
  }
  if (isLinkedOrReparseArtifact(dataDirectory, filesystemError) || filesystemError) {
    error = "Inventatory data folder must not be a link or reparse point";
    return false;
  }
  const auto inventorySource = dataDirectory / "inventory.db";
  if (isLinkedOrReparseArtifact(inventorySource, filesystemError) || filesystemError ||
      !filesystem::is_regular_file(inventorySource, filesystemError) || filesystemError) {
    error = "Inventory database is missing";
    return false;
  }
  if (filesystem::exists(destinationDirectory, filesystemError)) {
    error = "Backup destination already exists";
    return false;
  }
  if (filesystemError) {
    error = "Unable to inspect backup destination: " + filesystemError.message();
    return false;
  }
  bool overlap = false;
  if (!pathsOverlap(dataDirectory, destinationDirectory, overlap, error)) return false;
  if (overlap) {
    error = "Backup destination overlaps the active data directory";
    return false;
  }
  if (!pathsOverlap(destinationDirectory, appSettingsPath, overlap, error)) return false;
  if (overlap) {
    error = "Backup destination overlaps application settings";
    return false;
  }
  if (!pathsOverlap(dataDirectory, appSettingsPath, overlap, error)) return false;
  if (overlap) {
    error = "Application settings overlap the active data directory";
    return false;
  }
  if (applicationVersion.empty() || containsLineBreakOrTab(applicationVersion)) {
    error = "Backup application version is invalid";
    return false;
  }

  // Refuse oversized source files before opening SQLite's online-backup or
  // copying optional sidecars.  Validation repeats the bound on the staged
  // snapshot to cover growth races, but this preflight prevents an obviously
  // oversized local workspace from consuming staging disk and hash time.
  uintmax_t sourceAggregateSize = 0;
  const auto checkSourceSize = [&](const filesystem::path& source, const string& name) {
    error_code sourceSizeError;
    const auto size = filesystem::file_size(source, sourceSizeError);
    if (sourceSizeError || size > kMaximumBackupPayloadBytes ||
        sourceAggregateSize > kMaximumBackupPayloadBytes - size) {
      error = "Backup source payload is too large: " + name;
      return false;
    }
    sourceAggregateSize += size;
    return true;
  };
  if (!checkSourceSize(inventorySource, "inventory.db")) return false;
  const vector<string> optionalNames = {"activity.tsv", "printer.conf", "quick_labels.conf"};
  for (const auto& name : optionalNames) {
    const auto source = dataDirectory / name;
    const auto sourceStatus = filesystem::symlink_status(source, filesystemError);
    if (filesystemError && sourceStatus.type() != filesystem::file_type::not_found) {
      error = "Unable to inspect backup source file: " + name;
      return false;
    }
    filesystemError.clear();
    if (sourceStatus.type() == filesystem::file_type::symlink ||
        (sourceStatus.type() != filesystem::file_type::not_found &&
         isLinkedOrReparseArtifact(source, filesystemError))) {
      error = "Backup source contains an invalid optional file: " + name;
      return false;
    }
    if (filesystemError) {
      error = "Unable to inspect backup source file: " + name;
      return false;
    }
    const bool sourceExists = filesystem::exists(source, filesystemError);
    if (filesystemError) {
      error = "Unable to inspect backup source file: " + name;
      return false;
    }
    if (!sourceExists) continue;
    if (!filesystem::is_regular_file(source, filesystemError) || filesystemError) {
      error = "Backup source contains an invalid optional file: " + name;
      return false;
    }
    if (!checkSourceSize(source, name)) return false;
  }

  AppSettings settings;
  if (!loadAppSettings(appSettingsPath, settings)) {
    error = "Unable to read application settings for backup";
    return false;
  }
  settings.dataDirectory = filesystem::path(".");

  filesystem::path staging;
  if (!createUniqueDirectory(destinationDirectory, ".backup-staging", staging, error)) return false;
  const TransferOps ops{testHooks};
  const auto fail = [&](const string& primary) {
    string cleanupError;
    ops.removeAll(staging, cleanupError);
    error = primary;
    return false;
  };
  if (!saveAppSettings(staging / "settings.conf", settings)) return fail("Unable to write sanitized backup settings");
  string snapshotError;
  if (!createSqliteSnapshot(dataDirectory / "inventory.db", staging / "inventory.db", &snapshotError)) {
    return fail(snapshotError.empty() ? "Unable to snapshot inventory database" : snapshotError);
  }
  for (const auto& name : optionalNames) {
    const auto source = dataDirectory / name;
    const bool sourceExists = filesystem::exists(source, filesystemError);
    if (filesystemError) return fail("Unable to inspect backup source file: " + name);
    if (!sourceExists) {
      filesystemError.clear();
      continue;
    }
    if (filesystem::is_symlink(source, filesystemError) || filesystemError ||
        !filesystem::is_regular_file(source, filesystemError) || filesystemError) {
      return fail("Backup source contains an invalid optional file: " + name);
    }
    if (!ops.copy(source, staging / name, error)) return fail(error);
  }

  const vector<string> names = {"inventory.db", "activity.tsv", "printer.conf", "quick_labels.conf", "settings.conf"};
  vector<BackupEntry> entries;
  for (const auto& name : names) {
    const auto target = staging / name;
    filesystemError.clear();
    const bool targetExists = filesystem::exists(target, filesystemError);
    if (filesystemError) return fail("Unable to inspect staged backup file: " + name);
    if (!targetExists) continue;
    const bool targetIsFile = filesystem::is_regular_file(target, filesystemError);
    if (filesystemError) return fail("Unable to inspect staged backup file: " + name);
    if (!targetIsFile) return fail("Staged backup entry is not a regular file: " + name);
    BackupEntry entry;
    entry.name = name;
    entry.size = filesystem::file_size(target, filesystemError);
    if (filesystemError || !sha256File(target, entry.hash, error)) return fail(error);
    entries.push_back(move(entry));
  }
  if (!writeManifest(staging, applicationVersion, entries, error)) return fail(error);
  // The manifest is written only after every staged file is complete; this
  // validation is read-only and therefore cannot migrate or create the DB.
  if (!validateInventatoryBackup(staging, error)) return fail(error);
  if (!ops.rename(staging, destinationDirectory, error)) return fail(error);
  return true;
}

bool validateInventatoryBackup(const filesystem::path& backupDirectory, string& error) {
  try {
  error.clear();
  error_code filesystemError;
  if (isLinkedOrReparseArtifact(backupDirectory, filesystemError) || filesystemError ||
      !filesystem::is_directory(backupDirectory, filesystemError) || filesystemError) {
    error = "Backup folder does not exist";
    return false;
  }
  vector<BackupEntry> entries;
  return readManifest(backupDirectory, entries, error) && validateEntries(backupDirectory, entries, error);
  } catch (const filesystem::filesystem_error& exception) {
    error = string("Backup validation filesystem error: ") + exception.what();
    return false;
  } catch (const exception& exception) {
    error = string("Backup validation error: ") + exception.what();
    return false;
  }
}

}  // namespace inventatory
