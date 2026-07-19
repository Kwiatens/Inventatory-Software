// Signed IECD snapshot download, validation, and atomic installation.

#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace inventatory {

struct IecdInstallResult {
  bool completed = false;
  bool installed = false;
  std::string databaseVersion;
  std::string error;
};

const std::array<unsigned char, 32>& iecdReleasePublicKey();
IecdInstallResult installIecdSnapshot(const std::string& manifestJson,
                                      const std::filesystem::path& candidateDatabase,
                                      const std::filesystem::path& installedDatabase);
IecdInstallResult downloadAndInstallLatestIecd(const std::filesystem::path& installedDatabase);

}  // namespace inventatory
