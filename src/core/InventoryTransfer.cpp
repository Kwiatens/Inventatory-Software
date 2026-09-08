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

namespace {
string csvQuote(const string& value) {
  string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') escaped.push_back('"');
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

string rackLocationForExport(const InventoryItem& item, const InventoryStore& store) {
  const auto location = rackLocation(item, store.racks());
  return location.empty() ? item.location : location;
}
}  // namespace

using namespace inventory_transfer_detail;

bool exportInventoryCsv(const InventoryStore& store, const filesystem::path& path, string& error) {
  ofstream output(path, ios::binary | ios::trunc);
  if (!output) {
    error = "Unable to create " + path.string();
    return false;
  }

  output << "ID,Part name,Manufacturer,Category,Quantity,Location,SKU,Machine code,"
            "DigiKey part,Sync status,Tags,Parameters,Notes,Datasheet URL,Product URL,Last updated\r\n";
  for (const auto& item : store.items()) {
    output << csvQuote(item.id) << ',' << csvQuote(item.partName) << ',' << csvQuote(item.manufacturer) << ','
           << csvQuote(item.category) << ',' << item.quantity << ','
           << csvQuote(rackLocationForExport(item, store)) << ',' << csvQuote(item.sku) << ','
           << csvQuote(item.machineCode) << ',' << csvQuote(item.digikeyPartNumber) << ','
           << csvQuote(item.syncStatus) << ',' << csvQuote(join(item.tags, ';')) << ','
           << csvQuote(serializeParametersForStorage(item.parameters)) << ',' << csvQuote(item.notes) << ','
           << csvQuote(item.datasheetUrl) << ',' << csvQuote(item.productUrl) << ','
           << csvQuote(nowTimestampString(item.lastUpdated)) << "\r\n";
  }

  output.close();
  if (!output) {
    error = "Unable to finish writing " + path.string();
    return false;
  }
  return true;
}

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

bool restoreInventatoryBackup(const filesystem::path& backupDirectory, const filesystem::path& destinationDirectory,
                              const filesystem::path& appSettingsPath, string& error,
                              const InventoryTransferTestHooks* testHooks,
                              bool* replacementWorkspaceActiveOnFailure) {
  try {
  error.clear();
  if (replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
  const TransferOps ops{testHooks};
  bool overlap = false;
  if (!pathsOverlap(backupDirectory, destinationDirectory, overlap, error)) return false;
  if (overlap) {
    error = "Backup source overlaps restore destination";
    return false;
  }
  if (!pathsOverlap(backupDirectory, appSettingsPath, overlap, error)) return false;
  if (overlap) {
    error = "Backup source overlaps application settings";
    return false;
  }
  if (!pathsOverlap(destinationDirectory, appSettingsPath, overlap, error)) return false;
  if (overlap) {
    error = "Restore destination overlaps application settings";
    return false;
  }
  const auto journalPath = restoreJournalPath(appSettingsPath);
  error_code journalError;
  if (filesystem::exists(journalPath, journalError)) {
    error = "Another restore is pending recovery";
    return false;
  }
  if (journalError) {
    error = "Unable to inspect restore journal: " + journalError.message();
    return false;
  }
  if (!validateInventatoryBackup(backupDirectory, error)) return false;
  vector<BackupEntry> entries;
  if (!readManifest(backupDirectory, entries, error)) return false;

  error_code filesystemError;
  const bool destinationExists = filesystem::exists(destinationDirectory, filesystemError);
  if (filesystemError) {
    error = "Unable to inspect restore destination: " + filesystemError.message();
    return false;
  }
  if (destinationExists && (isLinkedOrReparseArtifact(destinationDirectory, filesystemError) ||
                            !filesystem::is_directory(destinationDirectory, filesystemError))) {
    error = "Restore destination is not a directory";
    return false;
  }
  if (filesystemError) {
    error = "Unable to inspect restore destination: " + filesystemError.message();
    return false;
  }
  if (destinationExists &&
      (isLinkedOrReparseArtifact(destinationDirectory, filesystemError) || filesystemError)) {
    error = "Restore destination must not be a link or reparse point";
    return false;
  }
  filesystem::path staging;
  if (!createUniqueDirectory(destinationDirectory, ".restore-staging", staging, error)) return false;
  RestoreJournal journal;
  journal.destination = destinationDirectory;
  journal.staging = staging;
  journal.settings = appSettingsPath;
  if (!chooseUnusedSibling(destinationDirectory, ".restore-old-data", journal.oldData, error) ||
      !chooseUnusedSibling(appSettingsPath, ".restore-old", journal.settingsBackup, error)) {
    string cleanupError;
    ops.removeAll(staging, cleanupError);
    return false;
  }
  journal.destinationExisted = destinationExists;
  journal.settingsExisted = filesystem::exists(appSettingsPath, filesystemError);
  if (filesystemError) {
    string cleanupError;
    ops.removeAll(staging, cleanupError);
    error = "Unable to inspect application settings: " + filesystemError.message();
    return false;
  }
  journal.state = "prepared";
  const auto discardStage = [&](const string& primary) {
    string cleanupError;
    string firstCleanupError;
    bool cleanupOk = ops.removeAll(staging, cleanupError);
    if (!cleanupOk) firstCleanupError = cleanupError;
    if (journal.settingsExisted) {
      cleanupError.clear();
      if (!ops.removeAll(journal.settingsBackup, cleanupError) && firstCleanupError.empty()) {
        firstCleanupError = cleanupError;
      }
      cleanupOk = cleanupOk && cleanupError.empty();
    }
    // Preserve the journal when any owned artifact could not be removed so
    // startup recovery can retry the cleanup; deleting the marker would make
    // the leftover settings backup undiscoverable.
    if (cleanupOk) {
      cleanupError.clear();
      cleanupOk = ops.removeAll(journalPath, cleanupError);
      if (!cleanupOk && firstCleanupError.empty()) firstCleanupError = cleanupError;
    }
    error = primary;
    if (!cleanupOk) error += "; restore cleanup pending: " + firstCleanupError;
    return false;
  };
  if (!writeRestoreJournal(journalPath, journal, ops, error)) return discardStage(error);
  if (!ops.copy(backupDirectory / "manifest.tsv", staging / "manifest.tsv", error)) return discardStage(error);
  for (const auto& entry : entries) {
    if (!ops.copy(backupDirectory / entry.name, staging / entry.name, error)) return discardStage(error);
  }
  if (!validateInventatoryBackup(staging, error)) return discardStage(error);
  if (journal.settingsExisted && !ops.copy(appSettingsPath, journal.settingsBackup, error)) return discardStage(error);

  string primaryError;
  journal.state = "protecting";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) return discardStage(primaryError);
  if (journal.destinationExisted && !ops.rename(destinationDirectory, journal.oldData, primaryError)) {
    return discardStage(primaryError);
  }
  // From this point onward the old workspace has been moved out of the
  // destination (or the destination was initially absent). Any failure can
  // leave the on-disk workspace in an indeterminate state unless rollback
  // completes, so callers must keep dependent services stopped.
  if (replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = true;
  journal.state = "protected";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }
  journal.state = "activating";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError) ||
      !ops.rename(staging, destinationDirectory, primaryError)) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }
  journal.state = "activated";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }

  AppSettings restoredSettings;
  if (!loadAppSettings(destinationDirectory / "settings.conf", restoredSettings)) {
    primaryError = "Restored settings are invalid";
  } else {
    restoredSettings.dataDirectory = destinationDirectory;
    journal.state = "writing_settings";
    if (!writeRestoreJournal(journalPath, journal, ops, primaryError) ||
        !saveRestoreSettings(appSettingsPath, restoredSettings, ops, primaryError)) {
      if (primaryError.empty()) primaryError = "Unable to activate restored settings";
    }
  }
  if (!primaryError.empty()) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }
  journal.state = "settings_activated";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }

  // The manifest and sanitized settings belong to the bundle, not to the
  // selected data directory. Remove them only after the active DB/settings
  // have been validated and journaled.
  if (!validateWorkspaceData(destinationDirectory, primaryError)) {
    if (primaryError.empty()) primaryError = "Restored workspace data could not be validated";
  }
  if (primaryError.empty() && !validateWorkspaceSettings(appSettingsPath, destinationDirectory, primaryError)) {
    if (primaryError.empty()) primaryError = "Restored application settings could not be validated";
  }
  if (!primaryError.empty()) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }
  if (filesystem::exists(destinationDirectory / "manifest.tsv") &&
      !ops.removeAll(destinationDirectory / "manifest.tsv", primaryError)) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }
  if (filesystem::exists(destinationDirectory / "settings.conf") &&
      !ops.removeAll(destinationDirectory / "settings.conf", primaryError)) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primaryError + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  }

  journal.state = "cleanup_pending";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) {
    error = "Restored data is active, but cleanup is pending: " + primaryError;
    return false;
  }
  return cleanupCommittedRestore(journal, ops, error);
  } catch (const filesystem::filesystem_error& exception) {
    error = string("Restore filesystem error: ") + exception.what();
    return false;
  } catch (const exception& exception) {
    error = string("Restore error: ") + exception.what();
    return false;
  }
}

}  // namespace inventatory
