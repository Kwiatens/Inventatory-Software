// Inventatory - Restore filesystem operations and journal persistence.

#include "core/transfer/InventoryTransferPrivate.h"

#include "app/settings/AppSettings.h"
#include "core/bom/BomProjectStore.h"
#include "core/storage/AtomicFile.h"
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
namespace inventory_transfer_detail {

namespace {

// The restore journal and the directory renames are the crash-recovery protocol, so a rename must be on
// disk before the next step is recorded. A move between directories changes both of them.
bool syncRenamedEntry(const filesystem::path& source, const filesystem::path& destination, string& error) {
  const auto parentOf = [](const filesystem::path& path) {
    return path.parent_path().empty() ? filesystem::path(".") : path.parent_path();
  };
  string syncError;
  if (!syncDirectory(parentOf(destination), &syncError) ||
      (parentOf(source) != parentOf(destination) && !syncDirectory(parentOf(source), &syncError))) {
    error = "Unable to flush " + destination.u8string() + ": " + syncError;
    return false;
  }
  return true;
}

}  // namespace

  bool TransferOps::copy(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->copyFile) return hooks->copyFile(source, destination, error);
    error_code filesystemError;
    filesystem::copy_file(source, destination, filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) {
      error = "Unable to copy " + source.filename().u8string() + ": " + filesystemError.message();
      return false;
    }
    // A staged copy is published by a later rename, so its content must already be on disk.
    string syncError;
    if (!syncFile(destination, &syncError)) {
      error = "Unable to flush " + destination.filename().u8string() + ": " + syncError;
      return false;
    }
    return true;
  }

  bool TransferOps::rename(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->renamePath) return hooks->renamePath(source, destination, error);
    error_code filesystemError;
    filesystem::rename(source, destination, filesystemError);
    if (filesystemError) {
      error = "Unable to rename " + source.u8string() + ": " + filesystemError.message();
      return false;
    }
    return syncRenamedEntry(source, destination, error);
  }

  bool TransferOps::removeAll(const filesystem::path& path, string& error) const {
    if (hooks != nullptr && hooks->removeAll) return hooks->removeAll(path, error);
    error_code filesystemError;
    filesystem::remove_all(path, filesystemError);
    if (filesystemError) {
      error = "Unable to remove " + path.u8string() + ": " + filesystemError.message();
      return false;
    }
    return true;
  }

  bool TransferOps::replace(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->replaceFile) return hooks->replaceFile(source, destination, error);
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      error = "Unable to atomically replace " + destination.u8string();
      return false;
    }
    return true;
#else
    error_code filesystemError;
    filesystem::rename(source, destination, filesystemError);
    if (filesystemError) {
      error = "Unable to atomically replace " + destination.u8string() + ": " + filesystemError.message();
      return false;
    }
    return syncRenamedEntry(source, destination, error);
#endif
  }

namespace {

// Files that restore replaces or discards together with the old workspace.
// SQLite sidecars and the scanner replay state belong to the old workspace's
// database and pairing, so keeping them next to the restored data would be
// wrong (a stale journal could be applied to the restored database).
constexpr const char* kRestoreReplacedNames[] = {
    "inventory.db",      "activity.tsv",          "printer.conf",
    "quick_labels.conf", "inventatory_scan.conf", "inventatory-scan-replay.state",
    "inventory.db-wal",  "inventory.db-shm",      "inventory.db-journal"};
// Names the restore bundle itself occupies in the replacement workspace.
constexpr const char* kRestoreReservedNames[] = {"settings.conf", "manifest.tsv"};

enum class WorkspaceEntryKind { Replaced, Preserved, Conflict };

WorkspaceEntryKind classifyWorkspaceEntry(const filesystem::path& path, WorkspaceEntrySource source) {
  const string name = path.filename().u8string();
  const string lowered = lowercaseAscii(name);
  bool replaced = false;
  bool reserved = false;
  bool similar = false;
  for (const char* known : kRestoreReplacedNames) {
    if (name == known) replaced = true;
    else if (lowered == known) similar = true;
  }
  for (const char* known : kRestoreReservedNames) {
    if (name == known) reserved = true;
    else if (lowered == known) similar = true;
  }
  // A name that differs only by case would collide on a case-insensitive
  // filesystem, so it is neither replaceable nor safe to carry over.
  if (similar) return WorkspaceEntryKind::Conflict;
  if (reserved) {
    return source == WorkspaceEntrySource::RestoredCopy ? WorkspaceEntryKind::Replaced
                                                        : WorkspaceEntryKind::Conflict;
  }
  if (!replaced) return WorkspaceEntryKind::Preserved;
  // A real folder with a managed file name is user data, not a managed file.
  error_code statusError;
  const auto status = filesystem::symlink_status(path, statusError);
  if (statusError || status.type() == filesystem::file_type::directory) return WorkspaceEntryKind::Conflict;
  return WorkspaceEntryKind::Replaced;
}

bool listPreservedEntries(const filesystem::path& directory, WorkspaceEntrySource source,
                          vector<filesystem::path>& preserved, string& error) {
  preserved.clear();
  error_code enumerationError;
  for (filesystem::directory_iterator iterator(directory, enumerationError), end;
       !enumerationError && iterator != end; iterator.increment(enumerationError)) {
    const auto current = iterator->path();
    switch (classifyWorkspaceEntry(current, source)) {
      case WorkspaceEntryKind::Replaced:
        break;
      case WorkspaceEntryKind::Preserved:
        preserved.push_back(current);
        break;
      case WorkspaceEntryKind::Conflict:
        error = "Restore destination contains \"" + current.filename().u8string() +
                "\", which conflicts with a managed Inventatory file name; move or rename it and try again";
        return false;
    }
  }
  if (enumerationError) {
    error = "Unable to enumerate " + directory.u8string() + ": " + enumerationError.message();
    return false;
  }
  return true;
}

}  // namespace

bool checkUnmanagedEntriesPreservable(const filesystem::path& directory, WorkspaceEntrySource source,
                                      string& error) {
  vector<filesystem::path> preserved;
  return listPreservedEntries(directory, source, preserved, error);
}

bool moveUnmanagedEntries(const filesystem::path& from, const filesystem::path& to, WorkspaceEntrySource source,
                          const TransferOps& ops, string& error) {
  vector<filesystem::path> preserved;
  if (!listPreservedEntries(from, source, preserved, error)) return false;
  for (const auto& entry : preserved) {
    const auto target = to / entry.filename();
    // rename() silently replaces an existing file, so never move onto a name
    // that is already in use.
    error_code statusError;
    const auto status = filesystem::symlink_status(target, statusError);
    if (status.type() != filesystem::file_type::not_found) {
      error = "Unable to preserve \"" + entry.filename().u8string() + "\": the name is already in use";
      return false;
    }
    if (!ops.rename(entry, target, error)) return false;
  }
  return true;
}

bool saveRestoreSettings(const filesystem::path& path, const AppSettings& settings, const TransferOps& ops,
                         string& error) {
  if (ops.hooks != nullptr && ops.hooks->saveSettings) return ops.hooks->saveSettings(path, settings, error);
  if (!saveAppSettings(path, settings)) {
    error = "Unable to activate restored settings";
    return false;
  }
  return true;
}

filesystem::path uniqueSibling(const filesystem::path& base, const string& suffix, size_t attempt) {
  const auto parent = base.parent_path().empty() ? filesystem::path(".") : base.parent_path();
  const auto timestamp = chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
  const auto processId = static_cast<unsigned long long>(GetCurrentProcessId());
#else
  const auto processId = static_cast<unsigned long long>(getpid());
#endif
  const auto name = base.filename().u8string() + suffix + "-" + to_string(timestamp) + "-" +
                    to_string(processId) + "-" + to_string(attempt);
  return parent / filesystem::u8path(name);
}

bool chooseUnusedSibling(const filesystem::path& base, const string& suffix, filesystem::path& result,
                         string& error) {
  error_code filesystemError;
  for (size_t attempt = 0; attempt < 100; ++attempt) {
    const auto candidate = uniqueSibling(base, suffix, attempt);
    if (!filesystem::exists(candidate, filesystemError)) {
      if (filesystemError) {
        error = "Unable to inspect restore artifact: " + filesystemError.message();
        return false;
      }
      result = candidate;
      return true;
    }
    filesystemError.clear();
  }
  error = "Unable to allocate a unique restore artifact";
  return false;
}

bool createUniqueDirectory(const filesystem::path& base, const string& suffix, filesystem::path& result,
                           string& error) {
  error_code filesystemError;
  const auto parent = base.parent_path().empty() ? filesystem::path(".") : base.parent_path();
  filesystem::create_directories(parent, filesystemError);
  if (filesystemError) {
    error = "Unable to create staging parent: " + filesystemError.message();
    return false;
  }
  for (size_t attempt = 0; attempt < 100; ++attempt) {
    const auto candidate = uniqueSibling(base, suffix, attempt);
    if (filesystem::create_directory(candidate, filesystemError)) {
      result = candidate;
      return true;
    }
    filesystemError.clear();
  }
  error = "Unable to allocate a unique staging directory";
  return false;
}

bool atomicWriteText(const filesystem::path& target, const string& text, const TransferOps& ops, string& error) {
  filesystem::path temporary;
  if (!chooseUnusedSibling(target, ".tmp", temporary, error)) return false;
  error_code filesystemError;
  ofstream output(temporary, ios::binary | ios::trunc);
  if (!output) {
    error = "Unable to create " + target.u8string();
    return false;
  }
  output.write(text.data(), static_cast<streamsize>(text.size()));
  output.flush();
  output.close();
  if (!output) {
    error = "Unable to finish writing " + target.u8string();
    filesystem::remove(temporary, filesystemError);
    return false;
  }
  // The content must be on disk before the rename can make it the journal; otherwise a crash can leave
  // a journal that is empty or older than the layout it describes.
  string syncError;
  if (!syncFile(temporary, &syncError)) {
    error = "Unable to flush " + target.u8string() + ": " + syncError;
    filesystem::remove(temporary, filesystemError);
    return false;
  }
  if (!ops.replace(temporary, target, error)) {
    filesystem::remove(temporary, filesystemError);
    return false;
  }
  return true;
}

string hexEncode(const string& value) {
  return hexBytes(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

bool hexDecode(const string& value, string& result) {
  if (value.size() % 2 != 0) return false;
  result.clear();
  for (size_t index = 0; index < value.size(); index += 2) {
    const auto decode = [](unsigned char ch) -> int {
      if (ch >= '0' && ch <= '9') return ch - '0';
      if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
      return -1;
    };
    const int high = decode(static_cast<unsigned char>(value[index]));
    const int low = decode(static_cast<unsigned char>(value[index + 1]));
    if (high < 0 || low < 0) return false;
    result.push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

filesystem::path restoreJournalPath(const filesystem::path& settings) {
  const auto parent = settings.parent_path().empty() ? filesystem::path(".") : settings.parent_path();
  return parent / filesystem::u8path(settings.filename().u8string() + ".restore-journal");
}

bool writeRestoreJournal(const filesystem::path& path, const RestoreJournal& journal, const TransferOps& ops,
                         string& error) {
  ostringstream output;
  output << "inventatory_restore_journal\t1\n"
         << "destination\t" << hexEncode(journal.destination.u8string()) << '\n'
         << "staging\t" << hexEncode(journal.staging.u8string()) << '\n'
         << "old_data\t" << hexEncode(journal.oldData.u8string()) << '\n'
         << "settings\t" << hexEncode(journal.settings.u8string()) << '\n'
         << "settings_backup\t" << hexEncode(journal.settingsBackup.u8string()) << '\n'
         << "destination_existed\t" << (journal.destinationExisted ? "1" : "0") << '\n'
         << "settings_existed\t" << (journal.settingsExisted ? "1" : "0") << '\n'
         << "state\t" << journal.state << '\n';
  return atomicWriteText(path, output.str(), ops, error);
}

bool readRestoreJournal(const filesystem::path& path, RestoreJournal& journal, string& error) {
  error_code sizeError;
  const auto journalSize = filesystem::file_size(path, sizeError);
  if (sizeError || journalSize > kMaximumRestoreJournalBytes) {
    error = "Restore journal is missing or too large";
    return false;
  }
  ifstream input(path, ios::binary);
  if (!input) {
    error = "Restore journal is unreadable";
    return false;
  }
  map<string, string> values;
  string line;
  while (getline(input, line)) {
    const auto fields = splitManifestRow(line);
    const set<string> allowed = {"inventatory_restore_journal", "destination", "staging", "old_data", "settings",
                                 "settings_backup", "destination_existed", "settings_existed", "state"};
    if (fields.size() != 2 || fields[0].empty() || allowed.find(fields[0]) == allowed.end() ||
        values.find(fields[0]) != values.end()) {
      error = "Restore journal is malformed";
      return false;
    }
    values.emplace(fields[0], fields[1]);
  }
  const char* required[] = {"inventatory_restore_journal", "destination", "staging", "old_data", "settings",
                            "settings_backup", "destination_existed", "settings_existed", "state"};
  for (const char* key : required) {
    if (values.find(key) == values.end()) {
      error = "Restore journal is incomplete";
      return false;
    }
  }
  if (values["inventatory_restore_journal"] != "1" ||
      (values["destination_existed"] != "0" && values["destination_existed"] != "1") ||
      (values["settings_existed"] != "0" && values["settings_existed"] != "1")) {
    error = "Restore journal has an invalid version or flag";
    return false;
  }
  string decoded;
  if (!hexDecode(values["destination"], decoded)) {
    error = "Restore journal contains an invalid destination";
    return false;
  }
  journal.destination = filesystem::u8path(decoded);
  if (!hexDecode(values["staging"], decoded)) {
    error = "Restore journal contains an invalid staging path";
    return false;
  }
  journal.staging = filesystem::u8path(decoded);
  if (!hexDecode(values["old_data"], decoded)) {
    error = "Restore journal contains an invalid old-data path";
    return false;
  }
  journal.oldData = filesystem::u8path(decoded);
  if (!hexDecode(values["settings"], decoded)) {
    error = "Restore journal contains an invalid settings path";
    return false;
  }
  journal.settings = filesystem::u8path(decoded);
  if (!hexDecode(values["settings_backup"], decoded)) {
    error = "Restore journal contains an invalid settings-backup path";
    return false;
  }
  journal.settingsBackup = filesystem::u8path(decoded);
  journal.destinationExisted = values["destination_existed"] == "1";
  journal.settingsExisted = values["settings_existed"] == "1";
  journal.state = values["state"];
  const set<string> states = {"prepared", "protecting", "protected", "activating", "activated",
                              "writing_settings", "settings_activated", "cleanup_pending"};
  if (states.find(journal.state) == states.end()) {
    error = "Restore journal has an invalid state";
    return false;
  }
  return true;
}

}  // namespace inventory_transfer_detail
}  // namespace inventatory
