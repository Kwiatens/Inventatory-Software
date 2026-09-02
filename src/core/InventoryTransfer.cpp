// Inventatory - Inventory export and data-folder backup helpers.

#include "core/InventoryTransfer.h"

#include "app/AppSettings.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
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

struct BackupEntry {
  string name;
  uintmax_t size = 0;
  string hash;
};

string hexBytes(const unsigned char* data, size_t count) {
  ostringstream output;
  output << hex << setfill('0');
  for (size_t index = 0; index < count; ++index) output << setw(2) << static_cast<unsigned int>(data[index]);
  return output.str();
}

bool sha256File(const filesystem::path& path, string& hash, string& error) {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hashHandle = nullptr;
  vector<unsigned char> object;
  vector<unsigned char> digest;
  auto cleanup = [&] {
    if (hashHandle != nullptr) BCryptDestroyHash(hashHandle);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
  };
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
    error = "Unable to initialize SHA-256";
    return false;
  }
  DWORD objectLength = 0;
  DWORD resultLength = 0;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                        &resultLength, 0) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&resultLength), sizeof(resultLength),
                        &resultLength, 0) != 0) {
    cleanup();
    error = "Unable to read SHA-256 properties";
    return false;
  }
  DWORD digestLength = 32;
  BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
                    &resultLength, 0);
  object.resize(objectLength);
  digest.resize(digestLength);
  if (BCryptCreateHash(algorithm, &hashHandle, object.data(), objectLength, nullptr, 0, 0) != 0) {
    cleanup();
    error = "Unable to initialize file hash";
    return false;
  }
  ifstream input(path, ios::binary);
  if (!input) {
    cleanup();
    error = "Unable to read " + path.string();
    return false;
  }
  array<unsigned char, 64 * 1024> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0 && BCryptHashData(hashHandle, buffer.data(), static_cast<ULONG>(count), 0) != 0) {
      cleanup();
      error = "Unable to hash " + path.string();
      return false;
    }
  }
  if (!input.eof() || BCryptFinishHash(hashHandle, digest.data(), digestLength, 0) != 0) {
    cleanup();
    error = "Unable to finish hash for " + path.string();
    return false;
  }
  hash = hexBytes(digest.data(), digest.size());
  cleanup();
  return true;
#else
  // The shipped PC build is Windows-only. Keep a deterministic fallback for
  // non-Windows tooling so backup tests still detect accidental corruption.
  ifstream input(path, ios::binary);
  if (!input) {
    error = "Unable to read " + path.string();
    return false;
  }
  string contents((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
  hash = to_string(std::hash<string>{}(contents));
  return true;
#endif
}

bool writeManifest(const filesystem::path& destination, const string& applicationVersion,
                   const vector<BackupEntry>& entries, string& error) {
  ofstream output(destination / "manifest.tsv", ios::binary | ios::trunc);
  if (!output) {
    error = "Unable to create backup manifest";
    return false;
  }
  output << "inventatory_backup_format\t1\n"
         << "application_version\t" << applicationVersion << "\n";
  for (const auto& entry : entries) output << "file\t" << entry.name << '\t' << entry.size << '\t' << entry.hash << '\n';
  output.close();
  if (!output) {
    error = "Unable to finish backup manifest";
    return false;
  }
  return true;
}

bool readManifest(const filesystem::path& backupDirectory, vector<BackupEntry>& entries, string& error) {
  ifstream input(backupDirectory / "manifest.tsv", ios::binary);
  if (!input) {
    error = "Backup manifest is missing";
    return false;
  }
  bool formatFound = false;
  bool applicationVersionFound = false;
  set<string> names;
  string line;
  while (getline(input, line)) {
    istringstream row(line);
    string kind;
    getline(row, kind, '\t');
    if (kind == "inventatory_backup_format") {
      string version;
      getline(row, version, '\t');
      if (version != "1") {
        error = "Unsupported backup format";
        return false;
      }
      formatFound = true;
    } else if (kind == "application_version") {
      string version;
      getline(row, version, '\t');
      if (version.empty()) {
        error = "Backup manifest has no application version";
        return false;
      }
      applicationVersionFound = true;
    } else if (kind == "file") {
      BackupEntry entry;
      string size;
      getline(row, entry.name, '\t');
      getline(row, size, '\t');
      getline(row, entry.hash, '\t');
      const bool supportedName = entry.name == "inventory.db" || entry.name == "activity.tsv" ||
                                 entry.name == "printer.conf" || entry.name == "quick_labels.conf" ||
                                 entry.name == "settings.conf";
      if (!supportedName || size.empty() || entry.hash.empty() || entry.name.find_first_of("/\\") != string::npos ||
          !names.insert(entry.name).second) {
        error = "Backup manifest contains an invalid or duplicate file";
        return false;
      }
      try {
        entry.size = stoull(size);
      } catch (...) {
        error = "Backup manifest contains an invalid file size";
        return false;
      }
      entries.push_back(move(entry));
    }
  }
  if (!formatFound || !applicationVersionFound || entries.empty() || names.count("inventory.db") == 0 ||
      names.count("settings.conf") == 0) {
    error = "Backup manifest is incomplete";
    return false;
  }
  return true;
}

bool validateEntries(const filesystem::path& directory, const vector<BackupEntry>& entries, string& error) {
  for (const auto& entry : entries) {
    const auto path = directory / entry.name;
    error_code filesystemError;
    if (!filesystem::is_regular_file(path, filesystemError) || filesystemError ||
        filesystem::file_size(path, filesystemError) != entry.size || filesystemError) {
      error = "Backup file is missing or has the wrong size: " + entry.name;
      return false;
    }
    string actualHash;
    if (!sha256File(path, actualHash, error) || actualHash != entry.hash) {
      if (error.empty()) error = "Backup file hash mismatch: " + entry.name;
      return false;
    }
  }
  InventoryStore store;
  if (!store.load(directory / "inventory.db")) {
    error = "Backup inventory database could not be opened";
    return false;
  }
  AppSettings settings;
  if (!loadAppSettings(directory / "settings.conf", settings)) {
    error = "Backup settings are invalid";
    return false;
  }
  return true;
}

bool copyBackupFile(const filesystem::path& source, const filesystem::path& destination, string& error) {
  error_code filesystemError;
  filesystem::copy_file(source, destination, filesystem::copy_options::overwrite_existing, filesystemError);
  if (filesystemError) {
    error = "Unable to copy " + source.filename().string() + ": " + filesystemError.message();
    return false;
  }
  return true;
}

}  // namespace

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

bool backupInventatoryData(const filesystem::path& sourceDirectory,
                           const filesystem::path& destinationDirectory, string& error) {
  error_code filesystemError;
  if (!filesystem::is_directory(sourceDirectory, filesystemError)) {
    error = "Inventatory data folder does not exist: " + sourceDirectory.string();
    return false;
  }
  if (!filesystem::create_directories(destinationDirectory, filesystemError) && filesystemError) {
    error = "Unable to create backup folder: " + filesystemError.message();
    return false;
  }

  for (const auto& entry : filesystem::directory_iterator(sourceDirectory, filesystemError)) {
    if (filesystemError) break;
    if (!entry.is_regular_file(filesystemError)) {
      if (filesystemError) break;
      continue;
    }
    const auto target = destinationDirectory / entry.path().filename();
    filesystem::copy_file(entry.path(), target, filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) break;
  }
  if (filesystemError) {
    error = "Unable to copy the Inventatory data: " + filesystemError.message();
    return false;
  }
  return true;
}

bool createInventatoryBackup(const filesystem::path& dataDirectory, const filesystem::path& appSettingsPath,
                             const filesystem::path& destinationDirectory, const string& applicationVersion,
                             string& error) {
  error_code filesystemError;
  if (!filesystem::is_directory(dataDirectory, filesystemError)) {
    error = "Inventatory data folder does not exist: " + dataDirectory.string();
    return false;
  }
  if (filesystem::exists(destinationDirectory)) {
    error = "Backup destination already exists";
    return false;
  }
  filesystem::create_directories(destinationDirectory, filesystemError);
  if (filesystemError) {
    error = "Unable to create backup folder: " + filesystemError.message();
    return false;
  }

  AppSettings settings;
  if (!loadAppSettings(appSettingsPath, settings)) {
    error = "Unable to read application settings for backup";
    return false;
  }
  settings.dataDirectory = filesystem::path(".");
  if (!saveAppSettings(destinationDirectory / "settings.conf", settings)) {
    error = "Unable to write sanitized backup settings";
    return false;
  }

  const vector<string> names = {"inventory.db", "activity.tsv", "printer.conf", "quick_labels.conf", "settings.conf"};
  vector<BackupEntry> entries;
  for (const auto& name : names) {
    const auto source = name == "settings.conf" ? destinationDirectory / name : dataDirectory / name;
    const auto target = destinationDirectory / name;
    if (name != "settings.conf") {
      if (!filesystem::exists(source)) {
        if (name == "inventory.db") {
          error = "Inventory database is missing";
          return false;
        }
        continue;
      }
      if (!copyBackupFile(source, target, error)) return false;
    }
    BackupEntry entry;
    entry.name = name;
    entry.size = filesystem::file_size(target, filesystemError);
    if (filesystemError || !sha256File(target, entry.hash, error)) return false;
    entries.push_back(move(entry));
  }
  if (!writeManifest(destinationDirectory, applicationVersion, entries, error)) return false;
  return true;
}

bool validateInventatoryBackup(const filesystem::path& backupDirectory, string& error) {
  error.clear();
  error_code filesystemError;
  if (!filesystem::is_directory(backupDirectory, filesystemError)) {
    error = "Backup folder does not exist";
    return false;
  }
  vector<BackupEntry> entries;
  return readManifest(backupDirectory, entries, error) && validateEntries(backupDirectory, entries, error);
}

bool restoreInventatoryBackup(const filesystem::path& backupDirectory, const filesystem::path& destinationDirectory,
                             const filesystem::path& appSettingsPath, string& error) {
  error.clear();
  if (!validateInventatoryBackup(backupDirectory, error)) return false;

  vector<BackupEntry> entries;
  if (!readManifest(backupDirectory, entries, error)) return false;
  const auto staging = filesystem::path(destinationDirectory.string() + ".restore-staging");
  error_code filesystemError;
  filesystem::remove_all(staging, filesystemError);
  filesystemError.clear();
  filesystem::create_directories(staging, filesystemError);
  if (filesystemError) {
    error = "Unable to stage restore: " + filesystemError.message();
    return false;
  }
  if (!copyBackupFile(backupDirectory / "manifest.tsv", staging / "manifest.tsv", error)) {
    filesystem::remove_all(staging, filesystemError);
    return false;
  }
  for (const auto& entry : entries) {
    if (!copyBackupFile(backupDirectory / entry.name, staging / entry.name, error)) {
      filesystem::remove_all(staging, filesystemError);
      return false;
    }
  }
  if (!validateInventatoryBackup(staging, error)) {
    filesystem::remove_all(staging, filesystemError);
    return false;
  }

  AppSettings restoredSettings;
  if (!loadAppSettings(staging / "settings.conf", restoredSettings)) {
    filesystem::remove_all(staging, filesystemError);
    error = "Restored settings are invalid";
    return false;
  }
  restoredSettings.dataDirectory = destinationDirectory;
  const auto settingsBackup = filesystem::path(appSettingsPath.string() + ".restore-old");
  if (filesystem::exists(appSettingsPath) && !copyBackupFile(appSettingsPath, settingsBackup, error)) {
    filesystem::remove_all(staging, filesystemError);
    return false;
  }

  const auto oldDirectory = filesystem::path(destinationDirectory.string() + ".restore-old-data");
  filesystem::remove_all(oldDirectory, filesystemError);
  filesystemError.clear();
  if (filesystem::exists(destinationDirectory)) {
    filesystem::rename(destinationDirectory, oldDirectory, filesystemError);
    if (filesystemError) {
      error = "Unable to protect the current data before restore: " + filesystemError.message();
      filesystem::remove_all(staging, filesystemError);
      return false;
    }
  }
  filesystem::rename(staging, destinationDirectory, filesystemError);
  if (filesystemError || !saveAppSettings(appSettingsPath, restoredSettings)) {
    error = filesystemError ? "Unable to activate restored data: " + filesystemError.message()
                            : "Unable to activate restored settings";
    filesystem::remove_all(destinationDirectory, filesystemError);
    if (filesystem::exists(oldDirectory)) filesystem::rename(oldDirectory, destinationDirectory, filesystemError);
    if (filesystem::exists(settingsBackup)) {
      copyBackupFile(settingsBackup, appSettingsPath, error);
      filesystem::remove(settingsBackup, filesystemError);
    }
    return false;
  }
  filesystem::remove_all(oldDirectory, filesystemError);
  filesystem::remove(settingsBackup, filesystemError);
  return true;
}

}  // namespace inventatory
