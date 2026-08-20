// Inventatory - Hardware Inventory Management System
// Versioned user-level application settings, stored outside inventory data.

#include "app/AppSettings.h"

#include "platform/Environment.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace inventatory {

using namespace std;

namespace {

bool parseBool(const string& value, bool fallback) {
  if (value == "1" || value == "true") return true;
  if (value == "0" || value == "false") return false;
  return fallback;
}

bool replaceSettingsFile(const filesystem::path& path, const filesystem::path& temporary) {
#ifdef _WIN32
  return MoveFileExA(temporary.string().c_str(), path.string().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  error_code error;
  filesystem::rename(temporary, path, error);
  return !error;
#endif
}

}  // namespace

filesystem::path appSettingsDirectory() {
  if (const auto value = environmentValue("LOCALAPPDATA"); value.has_value() && !value->empty()) {
    return filesystem::path(*value) / "Inventatory";
  }
  if (const auto value = environmentValue("USERPROFILE"); value.has_value() && !value->empty()) {
    return filesystem::path(*value) / "AppData" / "Local" / "Inventatory";
  }
  return filesystem::current_path() / ".inventatory";
}

filesystem::path appSettingsPath() {
  return appSettingsDirectory() / "settings.conf";
}

bool loadAppSettings(const filesystem::path& path, AppSettings& settings) {
  ifstream input(path);
  if (!input) return false;

  AppSettings loaded;
  string line;
  while (getline(input, line)) {
    const auto equals = line.find('=');
    if (equals == string::npos) continue;
    const auto key = line.substr(0, equals);
    istringstream value(line.substr(equals + 1));
    if (key == "schema_version") {
      value >> loaded.schemaVersion;
    } else if (key == "completed_onboarding_version") {
      value >> loaded.completedOnboardingVersion;
    } else if (key == "data_directory") {
      string decoded;
      value >> quoted(decoded);
      loaded.dataDirectory = decoded;
    } else if (key == "printer_queue") {
      value >> quoted(loaded.printerQueue);
    } else if (key == "auto_print_scanned_labels") {
      string decoded;
      value >> decoded;
      loaded.autoPrintScannedLabels = parseBool(decoded, loaded.autoPrintScannedLabels);
    } else if (key == "background_service_enabled") {
      string decoded;
      value >> decoded;
      loaded.backgroundServiceEnabled = parseBool(decoded, loaded.backgroundServiceEnabled);
    } else if (key == "background_consent_asked") {
      string decoded;
      value >> decoded;
      loaded.backgroundConsentAsked = parseBool(decoded, loaded.backgroundConsentAsked);
    } else if (key == "update_checks_enabled") {
      string decoded;
      value >> decoded;
      loaded.updateChecksEnabled = parseBool(decoded, loaded.updateChecksEnabled);
    } else if (key == "last_update_check_unix_seconds") {
      value >> loaded.lastUpdateCheckUnixSeconds;
    } else if (key == "latest_available_version") {
      value >> quoted(loaded.latestAvailableVersion);
    } else if (key == "latest_release_url") {
      value >> quoted(loaded.latestReleaseUrl);
    } else if (key == "device_service_port" || key == "bridge_port") {
      // bridge_port is the pre-v2 name and remains readable for migration.
      unsigned int port = loaded.deviceServicePort;
      value >> port;
      if (port >= 1 && port <= 65535) loaded.deviceServicePort = static_cast<uint16_t>(port);
    } else if (key == "digikey_client_id") {
      value >> quoted(loaded.digiKeyClientId);
    } else if (key == "digikey_account_id") {
      value >> quoted(loaded.digiKeyAccountId);
    } else if (key == "digikey_site") {
      value >> quoted(loaded.digiKeySite);
    } else if (key == "digikey_language") {
      value >> quoted(loaded.digiKeyLanguage);
    } else if (key == "digikey_currency") {
      value >> quoted(loaded.digiKeyCurrency);
    } else if (key == "quick_label") {
      string preset;
      value >> quoted(preset);
      if (!preset.empty() && loaded.quickLabelPresets.size() < 12) loaded.quickLabelPresets.push_back(preset);
    } else if (key == "quick_label_revision") {
      unsigned long revision = loaded.quickLabelRevision;
      value >> revision;
      if (revision > 0 && revision <= UINT32_MAX) loaded.quickLabelRevision = static_cast<uint32_t>(revision);
    }
  }
  if (loaded.schemaVersion < 1 || loaded.schemaVersion > 2) return false;
  if (loaded.schemaVersion == 1) {
    loaded.schemaVersion = 2;
  }
  settings = move(loaded);
  return true;
}

bool saveAppSettings(const filesystem::path& path, const AppSettings& settings) {
  error_code error;
  filesystem::create_directories(path.parent_path(), error);
  if (error) return false;

  const auto temporary = filesystem::path(path.string() + ".tmp");
  ofstream output(temporary, ios::trunc);
  if (!output) return false;
  output << "schema_version=" << settings.schemaVersion << '\n'
         << "completed_onboarding_version=" << settings.completedOnboardingVersion << '\n'
         << "data_directory=" << quoted(settings.dataDirectory.string()) << '\n'
         << "printer_queue=" << quoted(settings.printerQueue) << '\n'
         << "auto_print_scanned_labels=" << (settings.autoPrintScannedLabels ? "true" : "false") << '\n'
         << "background_service_enabled=" << (settings.backgroundServiceEnabled ? "true" : "false") << '\n'
         << "background_consent_asked=" << (settings.backgroundConsentAsked ? "true" : "false") << '\n'
         << "update_checks_enabled=" << (settings.updateChecksEnabled ? "true" : "false") << '\n'
         << "last_update_check_unix_seconds=" << settings.lastUpdateCheckUnixSeconds << '\n'
         << "latest_available_version=" << quoted(settings.latestAvailableVersion) << '\n'
         << "latest_release_url=" << quoted(settings.latestReleaseUrl) << '\n'
         << "device_service_port=" << settings.deviceServicePort << '\n'
         << "digikey_client_id=" << quoted(settings.digiKeyClientId) << '\n'
         << "digikey_account_id=" << quoted(settings.digiKeyAccountId) << '\n'
         << "digikey_site=" << quoted(settings.digiKeySite) << '\n'
         << "digikey_language=" << quoted(settings.digiKeyLanguage) << '\n'
         << "digikey_currency=" << quoted(settings.digiKeyCurrency) << '\n';
  output.close();
  if (!output) return false;
  if (!replaceSettingsFile(path, temporary)) {
    filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

filesystem::path quickLabelsPath(const filesystem::path& dataDirectory) {
  return dataDirectory / "quick_labels.conf";
}

bool loadQuickLabels(const filesystem::path& path, vector<string>& presets, uint32_t& revision) {
  ifstream input(path);
  if (!input) return false;

  vector<string> loadedPresets;
  uint32_t loadedRevision = revision;
  string line;
  while (getline(input, line)) {
    const auto equals = line.find('=');
    if (equals == string::npos) continue;
    const auto key = line.substr(0, equals);
    istringstream value(line.substr(equals + 1));
    if (key == "quick_label") {
      string preset;
      value >> quoted(preset);
      if (!preset.empty() && loadedPresets.size() < 12) loadedPresets.push_back(preset);
    } else if (key == "quick_label_revision") {
      unsigned long parsedRevision = loadedRevision;
      value >> parsedRevision;
      if (parsedRevision > 0 && parsedRevision <= UINT32_MAX) loadedRevision = static_cast<uint32_t>(parsedRevision);
    }
  }
  presets = move(loadedPresets);
  revision = loadedRevision;
  return true;
}

bool saveQuickLabels(const filesystem::path& path, const vector<string>& presets, uint32_t revision) {
  error_code error;
  filesystem::create_directories(path.parent_path(), error);
  if (error) return false;

  const auto temporary = filesystem::path(path.string() + ".tmp");
  ofstream output(temporary, ios::trunc);
  if (!output) return false;
  output << "quick_label_revision=" << revision << '\n';
  for (const auto& preset : presets) {
    output << "quick_label=" << quoted(preset) << '\n';
  }
  output.close();
  if (!output) return false;
  if (!replaceSettingsFile(path, temporary)) {
    filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

}  // namespace inventatory
