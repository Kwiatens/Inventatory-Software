// Inventatory - Backup and restore workspace validation.

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

bool validateEntries(const filesystem::path& directory, const vector<BackupEntry>& entries, string& error) {
  set<string> listed;
  uintmax_t aggregateSize = 0;
  for (const auto& entry : entries) listed.insert(entry.name);
  error_code enumerationError;
  for (filesystem::directory_iterator iterator(directory, enumerationError), end; iterator != end;
       iterator.increment(enumerationError)) {
    if (enumerationError) {
      error = "Unable to enumerate backup bundle: " + enumerationError.message();
      return false;
    }
    const auto current = iterator->path();
    if (filesystem::is_symlink(current, enumerationError) || enumerationError ||
        !filesystem::is_regular_file(current, enumerationError) || enumerationError) {
      error = "Backup bundle contains an unexpected entry";
      return false;
    }
    const auto name = current.filename().u8string();
    if (name != "manifest.tsv" && listed.find(name) == listed.end()) {
      error = "Backup bundle contains an unlisted file: " + name;
      return false;
    }
  }
  for (const auto& entry : entries) {
    if (entry.size > kMaximumBackupPayloadBytes ||
        aggregateSize > kMaximumBackupPayloadBytes - entry.size) {
      error = "Backup payload is too large";
      return false;
    }
    aggregateSize += entry.size;
    const auto path = directory / entry.name;
    error_code filesystemError;
    if (filesystem::is_symlink(path, filesystemError) || filesystemError ||
        !filesystem::is_regular_file(path, filesystemError) || filesystemError ||
        filesystem::file_size(path, filesystemError) != entry.size || filesystemError) {
      error = "Backup file is missing or has the wrong size: " + entry.name;
      return false;
    }
    string actualHash;
    if (!sha256File(path, actualHash, error) || lowercaseAscii(actualHash) != lowercaseAscii(entry.hash)) {
      if (error.empty()) error = "Backup file hash mismatch: " + entry.name;
      return false;
    }
  }
  if (!validateWorkspaceData(directory, error)) return false;
  AppSettings settings;
  if (!loadAppSettings(directory / "settings.conf", settings)) {
    error = "Backup settings are invalid";
    return false;
  }
  return true;
}

bool equivalentPath(const filesystem::path& first, const filesystem::path& second) {
  error_code firstError;
  error_code secondError;
  auto firstAbsolute = filesystem::weakly_canonical(first, firstError).lexically_normal();
  auto secondAbsolute = filesystem::weakly_canonical(second, secondError).lexically_normal();
  if (firstError || secondError) return false;
  string lhs = firstAbsolute.u8string();
  string rhs = secondAbsolute.u8string();
#ifdef _WIN32
  transform(lhs.begin(), lhs.end(), lhs.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  transform(rhs.begin(), rhs.end(), rhs.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
#endif
  return lhs == rhs;
}

bool isLinkedOrReparseArtifact(const filesystem::path& path, error_code& error) {
  error.clear();
  const auto status = filesystem::symlink_status(path, error);
  if (error) {
#ifdef _WIN32
    if (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND) {
      error.clear();
      return false;
    }
#else
    if (error == make_error_code(errc::no_such_file_or_directory)) {
      error.clear();
      return false;
    }
#endif
    return false;
  }
  if (status.type() == filesystem::file_type::symlink) return true;
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    error = error_code(static_cast<int>(GetLastError()), system_category());
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

// Validate the complete data-directory portion of a workspace without
// creating, migrating, or otherwise mutating any file. Backup staging,
// post-activation restore checks, and startup recovery all use this same
// semantic gate so an optional sidecar cannot bypass one of those paths.
bool validateWorkspaceData(const filesystem::path& directory, string& error) {
  error.clear();
  const auto rejectLinkedArtifact = [&](const filesystem::path& path, const char* label) {
    error_code statusError;
    const auto status = filesystem::symlink_status(path, statusError);
    if (status.type() == filesystem::file_type::not_found) return true;
    if (statusError) {
      error = "Unable to inspect workspace " + string(label) + ": " + statusError.message();
      return false;
    }
    error_code linkError;
    if (isLinkedOrReparseArtifact(path, linkError)) {
      error = "Workspace " + string(label) + " must not be a link or reparse point";
      return false;
    }
    if (linkError) {
      error = "Unable to inspect workspace " + string(label) + ": " + linkError.message();
      return false;
    }
    return true;
  };
  if (!rejectLinkedArtifact(directory / "inventory.db", "inventory database")) return false;
  SqliteConnection connection;
  if (!openDatabaseReadOnly(directory / "inventory.db", connection) ||
      !validateInventoryDatabase(connection, &error)) {
    if (error.empty()) error = "Workspace inventory database could not be validated";
    return false;
  }
  if (!validateBomProjects(connection, &error)) {
    if (error.empty()) error = "Workspace BOM project data could not be validated";
    return false;
  }

  const auto optionalPath = [&](const char* name) { return directory / name; };
  error_code filesystemError;
  if (!rejectLinkedArtifact(optionalPath("activity.tsv"), "activity history") ||
      !rejectLinkedArtifact(optionalPath("quick_labels.conf"), "Quick Labels settings") ||
      !rejectLinkedArtifact(optionalPath("printer.conf"), "printer settings")) {
    return false;
  }
  if (filesystem::exists(optionalPath("activity.tsv"), filesystemError)) {
    if (filesystemError) {
      error = "Unable to inspect workspace activity history: " + filesystemError.message();
      return false;
    }
    vector<ActivityEntry> activities;
    if (!loadActivities(optionalPath("activity.tsv"), activities)) {
      error = "Workspace activity history is invalid";
      return false;
    }
  } else if (filesystemError) {
    error = "Unable to inspect workspace activity history: " + filesystemError.message();
    return false;
  }
  filesystemError.clear();
  if (filesystem::exists(optionalPath("quick_labels.conf"), filesystemError)) {
    if (filesystemError) {
      error = "Unable to inspect workspace Quick Labels settings: " + filesystemError.message();
      return false;
    }
    vector<string> presets;
    uint32_t revision = 1;
    if (!loadQuickLabels(optionalPath("quick_labels.conf"), presets, revision)) {
      error = "Workspace Quick Labels settings are invalid";
      return false;
    }
  } else if (filesystemError) {
    error = "Unable to inspect workspace Quick Labels settings: " + filesystemError.message();
    return false;
  }
  filesystemError.clear();
  if (filesystem::exists(optionalPath("printer.conf"), filesystemError)) {
    if (filesystemError) {
      error = "Unable to inspect workspace printer settings: " + filesystemError.message();
      return false;
    }
    LabelPrinterService printer;
    if (!printer.loadConfig(optionalPath("printer.conf"))) {
      error = "Workspace printer settings are invalid";
      return false;
    }
  } else if (filesystemError) {
    error = "Unable to inspect workspace printer settings: " + filesystemError.message();
    return false;
  }
  return true;
}

bool validateWorkspaceSettings(const filesystem::path& settingsPath, const filesystem::path& dataDirectory,
                               string& error) {
  error.clear();
  AppSettings settings;
  if (!loadAppSettings(settingsPath, settings)) {
    error = "Workspace application settings are invalid";
    return false;
  }
  if (!equivalentPath(settings.dataDirectory, dataDirectory)) {
    error = "Workspace application settings point to a different data directory";
    return false;
  }
  return true;
}

bool pathContains(const filesystem::path& ancestor, const filesystem::path& candidate, bool& contains,
                  string& error) {
  error_code ancestorError;
  error_code candidateError;
  const auto ancestorAbsolute = filesystem::weakly_canonical(ancestor, ancestorError).lexically_normal();
  const auto candidateAbsolute = filesystem::weakly_canonical(candidate, candidateError).lexically_normal();
  if (ancestorError || candidateError) {
    error = "Unable to resolve backup path overlap: " +
            (ancestorError ? ancestorError.message() : candidateError.message());
    return false;
  }
  string lhs = ancestorAbsolute.generic_u8string();
  string rhs = candidateAbsolute.generic_u8string();
#ifdef _WIN32
  transform(lhs.begin(), lhs.end(), lhs.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  transform(rhs.begin(), rhs.end(), rhs.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
#endif
  if (lhs == rhs) {
    contains = true;
    return true;
  }
  if (!lhs.empty() && lhs.back() != '/') lhs.push_back('/');
  contains = rhs.rfind(lhs, 0) == 0;
  return true;
}

bool pathsOverlap(const filesystem::path& first, const filesystem::path& second, bool& overlaps, string& error) {
  bool firstContainsSecond = false;
  bool secondContainsFirst = false;
  if (!pathContains(first, second, firstContainsSecond, error) ||
      !pathContains(second, first, secondContainsFirst, error)) {
    return false;
  }
  overlaps = firstContainsSecond || secondContainsFirst;
  return true;
}

bool ownedRestoreSibling(const filesystem::path& artifact, const filesystem::path& base, const string& marker);

bool ownedRestoreSibling(const filesystem::path& artifact, const filesystem::path& base, const string& marker) {
  if (!equivalentPath(artifact.parent_path(), base.parent_path())) return false;
  const auto prefix = base.filename().u8string() + marker + "-";
  return artifact.filename().u8string().rfind(prefix, 0) == 0;
}

bool inspectOwnedArtifact(const filesystem::path& artifact, const filesystem::path& base, const string& marker,
                          bool expectedDirectory, bool required, bool& exists, string& error) {
  if (!ownedRestoreSibling(artifact, base, marker)) {
    error = "Restore artifact path is not owned by this workspace";
    return false;
  }
  error_code filesystemError;
  const auto status = filesystem::symlink_status(artifact, filesystemError);
  if (filesystemError) {
#ifdef _WIN32
    if (filesystemError.value() == ERROR_FILE_NOT_FOUND || filesystemError.value() == ERROR_PATH_NOT_FOUND) {
      exists = false;
      if (required) error = "Required restore artifact is missing";
      return !required;
    }
#else
    if (filesystemError == make_error_code(errc::no_such_file_or_directory)) {
      exists = false;
      if (required) error = "Required restore artifact is missing";
      return !required;
    }
#endif
    error = "Unable to inspect restore artifact: " + filesystemError.message();
    return false;
  }
  if (status.type() == filesystem::file_type::symlink) {
    error = "Restore artifact is a link or reparse point";
    return false;
  }
  exists = status.type() != filesystem::file_type::not_found;
  if (!exists) {
    if (required) error = "Required restore artifact is missing";
    return !required;
  }
  if (isLinkedOrReparseArtifact(artifact, filesystemError) || filesystemError) {
    error = "Restore artifact is a link or reparse point";
    return false;
  }
  const bool correctType = expectedDirectory ? filesystem::is_directory(artifact, filesystemError)
                                             : filesystem::is_regular_file(artifact, filesystemError);
  if (filesystemError || !correctType) {
    error = expectedDirectory ? "Restore artifact is not a real directory"
                              : "Restore artifact is not a regular file";
    return false;
  }
  return true;
}

bool validateJournalOwnership(const RestoreJournal& journal, const filesystem::path& destination,
                              const filesystem::path& settings, string& error) {
  if (!equivalentPath(journal.destination, destination) || !equivalentPath(journal.settings, settings) ||
      !ownedRestoreSibling(journal.staging, destination, ".restore-staging") ||
      !ownedRestoreSibling(journal.oldData, destination, ".restore-old-data") ||
      !ownedRestoreSibling(journal.settingsBackup, settings, ".restore-old")) {
    error = "Restore journal paths are not owned by this workspace";
    return false;
  }
  return true;
}

bool copyToAtomicTarget(const filesystem::path& source, const filesystem::path& target, const TransferOps& ops,
                        string& error) {
  filesystem::path temporary;
  if (!chooseUnusedSibling(target, ".restore-copy", temporary, error)) return false;
  error_code filesystemError;
  if (!ops.copy(source, temporary, error)) return false;
  if (!ops.replace(temporary, target, error)) {
    filesystem::remove(temporary, filesystemError);
    return false;
  }
  return true;
}

bool restoreSettingsFromJournal(const RestoreJournal& journal, const TransferOps& ops, string& error) {
  if (journal.settingsExisted) {
    bool backupExists = false;
    if (!inspectOwnedArtifact(journal.settingsBackup, journal.settings, ".restore-old", false, true, backupExists,
                              error)) return false;
    return copyToAtomicTarget(journal.settingsBackup, journal.settings, ops, error);
  }
  bool backupExists = false;
  if (!inspectOwnedArtifact(journal.settingsBackup, journal.settings, ".restore-old", false, false, backupExists,
                            error)) return false;
  if (backupExists) {
    error = "Unexpected settings backup is present during restore rollback";
    return false;
  }
  error_code filesystemError;
  filesystem::remove(journal.settings, filesystemError);
  if (filesystemError) {
    error = "Unable to remove newly created settings: " + filesystemError.message();
    return false;
  }
  return true;
}

}  // namespace inventory_transfer_detail
}  // namespace inventatory
