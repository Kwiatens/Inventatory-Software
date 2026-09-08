// Inventatory - Restore filesystem operations and journal persistence.

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

  bool TransferOps::copy(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->copyFile) return hooks->copyFile(source, destination, error);
    error_code filesystemError;
    filesystem::copy_file(source, destination, filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) {
      error = "Unable to copy " + source.filename().string() + ": " + filesystemError.message();
      return false;
    }
    return true;
  }

  bool TransferOps::rename(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->renamePath) return hooks->renamePath(source, destination, error);
    error_code filesystemError;
    filesystem::rename(source, destination, filesystemError);
    if (filesystemError) {
      error = "Unable to rename " + source.string() + ": " + filesystemError.message();
      return false;
    }
    return true;
  }

  bool TransferOps::removeAll(const filesystem::path& path, string& error) const {
    if (hooks != nullptr && hooks->removeAll) return hooks->removeAll(path, error);
    error_code filesystemError;
    filesystem::remove_all(path, filesystemError);
    if (filesystemError) {
      error = "Unable to remove " + path.string() + ": " + filesystemError.message();
      return false;
    }
    return true;
  }

  bool TransferOps::replace(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->replaceFile) return hooks->replaceFile(source, destination, error);
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      error = "Unable to atomically replace " + destination.string();
      return false;
    }
    return true;
#else
    error_code filesystemError;
    filesystem::rename(source, destination, filesystemError);
    if (filesystemError) {
      error = "Unable to atomically replace " + destination.string() + ": " + filesystemError.message();
      return false;
    }
    return true;
#endif
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
    error = "Unable to create " + target.string();
    return false;
  }
  output.write(text.data(), static_cast<streamsize>(text.size()));
  output.flush();
  output.close();
  if (!output) {
    error = "Unable to finish writing " + target.string();
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
