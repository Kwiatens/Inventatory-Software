// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <filesystem>
#include <functional>

namespace inventatory {

struct InventatoryDataPaths {
  std::filesystem::path dataDirectory;
  std::filesystem::path inventory;
  std::filesystem::path printer;
  std::filesystem::path activity;
  std::filesystem::path scanConfig;
};

InventatoryDataPaths makeInventatoryDataPaths(const std::filesystem::path& dataDirectory);
bool switchInventatoryDataPathsAfterSaving(InventatoryDataPaths& active,
                                           const std::filesystem::path& nextDataDirectory,
                                           const std::function<bool()>& saveCurrentState);
bool onboardingRequired(bool startInBackground, bool settingsLoaded, int completedOnboardingVersion);

}  // namespace inventatory
