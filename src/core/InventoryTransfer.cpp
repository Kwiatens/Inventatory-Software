// Inventatory - Inventory export and data-folder backup helpers.

#include "core/InventoryTransfer.h"

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

struct BackupEntry {
  string name;
  uintmax_t size = 0;
  string hash;
};

constexpr uintmax_t kMaximumBackupManifestBytes = 1U * 1024U * 1024U;
constexpr uintmax_t kMaximumBackupPayloadBytes = 512U * 1024U * 1024U;
constexpr uintmax_t kMaximumRestoreJournalBytes = 64U * 1024U;

string hexBytes(const unsigned char* data, size_t count) {
  ostringstream output;
  output << hex << setfill('0');
  for (size_t index = 0; index < count; ++index) output << setw(2) << static_cast<unsigned int>(data[index]);
  return output.str();
}

// Keep the manifest format cryptographically meaningful on every build. The
// PC product is Windows-only, but using a real portable implementation here
// also keeps validation and tests deterministic on other toolchains.
class Sha256 {
 public:
  Sha256() : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

  void update(const unsigned char* data, size_t length) {
    bitCount_ += static_cast<uint64_t>(length) * 8u;
    while (length != 0) {
      const size_t count = min(length, block_.size() - used_);
      copy(data, data + count, block_.begin() + static_cast<ptrdiff_t>(used_));
      used_ += count;
      data += count;
      length -= count;
      if (used_ == block_.size()) {
        transform(block_.data());
        used_ = 0;
      }
    }
  }

  array<unsigned char, 32> finish() {
    block_[used_++] = 0x80;
    if (used_ > 56) {
      fill(block_.begin() + static_cast<ptrdiff_t>(used_), block_.end(), 0);
      transform(block_.data());
      used_ = 0;
    }
    fill(block_.begin() + static_cast<ptrdiff_t>(used_), block_.begin() + 56, 0);
    for (size_t index = 0; index < 8; ++index) {
      block_[63 - index] = static_cast<unsigned char>(bitCount_ >> (index * 8));
    }
    transform(block_.data());

    array<unsigned char, 32> digest{};
    for (size_t index = 0; index < state_.size(); ++index) {
      digest[index * 4] = static_cast<unsigned char>(state_[index] >> 24);
      digest[index * 4 + 1] = static_cast<unsigned char>(state_[index] >> 16);
      digest[index * 4 + 2] = static_cast<unsigned char>(state_[index] >> 8);
      digest[index * 4 + 3] = static_cast<unsigned char>(state_[index]);
    }
    return digest;
  }

 private:
  static constexpr array<uint32_t, 64> kConstants = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
      0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
      0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
      0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
      0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
      0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
      0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
      0xc67178f2u};

  static uint32_t rotateRight(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32 - count));
  }

  void transform(const unsigned char* block) {
    array<uint32_t, 64> words{};
    for (size_t index = 0; index < 16; ++index) {
      words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                     (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                     (static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
                     static_cast<uint32_t>(block[index * 4 + 3]);
    }
    for (size_t index = 16; index < words.size(); ++index) {
      const uint32_t s0 = rotateRight(words[index - 15], 7) ^ rotateRight(words[index - 15], 18) ^
                          (words[index - 15] >> 3);
      const uint32_t s1 = rotateRight(words[index - 2], 17) ^ rotateRight(words[index - 2], 19) ^
                          (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (size_t index = 0; index < words.size(); ++index) {
      const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const uint32_t choose = (e & f) ^ (~e & g);
      const uint32_t t1 = h + s1 + choose + kConstants[index] + words[index];
      const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + majority;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
  }

  array<uint32_t, 8> state_;
  array<unsigned char, 64> block_{};
  size_t used_ = 0;
  uint64_t bitCount_ = 0;
};

constexpr array<uint32_t, 64> Sha256::kConstants;

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
  ifstream input(path, ios::binary);
  if (!input) {
    error = "Unable to read " + path.string();
    return false;
  }
  Sha256 digest;
  array<unsigned char, 64 * 1024> buffer{};
  while (true) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) digest.update(buffer.data(), static_cast<size_t>(count));
    if (input.eof()) break;
    if (!input) {
      error = "Unable to read " + path.string();
      return false;
    }
  }
  const auto result = digest.finish();
  hash = hexBytes(result.data(), result.size());
  return true;
#endif
}

bool containsLineBreakOrTab(const string& value) {
  return value.find_first_of("\t\r\n") != string::npos;
}

vector<string> splitManifestRow(string line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  vector<string> fields;
  size_t start = 0;
  while (true) {
    const auto tab = line.find('\t', start);
    if (tab == string::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

bool supportedBackupName(const string& name) {
  return name == "inventory.db" || name == "activity.tsv" || name == "printer.conf" ||
         name == "quick_labels.conf" || name == "settings.conf";
}

bool isSha256Digest(const string& value) {
  if (value.size() != 64) return false;
  return all_of(value.begin(), value.end(), [](unsigned char ch) { return isxdigit(ch) != 0; });
}

string lowercaseAscii(string value) {
  transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  return value;
}

bool parseManifestSize(const string& text, uintmax_t& value) {
  if (text.empty() || !all_of(text.begin(), text.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) return false;
  try {
    size_t consumed = 0;
    const auto parsed = stoull(text, &consumed, 10);
    if (consumed != text.size() || parsed > (numeric_limits<uintmax_t>::max)()) return false;
    value = static_cast<uintmax_t>(parsed);
    return true;
  } catch (const exception&) {
    return false;
  }
}

bool writeManifest(const filesystem::path& destination, const string& applicationVersion,
                   const vector<BackupEntry>& entries, string& error) {
  if (applicationVersion.empty() || containsLineBreakOrTab(applicationVersion)) {
    error = "Backup application version is invalid";
    return false;
  }
  ofstream output(destination / "manifest.tsv", ios::binary | ios::trunc);
  if (!output) {
    error = "Unable to create backup manifest";
    return false;
  }
  output << "inventatory_backup_format\t1\n" << "application_version\t" << applicationVersion << "\n";
  for (const auto& entry : entries) {
    output << "file\t" << entry.name << '\t' << entry.size << '\t' << entry.hash << '\n';
  }
  output.flush();
  output.close();
  if (!output) {
    error = "Unable to finish backup manifest";
    return false;
  }
  return true;
}

bool readManifest(const filesystem::path& backupDirectory, vector<BackupEntry>& entries, string& error) {
  error_code sizeError;
  const auto manifestSize = filesystem::file_size(backupDirectory / "manifest.tsv", sizeError);
  if (sizeError || manifestSize > kMaximumBackupManifestBytes) {
    error = "Backup manifest is missing or too large";
    return false;
  }
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
    if (line.empty() || line.find('\0') != string::npos) {
      error = "Backup manifest contains an empty or invalid row";
      return false;
    }
    const auto fields = splitManifestRow(line);
    if (fields[0] == "inventatory_backup_format") {
      if (fields.size() != 2 || formatFound || fields[1] != "1") {
        error = "Backup manifest has an invalid or duplicate format row";
        return false;
      }
      formatFound = true;
    } else if (fields[0] == "application_version") {
      if (fields.size() != 2 || applicationVersionFound || fields[1].empty() ||
          containsLineBreakOrTab(fields[1])) {
        error = "Backup manifest has an invalid or duplicate application version row";
        return false;
      }
      applicationVersionFound = true;
    } else if (fields[0] == "file") {
      if (fields.size() != 4 || !supportedBackupName(fields[1]) ||
          fields[1].find_first_of("/\\") != string::npos || !names.insert(fields[1]).second ||
          !isSha256Digest(fields[3])) {
        error = "Backup manifest contains an invalid or duplicate file row";
        return false;
      }
      BackupEntry entry;
      entry.name = fields[1];
      if (!parseManifestSize(fields[2], entry.size)) {
        error = "Backup manifest contains an invalid file size";
        return false;
      }
      entry.hash = fields[3];
      entries.push_back(move(entry));
    } else {
      error = "Backup manifest contains an unknown row";
      return false;
    }
  }
  if (!input.eof()) {
    error = "Unable to read backup manifest";
    return false;
  }
  if (!formatFound || !applicationVersionFound || entries.empty() || names.count("inventory.db") == 0 ||
      names.count("settings.conf") == 0) {
    error = "Backup manifest is incomplete";
    return false;
  }
  return true;
}

bool validateWorkspaceData(const filesystem::path& directory, string& error);

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

struct TransferOps {
  const InventoryTransferTestHooks* hooks = nullptr;

  bool copy(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->copyFile) return hooks->copyFile(source, destination, error);
    error_code filesystemError;
    filesystem::copy_file(source, destination, filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError) {
      error = "Unable to copy " + source.filename().string() + ": " + filesystemError.message();
      return false;
    }
    return true;
  }

  bool rename(const filesystem::path& source, const filesystem::path& destination, string& error) const {
    if (hooks != nullptr && hooks->renamePath) return hooks->renamePath(source, destination, error);
    error_code filesystemError;
    filesystem::rename(source, destination, filesystemError);
    if (filesystemError) {
      error = "Unable to rename " + source.string() + ": " + filesystemError.message();
      return false;
    }
    return true;
  }

  bool removeAll(const filesystem::path& path, string& error) const {
    if (hooks != nullptr && hooks->removeAll) return hooks->removeAll(path, error);
    error_code filesystemError;
    filesystem::remove_all(path, filesystemError);
    if (filesystemError) {
      error = "Unable to remove " + path.string() + ": " + filesystemError.message();
      return false;
    }
    return true;
  }

  bool replace(const filesystem::path& source, const filesystem::path& destination, string& error) const {
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
};

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

struct RestoreJournal {
  filesystem::path destination;
  filesystem::path staging;
  filesystem::path oldData;
  filesystem::path settings;
  filesystem::path settingsBackup;
  bool destinationExisted = false;
  bool settingsExisted = false;
  string state;
};

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
