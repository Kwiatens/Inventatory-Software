// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Inventory restore activation workflow.

#include "core/transfer/InventoryTransferPrivate.h"

#include "app/settings/AppSettings.h"
#include "core/bom/BomProjectStore.h"
#include "core/storage/InventorySqlite.h"
#include "label_printer/core/LabelPrinter.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace inventatory {
using namespace std;

using namespace inventory_transfer_detail;

namespace {

// The settings record recordedDirectory as the user named it, even when a symlink is resolved.
bool restoreInventatoryBackupImpl(const filesystem::path& backupDirectory, const filesystem::path& destinationDirectory,
                                  const filesystem::path& recordedDirectory, const filesystem::path& appSettingsPath,
                                  string& error, const InventoryTransferTestHooks* testHooks,
                                  bool* replacementWorkspaceActiveOnFailure) {
  try {
  error.clear();
  if (replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
#ifndef _WIN32
  {
    // Activation renames the data folder, so for a symlink work on its target instead.
    error_code linkError;
    if (filesystem::is_symlink(destinationDirectory, linkError) && !linkError) {
      const auto resolved = filesystem::canonical(destinationDirectory, linkError);
      if (linkError || !filesystem::is_directory(resolved, linkError) || linkError) {
        error = "Restore destination is a link that does not lead to a folder";
        return false;
      }
      return restoreInventatoryBackupImpl(backupDirectory, resolved, recordedDirectory, appSettingsPath, error,
                                          testHooks, replacementWorkspaceActiveOnFailure);
    }
  }
#endif
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
  if (destinationExists && isLinkedOrReparseArtifact(destinationDirectory, filesystemError)) {
    error = "Restore destination must not be a link or reparse point";
    return false;
  }
  if (filesystemError) {
    error = "Unable to inspect restore destination: " + filesystemError.message();
    return false;
  }
  if (destinationExists && !filesystem::is_directory(destinationDirectory, filesystemError)) {
    error = "Restore destination is not a directory";
    return false;
  }
  if (filesystemError) {
    error = "Unable to inspect restore destination: " + filesystemError.message();
    return false;
  }
  // Refuse before any artifact exists if an unmanaged entry cannot be carried over.
  if (destinationExists &&
      !checkUnmanagedEntriesPreservable(destinationDirectory, WorkspaceEntrySource::OldWorkspace, error)) {
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
    // Keep the journal when cleanup fails, so startup recovery can retry and find the
    // leftover settings backup.
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
  // Past this point a failure can leave the workspace indeterminate until rollback completes,
  // so callers must keep dependent services stopped.
  if (replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = true;
  const auto failWithRollback = [&](const string& primary) {
    string rollbackError;
    const bool rolledBack = rollbackRestore(journal, ops, rollbackError);
    if (rolledBack && replacementWorkspaceActiveOnFailure != nullptr) *replacementWorkspaceActiveOnFailure = false;
    error = primary + (rollbackError.empty() ? string() : "; rollback failed: " + rollbackError);
    return false;
  };
  journal.state = "protected";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) return failWithRollback(primaryError);
  // Rename, not copy, so the activation rename publishes these entries with the restored files.
  // Rollback and recovery move them back before staging is discarded.
  if (journal.destinationExisted &&
      !moveUnmanagedEntries(journal.oldData, staging, WorkspaceEntrySource::OldWorkspace, ops, primaryError)) {
    return failWithRollback(primaryError);
  }
  journal.state = "activating";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError) ||
      !ops.rename(staging, destinationDirectory, primaryError)) {
    return failWithRollback(primaryError);
  }
  journal.state = "activated";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) return failWithRollback(primaryError);

  AppSettings restoredSettings;
  if (!loadAppSettings(destinationDirectory / "settings.conf", restoredSettings)) {
    primaryError = "Restored settings are invalid";
  } else {
    restoredSettings.dataDirectory = recordedDirectory;
    // These describe this computer, not the workspace. Keep the local values so no startup
    // service is registered without consent.
    AppSettings localSettings;
    if (journal.settingsExisted) loadAppSettings(appSettingsPath, localSettings);
    restoredSettings.backgroundServiceEnabled = localSettings.backgroundServiceEnabled;
    restoredSettings.backgroundConsentAsked = localSettings.backgroundConsentAsked;
    restoredSettings.deviceServicePort = localSettings.deviceServicePort;
    journal.state = "writing_settings";
    if (!writeRestoreJournal(journalPath, journal, ops, primaryError) ||
        !saveRestoreSettings(appSettingsPath, restoredSettings, ops, primaryError)) {
      if (primaryError.empty()) primaryError = "Unable to activate restored settings";
    }
  }
  if (!primaryError.empty()) return failWithRollback(primaryError);
  journal.state = "settings_activated";
  if (!writeRestoreJournal(journalPath, journal, ops, primaryError)) return failWithRollback(primaryError);

  // Bundle files are removed only after the active DB and settings are validated and journaled.
  if (!validateWorkspaceData(destinationDirectory, primaryError)) {
    if (primaryError.empty()) primaryError = "Restored workspace data could not be validated";
  }
  if (primaryError.empty() && !validateWorkspaceSettings(appSettingsPath, destinationDirectory, primaryError)) {
    if (primaryError.empty()) primaryError = "Restored application settings could not be validated";
  }
  if (!primaryError.empty()) return failWithRollback(primaryError);
  if (filesystem::exists(destinationDirectory / "manifest.tsv") &&
      !ops.removeAll(destinationDirectory / "manifest.tsv", primaryError)) {
    return failWithRollback(primaryError);
  }
  if (filesystem::exists(destinationDirectory / "settings.conf") &&
      !ops.removeAll(destinationDirectory / "settings.conf", primaryError)) {
    return failWithRollback(primaryError);
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

}  // namespace

bool restoreInventatoryBackup(const filesystem::path& backupDirectory, const filesystem::path& destinationDirectory,
                              const filesystem::path& appSettingsPath, string& error,
                              const InventoryTransferTestHooks* testHooks,
                              bool* replacementWorkspaceActiveOnFailure) {
  return restoreInventatoryBackupImpl(backupDirectory, destinationDirectory, destinationDirectory, appSettingsPath,
                                      error, testHooks, replacementWorkspaceActiveOnFailure);
}

}  // namespace inventatory
