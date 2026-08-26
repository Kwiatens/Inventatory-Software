// Inventatory - Hardware Inventory Management System
// Versioned user-level application settings, stored outside inventory data.

#include "app/AppSettings.h"

#include "platform/Environment.h"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace inventatory {

using namespace std;

namespace {

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

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

const char* appearanceColorKey(AppearanceColorRole role) {
  switch (role) {
    case AppearanceColorRole::CanvasBg: return "canvas_bg";
    case AppearanceColorRole::SurfaceBg: return "surface_bg";
    case AppearanceColorRole::RaisedSurfaceBg: return "raised_surface_bg";
    case AppearanceColorRole::HoverBg: return "hover_bg";
    case AppearanceColorRole::SelectionBg: return "selection_bg";
    case AppearanceColorRole::Divider: return "divider";
    case AppearanceColorRole::PrimaryText: return "primary_text";
    case AppearanceColorRole::SecondaryText: return "secondary_text";
    case AppearanceColorRole::MutedText: return "muted_text";
    case AppearanceColorRole::FocusText: return "focus_text";
    case AppearanceColorRole::Interactive: return "interactive";
    case AppearanceColorRole::Success: return "success";
    case AppearanceColorRole::Link: return "link";
    case AppearanceColorRole::WarningText: return "warning_text";
    case AppearanceColorRole::DangerText: return "danger_text";
    case AppearanceColorRole::ActiveBg: return "active_bg";
    case AppearanceColorRole::ActiveSoftBg: return "active_soft_bg";
    case AppearanceColorRole::WarningBg: return "warning_bg";
    case AppearanceColorRole::DangerBg: return "danger_bg";
    case AppearanceColorRole::DangerFlashBg: return "danger_flash_bg";
    case AppearanceColorRole::Count: break;
  }
  return "unknown";
}

const char* appearanceColorLabel(AppearanceColorRole role) {
  switch (role) {
    case AppearanceColorRole::CanvasBg: return "Canvas background";
    case AppearanceColorRole::SurfaceBg: return "Surface background";
    case AppearanceColorRole::RaisedSurfaceBg: return "Raised surface";
    case AppearanceColorRole::HoverBg: return "Hover background";
    case AppearanceColorRole::SelectionBg: return "Selection background";
    case AppearanceColorRole::Divider: return "Divider";
    case AppearanceColorRole::PrimaryText: return "Primary text";
    case AppearanceColorRole::SecondaryText: return "Secondary text";
    case AppearanceColorRole::MutedText: return "Muted text";
    case AppearanceColorRole::FocusText: return "Focus text";
    case AppearanceColorRole::Interactive: return "Interactive / accent";
    case AppearanceColorRole::Success: return "Success";
    case AppearanceColorRole::Link: return "Link";
    case AppearanceColorRole::WarningText: return "Warning text";
    case AppearanceColorRole::DangerText: return "Danger text";
    case AppearanceColorRole::ActiveBg: return "Active background";
    case AppearanceColorRole::ActiveSoftBg: return "Active soft background";
    case AppearanceColorRole::WarningBg: return "Warning background";
    case AppearanceColorRole::DangerBg: return "Danger background";
    case AppearanceColorRole::DangerFlashBg: return "Danger flash background";
    case AppearanceColorRole::Count: break;
  }
  return "Unknown";
}

string appearanceColorHex(uint32_t rgb) {
  ostringstream output;
  output << '#' << uppercase << hex << setw(6) << setfill('0') << (rgb & 0xFFFFFFu);
  return output.str();
}

bool parseAppearanceColorHex(const string& text, uint32_t& rgb) {
  string value;
  for (const char character : text) {
    if (!isspace(static_cast<unsigned char>(character))) value.push_back(character);
  }
  if (!value.empty() && value.front() == '#') value.erase(value.begin());
  if (value.size() != 6) return false;

  uint32_t parsed = 0;
  for (const char character : value) {
    const int digit = hexDigit(character);
    if (digit < 0) return false;
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
  }
  rgb = parsed;
  return true;
}

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
    } else if (key == "device_service_port") {
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
    } else if (key == "low_stock_threshold") {
      int threshold = loaded.lowStockThreshold;
      value >> threshold;
      if (threshold > 0) loaded.lowStockThreshold = threshold;
    } else if (key.rfind("appearance_", 0) == 0) {
      for (size_t index = 0; index < kAppearanceColorCount; ++index) {
        const auto role = static_cast<AppearanceColorRole>(index);
        if (key == string("appearance_") + appearanceColorKey(role)) {
          string encoded;
          value >> encoded;
          uint32_t parsed = loaded.appearance.colors[index];
          if (parseAppearanceColorHex(encoded, parsed)) loaded.appearance.colors[index] = parsed;
          break;
        }
      }
    } else if (key == "tolerance_resistance") {
      double tol = loaded.physicalValueTolerances.resistance;
      value >> tol;
      if (tol >= 0 && tol <= 1) loaded.physicalValueTolerances.resistance = tol;
    } else if (key == "tolerance_capacitance") {
      double tol = loaded.physicalValueTolerances.capacitance;
      value >> tol;
      if (tol >= 0 && tol <= 1) loaded.physicalValueTolerances.capacitance = tol;
    } else if (key == "tolerance_inductance") {
      double tol = loaded.physicalValueTolerances.inductance;
      value >> tol;
      if (tol >= 0 && tol <= 1) loaded.physicalValueTolerances.inductance = tol;
    } else if (key == "tolerance_frequency") {
      double tol = loaded.physicalValueTolerances.frequency;
      value >> tol;
      if (tol >= 0 && tol <= 1) loaded.physicalValueTolerances.frequency = tol;
    }
  }
  if (loaded.schemaVersion < 1 || loaded.lowStockThreshold <= 0) return false;
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
          << "digikey_currency=" << quoted(settings.digiKeyCurrency) << '\n'
          << "low_stock_threshold=" << settings.lowStockThreshold << '\n';
  for (size_t index = 0; index < kAppearanceColorCount; ++index) {
    const auto role = static_cast<AppearanceColorRole>(index);
    output << "appearance_" << appearanceColorKey(role) << '='
           << appearanceColorHex(settings.appearance.colors[index]) << '\n';
   }
   output << "tolerance_resistance=" << settings.physicalValueTolerances.resistance << '\n'
          << "tolerance_capacitance=" << settings.physicalValueTolerances.capacitance << '\n'
          << "tolerance_inductance=" << settings.physicalValueTolerances.inductance << '\n'
          << "tolerance_frequency=" << settings.physicalValueTolerances.frequency << '\n';
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
