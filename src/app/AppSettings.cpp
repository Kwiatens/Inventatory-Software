// HIMS - Hardware Inventory Management System
// Versioned user-level application settings, stored outside inventory data.

#include "app/AppSettings.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hims {

using namespace std;

namespace {

bool parseBool(const string& value, bool fallback) {
  if (value == "1" || value == "true") return true;
  if (value == "0" || value == "false") return false;
  return fallback;
}

}  // namespace

filesystem::path appSettingsDirectory() {
  if (const char* value = getenv("LOCALAPPDATA"); value != nullptr && *value != '\0') {
    return filesystem::path(value) / "HIMS";
  }
  if (const char* value = getenv("USERPROFILE"); value != nullptr && *value != '\0') {
    return filesystem::path(value) / "AppData" / "Local" / "HIMS";
  }
  return filesystem::current_path() / ".hims";
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
    }
  }
  if (loaded.schemaVersion != 1) return false;
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
         << "data_directory=" << quoted(settings.dataDirectory.string()) << '\n'
         << "printer_queue=" << quoted(settings.printerQueue) << '\n'
         << "auto_print_scanned_labels=" << (settings.autoPrintScannedLabels ? "true" : "false") << '\n'
         << "device_service_port=" << settings.deviceServicePort << '\n'
         << "digikey_client_id=" << quoted(settings.digiKeyClientId) << '\n'
         << "digikey_account_id=" << quoted(settings.digiKeyAccountId) << '\n'
         << "digikey_site=" << quoted(settings.digiKeySite) << '\n'
         << "digikey_language=" << quoted(settings.digiKeyLanguage) << '\n'
         << "digikey_currency=" << quoted(settings.digiKeyCurrency) << '\n';
  output.close();
  if (!output) return false;
  filesystem::remove(path, error);
  error.clear();
  filesystem::rename(temporary, path, error);
  return !error;
}

}  // namespace hims
