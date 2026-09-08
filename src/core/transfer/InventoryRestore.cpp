// Inventatory - Inventory restore activation workflow.

// Inventatory - Inventory export and backup/restore workflows.

#include "core/transfer/InventoryTransferPrivate.h"

#include "app/settings/AppSettings.h"
#include "core/bom/BomProjectStore.h"
#include "core/storage/InventorySqlite.h"
#include "label_printer/core/LabelPrinter.h"

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
