// Inventatory - Hardware Inventory Management System
// Versioned user-level application settings, stored outside inventory data.

#include "app/AppSettings.h"

#include "core/AtomicFile.h"
#include "core/InventatoryScanProtocol.h"
#include "platform/Environment.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kMaxSettingsFileBytes = 64U * 1024U;
constexpr size_t kMaxQuickLabelsFileBytes = 16U * 1024U;
constexpr size_t kMaxDataDirectoryBytes = 32767U;
constexpr size_t kMaxPrinterQueueBytes = 1024U;
constexpr size_t kMaxVersionBytes = 128U;
constexpr size_t kMaxUrlBytes = 4096U;
constexpr size_t kMaxDigiKeyFieldBytes = 512U;
constexpr size_t kMaxLocaleFieldBytes = 32U;

bool validOptionalText(const string& value, size_t maximum) {
  return value.empty() || (value.size() <= maximum &&
                           all_of(value.begin(), value.end(), [](unsigned char character) {
                             return character != 0 && character != '\r' && character != '\n';
                           }));
}

bool validQuickLabel(const string& value) {
  return !value.empty() && value.size() <= kQuickLabelPresetTextLimit &&
         all_of(value.begin(), value.end(), [](unsigned char character) {
           return character != 0 && character != '\r' && character != '\n';
         });
}

bool validQuickLabels(const vector<string>& presets, uint32_t revision) {
  if (revision == 0 || presets.size() > kQuickLabelPresetLimit) return false;
  return all_of(presets.begin(), presets.end(), validQuickLabel);
}

bool validAppSettings(const AppSettings& settings) {
  if (settings.schemaVersion != 1 || settings.completedOnboardingVersion < 0 || settings.deviceServicePort == 0 ||
      settings.lowStockThreshold <= 0 || settings.appearance.colors.size() != kAppearanceColorCount ||
      settings.quickLabelRevision == 0) {
    return false;
  }
  string dataDirectory;
  try {
    dataDirectory = settings.dataDirectory.u8string();
  } catch (...) {
    return false;
  }
  return validOptionalText(dataDirectory, kMaxDataDirectoryBytes) &&
         validOptionalText(settings.printerQueue, kMaxPrinterQueueBytes) &&
         validOptionalText(settings.latestAvailableVersion, kMaxVersionBytes) &&
         validOptionalText(settings.latestReleaseUrl, kMaxUrlBytes) &&
         validOptionalText(settings.digiKeyClientId, kMaxDigiKeyFieldBytes) &&
         validOptionalText(settings.digiKeyAccountId, kMaxDigiKeyFieldBytes) &&
         validOptionalText(settings.digiKeySite, kMaxLocaleFieldBytes) &&
         validOptionalText(settings.digiKeyLanguage, kMaxLocaleFieldBytes) &&
         validOptionalText(settings.digiKeyCurrency, kMaxLocaleFieldBytes) &&
         validQuickLabels(settings.quickLabelPresets, settings.quickLabelRevision);
}

bool fileWithinLimit(const filesystem::path& path, size_t maximum) {
  error_code error;
  const auto size = filesystem::file_size(path, error);
  return !error && size <= maximum;
}

bool parseQuotedValue(istringstream& input, string& value) {
  if (!(input >> quoted(value))) return false;
  string trailing;
  return !(input >> trailing);
}

bool parseUnsignedValue(istringstream& input, uint64_t& value) {
  string encoded;
  if (!(input >> encoded) || encoded.empty()) return false;
  size_t consumed = 0;
  try {
    value = stoull(encoded, &consumed, 10);
  } catch (...) {
    return false;
  }
  string trailing;
  return consumed == encoded.size() && !(input >> trailing);
}

bool parseBool(const string& value, bool& result);

bool parseBoolValue(istringstream& input, bool& value) {
  string encoded;
  string trailing;
  return (input >> encoded) && !(input >> trailing) && parseBool(encoded, value);
}

bool parseBool(const string& value, bool& result) {
  if (value == "1" || value == "true") {
    result = true;
    return true;
  }
  if (value == "0" || value == "false") {
    result = false;
    return true;
  }
  return false;
}

bool requiredSettingsKey(const string& key) {
  if (key == "schema_version" || key == "completed_onboarding_version" || key == "data_directory" ||
      key == "printer_queue" || key == "auto_print_scanned_labels" || key == "background_service_enabled" ||
      key == "background_consent_asked" || key == "update_checks_enabled" ||
      key == "last_update_check_unix_seconds" || key == "latest_available_version" ||
      key == "latest_release_url" || key == "device_service_port" || key == "digikey_client_id" ||
      key == "digikey_account_id" || key == "digikey_site" || key == "digikey_language" ||
      key == "digikey_currency" || key == "low_stock_threshold") {
    return true;
  }
  for (size_t index = 0; index < kAppearanceColorCount; ++index) {
    if (key == string("appearance_") + appearanceColorKey(static_cast<AppearanceColorRole>(index))) return true;
  }
  return false;
}

const set<string>& persistedSettingsKeys() {
  static const set<string> keys = [] {
    set<string> result = {"schema_version",
                          "completed_onboarding_version",
                          "data_directory",
                          "printer_queue",
                          "auto_print_scanned_labels",
                          "background_service_enabled",
                          "background_consent_asked",
                          "update_checks_enabled",
                          "last_update_check_unix_seconds",
                          "latest_available_version",
                          "latest_release_url",
                          "device_service_port",
                          "digikey_client_id",
                          "digikey_account_id",
                          "digikey_site",
                          "digikey_language",
                          "digikey_currency",
                          "low_stock_threshold"};
    for (size_t index = 0; index < kAppearanceColorCount; ++index) {
      result.insert(string("appearance_") + appearanceColorKey(static_cast<AppearanceColorRole>(index)));
    }
    return result;
  }();
  return keys;
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
  if (!fileWithinLimit(path, kMaxSettingsFileBytes)) return false;
  ifstream input(path);
  if (!input) return false;

  AppSettings loaded;
  set<string> seenRequiredKeys;
  bool legacyFormat = false;
  bool malformedLine = false;
  bool malformedAppearance = false;
  string line;
  while (getline(input, line)) {
    if (line.size() > kMaxSettingsFileBytes) return false;
    const auto equals = line.find('=');
    if (equals == string::npos) {
      malformedLine = true;
      continue;
    }
    const auto key = line.substr(0, equals);
    if (key.empty()) return false;
    if (key == "bridge_port") {
      legacyFormat = true;
    } else if (key == "quick_label" || key == "quick_label_revision" || key.rfind("tolerance_", 0) == 0) {
      // These keys were written by older schema-1 settings and are kept as
      // ignorable migration inputs. Quick labels now have their own file.
      legacyFormat = true;
    } else if (key.rfind("appearance_", 0) == 0 && !requiredSettingsKey(key)) {
      return false;
    } else if (!requiredSettingsKey(key)) {
      return false;
    }
    string requiredKey = key == "bridge_port" ? "device_service_port" : key;
    if (requiredSettingsKey(requiredKey) && !seenRequiredKeys.insert(requiredKey).second) return false;
    istringstream value(line.substr(equals + 1));
    if (key == "schema_version") {
      uint64_t parsed = 0;
      if (!parseUnsignedValue(value, parsed) || parsed > numeric_limits<int>::max()) return false;
      loaded.schemaVersion = static_cast<int>(parsed);
      if (loaded.schemaVersion == 2) legacyFormat = true;
    } else if (key == "completed_onboarding_version") {
      uint64_t parsed = 0;
      if (!parseUnsignedValue(value, parsed) || parsed > numeric_limits<int>::max()) return false;
      loaded.completedOnboardingVersion = static_cast<int>(parsed);
    } else if (key == "data_directory") {
      string decoded;
      if (!parseQuotedValue(value, decoded) || decoded.size() > kMaxDataDirectoryBytes) return false;
      try {
        loaded.dataDirectory = filesystem::u8path(decoded);
      } catch (...) {
        return false;
      }
    } else if (key == "printer_queue") {
      if (!parseQuotedValue(value, loaded.printerQueue) || loaded.printerQueue.size() > kMaxPrinterQueueBytes) {
        return false;
      }
    } else if (key == "auto_print_scanned_labels") {
      if (!parseBoolValue(value, loaded.autoPrintScannedLabels)) return false;
    } else if (key == "background_service_enabled") {
      if (!parseBoolValue(value, loaded.backgroundServiceEnabled)) return false;
    } else if (key == "background_consent_asked") {
      if (!parseBoolValue(value, loaded.backgroundConsentAsked)) return false;
    } else if (key == "update_checks_enabled") {
      if (!parseBoolValue(value, loaded.updateChecksEnabled)) return false;
    } else if (key == "last_update_check_unix_seconds") {
      uint64_t parsed = 0;
      if (!parseUnsignedValue(value, parsed) ||
          parsed > static_cast<uint64_t>((numeric_limits<int64_t>::max)())) return false;
      loaded.lastUpdateCheckUnixSeconds = static_cast<int64_t>(parsed);
    } else if (key == "latest_available_version") {
      if (!parseQuotedValue(value, loaded.latestAvailableVersion) ||
          loaded.latestAvailableVersion.size() > kMaxVersionBytes) {
        return false;
      }
    } else if (key == "latest_release_url") {
      if (!parseQuotedValue(value, loaded.latestReleaseUrl) || loaded.latestReleaseUrl.size() > kMaxUrlBytes) {
        return false;
      }
    } else if (key == "device_service_port" || key == "bridge_port") {
      uint64_t port = 0;
      if (!parseUnsignedValue(value, port) || port < 1 || port > 65535) return false;
      loaded.deviceServicePort = static_cast<uint16_t>(port);
    } else if (key == "digikey_client_id") {
      if (!parseQuotedValue(value, loaded.digiKeyClientId) || loaded.digiKeyClientId.size() > kMaxDigiKeyFieldBytes) {
        return false;
      }
    } else if (key == "digikey_account_id") {
      if (!parseQuotedValue(value, loaded.digiKeyAccountId) ||
          loaded.digiKeyAccountId.size() > kMaxDigiKeyFieldBytes) {
        return false;
      }
    } else if (key == "digikey_site") {
      if (!parseQuotedValue(value, loaded.digiKeySite) || loaded.digiKeySite.size() > kMaxLocaleFieldBytes) return false;
    } else if (key == "digikey_language") {
      if (!parseQuotedValue(value, loaded.digiKeyLanguage) || loaded.digiKeyLanguage.size() > kMaxLocaleFieldBytes) {
        return false;
      }
    } else if (key == "digikey_currency") {
      if (!parseQuotedValue(value, loaded.digiKeyCurrency) ||
          loaded.digiKeyCurrency.size() > kMaxLocaleFieldBytes) {
        return false;
      }
    } else if (key == "low_stock_threshold") {
      uint64_t threshold = 0;
      if (!parseUnsignedValue(value, threshold) || threshold == 0 ||
          threshold > numeric_limits<int>::max()) return false;
      loaded.lowStockThreshold = static_cast<int>(threshold);
    } else if (key.rfind("appearance_", 0) == 0) {
      for (size_t index = 0; index < kAppearanceColorCount; ++index) {
        const auto role = static_cast<AppearanceColorRole>(index);
        if (key == string("appearance_") + appearanceColorKey(role)) {
          string encoded;
          string trailing;
          if (!(value >> encoded) || (value >> trailing)) return false;
          uint32_t parsed = loaded.appearance.colors[index];
          if (!parseAppearanceColorHex(encoded, parsed)) {
            malformedAppearance = true;
          } else {
            loaded.appearance.colors[index] = parsed;
          }
          break;
        }
      }
    }
  }
  string loadedDataDirectory;
  try {
    loadedDataDirectory = loaded.dataDirectory.u8string();
  } catch (...) {
    return false;
  }
  if (!input.eof() || malformedLine || malformedAppearance && !legacyFormat || loaded.schemaVersion < 1 ||
      loaded.schemaVersion > 2 || loaded.lowStockThreshold <= 0 ||
      loaded.lastUpdateCheckUnixSeconds < 0 || !validOptionalText(loadedDataDirectory, kMaxDataDirectoryBytes) ||
      !validOptionalText(loaded.printerQueue, kMaxPrinterQueueBytes) ||
      !validOptionalText(loaded.latestAvailableVersion, kMaxVersionBytes) ||
      !validOptionalText(loaded.latestReleaseUrl, kMaxUrlBytes) ||
      !validOptionalText(loaded.digiKeyClientId, kMaxDigiKeyFieldBytes) ||
      !validOptionalText(loaded.digiKeyAccountId, kMaxDigiKeyFieldBytes) ||
      !validOptionalText(loaded.digiKeySite, kMaxLocaleFieldBytes) ||
      !validOptionalText(loaded.digiKeyLanguage, kMaxLocaleFieldBytes) ||
      !validOptionalText(loaded.digiKeyCurrency, kMaxLocaleFieldBytes)) {
    return false;
  }
  if (loaded.schemaVersion == 2) legacyFormat = true;
  if (!legacyFormat && seenRequiredKeys != persistedSettingsKeys()) return false;
  if (loaded.schemaVersion == 2) loaded.schemaVersion = 1;
  settings = move(loaded);
  return true;
}

bool saveAppSettings(const filesystem::path& path, const AppSettings& settings) {
  if (!validAppSettings(settings)) return false;

  string dataDirectory;
  try {
    dataDirectory = settings.dataDirectory.u8string();
  } catch (...) {
    return false;
  }
  ostringstream output;
  output << "schema_version=" << settings.schemaVersion << '\n'
         << "completed_onboarding_version=" << settings.completedOnboardingVersion << '\n'
         << "data_directory=" << quoted(dataDirectory) << '\n'
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
  const auto text = output.str();
  if (text.size() > kMaxSettingsFileBytes) return false;
  string error;
  return writeFileAtomically(path, text, &error);
}

filesystem::path quickLabelsPath(const filesystem::path& dataDirectory) {
  return dataDirectory / "quick_labels.conf";
}

bool loadQuickLabels(const filesystem::path& path, vector<string>& presets, uint32_t& revision) {
  if (!fileWithinLimit(path, kMaxQuickLabelsFileBytes)) return false;
  ifstream input(path);
  if (!input) return false;

  vector<string> loadedPresets;
  uint32_t loadedRevision = revision;
  bool hasRevision = false;
  string line;
  while (getline(input, line)) {
    if (line.size() > kMaxQuickLabelsFileBytes) return false;
    const auto equals = line.find('=');
    if (equals == string::npos || equals == 0) return false;
    const auto key = line.substr(0, equals);
    istringstream value(line.substr(equals + 1));
    if (key == "quick_label") {
      string preset;
      if (!parseQuotedValue(value, preset) || !validQuickLabel(preset) ||
          loadedPresets.size() >= kQuickLabelPresetLimit) {
        return false;
      }
      loadedPresets.push_back(move(preset));
    } else if (key == "quick_label_revision") {
      uint64_t parsedRevision = 0;
      if (hasRevision || !parseUnsignedValue(value, parsedRevision) || parsedRevision == 0 ||
          parsedRevision > numeric_limits<uint32_t>::max()) {
        return false;
      }
      loadedRevision = static_cast<uint32_t>(parsedRevision);
      hasRevision = true;
    } else {
      return false;
    }
  }
  if (!input.eof() || !hasRevision || !validQuickLabels(loadedPresets, loadedRevision)) return false;
  presets = move(loadedPresets);
  revision = loadedRevision;
  return true;
}

bool saveQuickLabels(const filesystem::path& path, const vector<string>& presets, uint32_t revision) {
  if (!validQuickLabels(presets, revision)) return false;

  ostringstream output;
  output << "quick_label_revision=" << revision << '\n';
  for (const auto& preset : presets) {
    output << "quick_label=" << quoted(preset) << '\n';
  }
  const auto text = output.str();
  if (text.size() > kMaxQuickLabelsFileBytes) return false;
  string error;
  return writeFileAtomically(path, text, &error);
}

}  // namespace inventatory
