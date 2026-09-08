// Inventatory - Private inventory-transfer implementation contracts.

#pragma once

#include "core/transfer/InventoryTransfer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace inventatory {
namespace inventory_transfer_detail {

using std::size_t;
using std::string;
using std::uintmax_t;
using std::vector;

inline constexpr uintmax_t kMaximumBackupManifestBytes = 1U * 1024U * 1024U;
inline constexpr uintmax_t kMaximumBackupPayloadBytes = 512U * 1024U * 1024U;
inline constexpr uintmax_t kMaximumRestoreJournalBytes = 64U * 1024U;

struct BackupEntry {
  string name;
  uintmax_t size = 0;
  string hash;
};

struct RestoreJournal {
  std::filesystem::path destination;
  std::filesystem::path staging;
  std::filesystem::path oldData;
  std::filesystem::path settings;
  std::filesystem::path settingsBackup;
  bool destinationExisted = false;
  bool settingsExisted = false;
  string state;
};

struct TransferOps {
  const InventoryTransferTestHooks* hooks = nullptr;

  bool copy(const std::filesystem::path& source, const std::filesystem::path& destination, string& error) const;
  bool rename(const std::filesystem::path& source, const std::filesystem::path& destination, string& error) const;
  bool removeAll(const std::filesystem::path& path, string& error) const;
  bool replace(const std::filesystem::path& source, const std::filesystem::path& destination, string& error) const;
};

string hexBytes(const unsigned char* data, size_t count);
bool sha256File(const std::filesystem::path& path, string& hash, string& error);
bool containsLineBreakOrTab(const string& value);
vector<string> splitManifestRow(string line);
bool supportedBackupName(const string& name);
bool isSha256Digest(const string& value);
string lowercaseAscii(string value);
bool parseManifestSize(const string& text, uintmax_t& value);
bool writeManifest(const std::filesystem::path& destination, const string& applicationVersion,
                   const vector<BackupEntry>& entries, string& error);
bool readManifest(const std::filesystem::path& backupDirectory, vector<BackupEntry>& entries, string& error);

bool validateEntries(const std::filesystem::path& directory, const vector<BackupEntry>& entries, string& error);
bool validateWorkspaceData(const std::filesystem::path& directory, string& error);
bool validateWorkspaceSettings(const std::filesystem::path& settingsPath,
                               const std::filesystem::path& dataDirectory, string& error);
bool equivalentPath(const std::filesystem::path& first, const std::filesystem::path& second);
bool isLinkedOrReparseArtifact(const std::filesystem::path& path, std::error_code& error);
bool pathsOverlap(const std::filesystem::path& first, const std::filesystem::path& second, bool& overlaps, string& error);
bool ownedRestoreSibling(const std::filesystem::path& artifact, const std::filesystem::path& base, const string& marker);
bool inspectOwnedArtifact(const std::filesystem::path& artifact, const std::filesystem::path& base,
                          const string& marker, bool expectedDirectory, bool required, bool& exists, string& error);
bool validateJournalOwnership(const RestoreJournal& journal, const std::filesystem::path& destination,
                              const std::filesystem::path& settings, string& error);

bool copyToAtomicTarget(const std::filesystem::path& source, const std::filesystem::path& target,
                        const TransferOps& ops, string& error);
bool restoreSettingsFromJournal(const RestoreJournal& journal, const TransferOps& ops, string& error);
bool rollbackRestore(const RestoreJournal& journal, const TransferOps& ops, string& error);
bool cleanupCommittedRestore(const RestoreJournal& journal, const TransferOps& ops, string& error);

bool saveRestoreSettings(const std::filesystem::path& path, const AppSettings& settings, const TransferOps& ops,
                         string& error);
std::filesystem::path uniqueSibling(const std::filesystem::path& base, const string& suffix, size_t attempt);
bool chooseUnusedSibling(const std::filesystem::path& base, const string& suffix,
                         std::filesystem::path& result, string& error);
bool createUniqueDirectory(const std::filesystem::path& base, const string& suffix,
                           std::filesystem::path& result, string& error);
bool atomicWriteText(const std::filesystem::path& target, const string& text,
                     const TransferOps& ops, string& error);
string hexEncode(const string& value);
bool hexDecode(const string& value, string& result);
std::filesystem::path restoreJournalPath(const std::filesystem::path& settings);
bool writeRestoreJournal(const std::filesystem::path& path, const RestoreJournal& journal,
                         const TransferOps& ops, string& error);
bool readRestoreJournal(const std::filesystem::path& path, RestoreJournal& journal, string& error);

}  // namespace inventory_transfer_detail
}  // namespace inventatory
