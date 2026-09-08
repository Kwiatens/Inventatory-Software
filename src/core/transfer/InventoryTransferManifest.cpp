// Inventatory - Backup hashing and manifest serialization.

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
namespace inventory_transfer_detail {

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

}  // namespace inventory_transfer_detail
}  // namespace inventatory
