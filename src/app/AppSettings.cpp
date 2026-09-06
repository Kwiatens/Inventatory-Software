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
  return consumed == encoded.size();
}

bool parseBool(const string& value, bool& result);

bool parseBoolValue(istringstream& input, bool& value) {
  string encoded;
  string trailing;
  return (input >> encoded) && !(input >> trailing) && parseBool(encoded, value);
}

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
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
  if (!fileWithinLimit(path, kMaxSettingsFileBytes)) return false;
  ifstream input(path);
  if (!input) return false;

  AppSettings loaded;
  bool hasSchemaVersion = false;
  string line;
  while (getline(input, line)) {
    if (line.size() > kMaxSettingsFileBytes) return false;
    const auto equals = line.find('=');
    if (equals == string::npos) continue;
    const auto key = line.substr(0, equals);
    istringstream value(line.substr(equals + 1));
    if (key == "schema_version") {
      uint64_t parsed = 0;
      if (!parseUnsignedValue(value, parsed) || parsed > numeric_limits<int>::max()) return false;
      loaded.schemaVersion = static_cast<int>(parsed);
      hasSchemaVersion = true;
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
      if (!parseUnsignedValue(value, parsed) || parsed > numeric_limits<int64_t>::max()) return false;
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
    } else if (key == "device_service_port") {
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
      if (!parseUnsignedValue(value, threshold)) return false;
      if (threshold > 0 && threshold <= numeric_limits<int>::max()) {
        loaded.lowStockThreshold = static_cast<int>(threshold);
      }
    } else if (key.rfind("appearance_", 0) == 0) {
      for (size_t index = 0; index < kAppearanceColorCount; ++index) {
        const auto role = static_cast<AppearanceColorRole>(index);
        if (key == string("appearance_") + appearanceColorKey(role)) {
          string encoded;
          if (!(value >> encoded)) return false;
          uint32_t parsed = loaded.appearance.colors[index];
          // Keep the established forward-compatible behavior for malformed
          // optional appearance values: use the default role color while
          // still bounding the input line above.
          if (parseAppearanceColorHex(encoded, parsed)) loaded.appearance.colors[index] = parsed;
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
  if (!input.eof() || !hasSchemaVersion || loaded.schemaVersion < 1 || loaded.lowStockThreshold <= 0 ||
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
    if (equals == string::npos) continue;
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
      if (!parseUnsignedValue(value, parsedRevision) || parsedRevision == 0 ||
          parsedRevision > numeric_limits<uint32_t>::max()) {
        return false;
      }
      loadedRevision = static_cast<uint32_t>(parsedRevision);
      hasRevision = true;
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
