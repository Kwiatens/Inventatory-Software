// Inventatory - Restore rollback, cleanup, and startup recovery.

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
namespace inventory_transfer_detail {

bool rollbackRestore(const RestoreJournal& journal, const TransferOps& ops, string& error) {
  string stepError;
  bool oldDataExists = false;
  if (journal.destinationExisted) {
    // Never destroy the active destination unless the journal still points to
    // a real, owned protected directory that can be put back. A missing old
    // directory is an unrecoverable state, not permission to continue.
    if (!inspectOwnedArtifact(journal.oldData, journal.destination, ".restore-old-data", true, true, oldDataExists,
                              stepError)) {
      error = stepError;
      return false;
    }
  } else if (!inspectOwnedArtifact(journal.oldData, journal.destination, ".restore-old-data", true, false,
                                   oldDataExists, stepError) || oldDataExists) {
    // A journal that says no destination existed must never remove an
    // unexpected old-data artifact. It may be user data or evidence of a
    // journal/state mismatch, so leave everything untouched and require
    // explicit recovery handling.
    error = oldDataExists ? "Unexpected protected old-data artifact is present" : stepError;
    return false;
  }
  bool settingsBackupExists = false;
  if (journal.settingsExisted) {
    if (!inspectOwnedArtifact(journal.settingsBackup, journal.settings, ".restore-old", false, true,
                              settingsBackupExists, stepError)) {
      error = stepError;
      return false;
    }
  } else if (!inspectOwnedArtifact(journal.settingsBackup, journal.settings, ".restore-old", false, false,
                                   settingsBackupExists, stepError) || settingsBackupExists) {
    error = settingsBackupExists ? "Unexpected settings backup is present" : stepError;
    return false;
  }
  if (journal.destinationExisted) {
    error_code filesystemError;
    const bool destinationExists = filesystem::exists(journal.destination, filesystemError);
    if (filesystemError) {
      error = "Unable to inspect restored data during rollback: " + filesystemError.message();
      return false;
    }
    filesystem::path newData;
    if (destinationExists) {
      if (!chooseUnusedSibling(journal.destination, ".restore-rollback-new", newData, stepError) ||
          !ops.rename(journal.destination, newData, stepError)) {
        error = stepError;
        return false;
      }
    }
    if (!ops.rename(journal.oldData, journal.destination, stepError)) {
      if (destinationExists) {
        string restoreError;
        if (!ops.rename(newData, journal.destination, restoreError) && !restoreError.empty()) {
          stepError += "; failed to restore the active destination: " + restoreError;
        }
      }
      error = stepError;
      return false;
    }
    if (destinationExists && !ops.removeAll(newData, stepError)) {
      error = "Restored data was rolled back, but the failed activation could not be removed: " + stepError;
      return false;
    }
  } else if (filesystem::exists(journal.destination) && !ops.removeAll(journal.destination, stepError)) {
    error = stepError;
    return false;
  }
  if (!restoreSettingsFromJournal(journal, ops, stepError)) {
    error = stepError;
    return false;
  }
  if (filesystem::exists(journal.staging) && !ops.removeAll(journal.staging, stepError)) {
    error = stepError;
    return false;
  }
  if (settingsBackupExists && !ops.removeAll(journal.settingsBackup, stepError)) {
    error = stepError;
    return false;
  }
  if (!ops.removeAll(restoreJournalPath(journal.settings), stepError)) {
    error = stepError;
    return false;
  }
  return true;
}

bool cleanupCommittedRestore(const RestoreJournal& journal, const TransferOps& ops, string& error) {
  string stepError;
  bool oldDataExists = false;
  if (!inspectOwnedArtifact(journal.oldData, journal.destination, ".restore-old-data", true, false, oldDataExists,
                            stepError)) {
    error = "Restored data is active, but old-data cleanup is pending: " + stepError;
    return false;
  }
  if (!journal.destinationExisted && oldDataExists) {
    error = "Restored data is active, but unexpected old-data cleanup is pending";
    return false;
  }

  bool settingsBackupExists = false;
  if (!inspectOwnedArtifact(journal.settingsBackup, journal.settings, ".restore-old", false, false,
                            settingsBackupExists, stepError)) {
    error = "Restored data is active, but settings backup cleanup is pending: " + stepError;
    return false;
  }
  if (!journal.settingsExisted && settingsBackupExists) {
    error = "Restored data is active, but unexpected settings backup cleanup is pending";
    return false;
  }

  bool stagingExists = false;
  if (!inspectOwnedArtifact(journal.staging, journal.destination, ".restore-staging", true, false, stagingExists,
                            stepError)) {
    error = "Restored data is active, but staging cleanup is pending: " + stepError;
    return false;
  }
  if (stagingExists && !ops.removeAll(journal.staging, stepError)) {
    error = "Restored data is active, but staging cleanup failed: " + stepError;
    return false;
  }
  if (oldDataExists && !ops.removeAll(journal.oldData, stepError)) {
    error = "Restored data is active, but old data cleanup failed: " + stepError;
    return false;
  }
  if (settingsBackupExists && !ops.removeAll(journal.settingsBackup, stepError)) {
    error = "Restored data is active, but settings backup cleanup failed: " + stepError;
    return false;
  }
  if (!ops.removeAll(restoreJournalPath(journal.settings), stepError)) {
    error = "Restored data is active, but restore journal cleanup failed: " + stepError;
    return false;
  }
  return true;
}

}  // namespace inventory_transfer_detail

using namespace inventory_transfer_detail;

bool recoverInventatoryRestore(const filesystem::path& destinationDirectory, const filesystem::path& appSettingsPath,
                               string& error) {
  try {
  error.clear();
  bool overlap = false;
  if (!pathsOverlap(destinationDirectory, appSettingsPath, overlap, error)) return false;
  if (overlap) {
    error = "Restore destination overlaps application settings";
    return false;
  }
  const auto journalPath = restoreJournalPath(appSettingsPath);
  error_code filesystemError;
  if (!filesystem::exists(journalPath, filesystemError)) {
    if (filesystemError) error = "Unable to inspect restore journal: " + filesystemError.message();
    return !filesystemError;
  }
  if (filesystemError) {
    error = "Unable to inspect restore journal: " + filesystemError.message();
    return false;
  }
  RestoreJournal journal;
  if (!readRestoreJournal(journalPath, journal, error) ||
      !validateJournalOwnership(journal, destinationDirectory, appSettingsPath, error)) {
    return false;
  }
  bool oldDataExists = false;
  if (!inspectOwnedArtifact(journal.oldData, journal.destination, ".restore-old-data", true, false, oldDataExists,
                            error)) {
    return false;
  }
  if (!journal.destinationExisted && oldDataExists) {
    error = "Unexpected protected old-data artifact is present";
    return false;
  }
  bool settingsBackupExists = false;
  if (!inspectOwnedArtifact(journal.settingsBackup, journal.settings, ".restore-old", false, false,
                            settingsBackupExists, error)) {
    return false;
  }
  if (!journal.settingsExisted && settingsBackupExists) {
    error = "Unexpected settings backup is present";
    return false;
  }
  const TransferOps ops;
  if (journal.state == "prepared") {
    string cleanupError;
    if (oldDataExists) {
      error = "Unexpected protected old-data artifact is present in prepared restore";
      return false;
    }
    if (!ops.removeAll(journal.staging, cleanupError) || !ops.removeAll(journal.settingsBackup, cleanupError) ||
        !ops.removeAll(journalPath, cleanupError)) {
      error = cleanupError;
      return false;
    }
    return true;
  }
  if (journal.state == "protecting" && filesystem::exists(journal.destination) &&
      !filesystem::exists(journal.oldData)) {
    string cleanupError;
    if (!ops.removeAll(journal.staging, cleanupError) || !ops.removeAll(journal.settingsBackup, cleanupError) ||
        !ops.removeAll(journalPath, cleanupError)) {
      error = cleanupError;
      return false;
    }
    return true;
  }
  if (journal.state == "settings_activated" || journal.state == "cleanup_pending") {
    if (journal.state == "settings_activated" && journal.settingsExisted && !settingsBackupExists) {
      error = "Restored data is active, but the settings backup is missing";
      return false;
    }
    string validationError;
    if (filesystem::exists(journal.destination) && validateWorkspaceData(journal.destination, validationError) &&
        validateWorkspaceSettings(journal.settings, journal.destination, validationError)) {
        string cleanupError;
        if (journal.state == "settings_activated" && journal.destinationExisted && !oldDataExists) {
          error = "Restored data is active, but the protected old data is missing";
          return false;
        }
        if (filesystem::exists(journal.destination / "manifest.tsv") &&
            !ops.removeAll(journal.destination / "manifest.tsv", cleanupError)) {
          error = cleanupError;
          return false;
        }
        if (filesystem::exists(journal.destination / "settings.conf") &&
            !ops.removeAll(journal.destination / "settings.conf", cleanupError)) {
          error = cleanupError;
          return false;
        }
        if (journal.state == "settings_activated") {
          journal.state = "cleanup_pending";
          if (!writeRestoreJournal(journalPath, journal, ops, cleanupError)) {
            error = "Restored data is active, but cleanup could not be committed: " + cleanupError;
            return false;
          }
        }
        return cleanupCommittedRestore(journal, ops, error);
    } else if (validationError.empty()) {
      validationError = "Restored workspace data could not be validated";
    }
    // An activated DB without valid settings is not a committed workspace;
    // roll it back and leave the original validation error visible.
    string rollbackError;
    if (rollbackRestore(journal, ops, rollbackError)) {
      error = validationError.empty() ? "Interrupted restore was rolled back" : validationError;
      return false;
    }
    error = validationError + "; rollback failed: " + rollbackError;
    return false;
  }

  const string interruptedState = journal.state;
  string rollbackError;
  if (rollbackRestore(journal, ops, rollbackError)) {
    error = "Interrupted restore in state " + interruptedState + " was rolled back";
    return true;
  }
  error = "Unable to roll back interrupted restore in state " + interruptedState + ": " + rollbackError;
  return false;
  } catch (const filesystem::filesystem_error& exception) {
    error = string("Restore recovery filesystem error: ") + exception.what();
    return false;
  } catch (const exception& exception) {
    error = string("Restore recovery error: ") + exception.what();
    return false;
  }
}

}  // namespace inventatory
