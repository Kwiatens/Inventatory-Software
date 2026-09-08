// Inventatory - Settings state and persistence.

#include "App.h"
#include "ui/pages/SettingsPagePrivate.h"

#include "platform/CredentialStore.h"
#include "platform/DigiKeyApi.h"
#include "platform/StartupRegistration.h"
#include "core/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;
using namespace settings_page_detail;

string App::settingsCategoryName(SettingsCategory category) const {
  switch (category) {
    case SettingsCategory::General: return "General / Data";
    case SettingsCategory::Updates: return "Updates";
    case SettingsCategory::Appearance: return "Appearance";
    case SettingsCategory::Printer: return "Printer";
    case SettingsCategory::QuickLabels: return "Quick Labels";
    case SettingsCategory::InventatoryScan: return "Inventatory Scan";
    case SettingsCategory::DigiKey: return "DigiKey";
  }
  return {};
}

void App::openSettings(SettingsCategory category) {
  settingsCategory_ = category;
  settingsDraft_ = settings_;
  settingsDraft_.printerQueue = printerService_.configuredPrinter();
  settingsDirty_ = false;
  settingsEditingField_ = false;
  appearancePickerOpen_ = false;
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  hasStoredDigiKeySecret_ = CredentialStore::read(kDigiKeySecretName).has_value();
  inputBuffer_.clear();
  applyUiAppearance(settings_.appearance);
  changePage(Page::Settings);
  if (category == SettingsCategory::Printer) refreshPrinterState();
}

void App::beginSettingsEdit() {
  settingsDraft_ = settings_;
  settingsDirty_ = false;
}

bool App::settingsDraftHasChanges() const {
  return settingsDraft_.dataDirectory != settings_.dataDirectory ||
         settingsDraft_.printerQueue != settings_.printerQueue ||
         settingsDraft_.autoPrintScannedLabels != settings_.autoPrintScannedLabels ||
         settingsDraft_.backgroundServiceEnabled != settings_.backgroundServiceEnabled ||
         settingsDraft_.backgroundConsentAsked != settings_.backgroundConsentAsked ||
         settingsDraft_.updateChecksEnabled != settings_.updateChecksEnabled ||
         settingsDraft_.deviceServicePort != settings_.deviceServicePort ||
         settingsDraft_.digiKeyClientId != settings_.digiKeyClientId ||
         settingsDraft_.digiKeyAccountId != settings_.digiKeyAccountId ||
         settingsDraft_.digiKeySite != settings_.digiKeySite ||
         settingsDraft_.digiKeyLanguage != settings_.digiKeyLanguage ||
         settingsDraft_.digiKeyCurrency != settings_.digiKeyCurrency ||
         settingsDraft_.lowStockThreshold != settings_.lowStockThreshold ||
         settingsDraft_.quickLabelPresets != settings_.quickLabelPresets ||
         settingsDraft_.appearance.colors != settings_.appearance.colors ||
         stagedDigiKeySecretChanged_;
}

bool App::stageInventatoryFolder() {
  filesystem::path selected;
  if (!openFolderDialog(selected, "Select Inventatory data folder")) {
    setMessage("Data folder selection cancelled", 2);
    return false;
  }
  if (selected.extension() == ".db") selected = selected.parent_path();
  settingsDraft_.dataDirectory = selected;
  settingsDirty_ = settingsDraft_.dataDirectory != settings_.dataDirectory;
  setMessage("Data folder staged; save settings to switch", 3);
  return true;
}

bool App::testStagedPrinter() {
  if (printerQueues_.empty() || printerSelection_ >= printerQueues_.size()) {
    setMessage("Select a detected printer first", 3);
    return false;
  }
  settingsDraft_.printerQueue = printerQueues_[printerSelection_].name;
  settingsDirty_ = settingsDraft_.printerQueue != settings_.printerQueue || settingsDirty_;
  printerCheck_ = {false, "Testing printer queue..."};
  if (!enqueuePrinterProbe(settingsDraft_.printerQueue)) {
    setMessage("Printer request queue is full; try again shortly", 4);
    return false;
  }
  setMessage("Testing printer queue...", 4);
  return false;  // The result is published by the UI tick.
}

bool App::testStagedDigiKey() {
  DigiKeyConfig config;
  config.clientId = settingsDraft_.digiKeyClientId;
  config.accountId = settingsDraft_.digiKeyAccountId;
  config.site = settingsDraft_.digiKeySite;
  config.language = settingsDraft_.digiKeyLanguage;
  config.currency = settingsDraft_.digiKeyCurrency;
  if (stagedDigiKeySecretChanged_) {
    config.clientSecret = stagedDigiKeySecret_;
  } else if (const auto secret = CredentialStore::read(kDigiKeySecretName); secret.has_value()) {
    config.clientSecret = *secret;
  }
  if (!config.valid()) {
    setMessage("Client ID and client secret are required", 4);
    return false;
  }
  string error;
  DigiKeyApiClient client(move(config));
  const bool ok = client.testConnection(&error);
  setMessage(ok ? "DigiKey credentials are valid" : "DigiKey test failed: " + error, 5);
  return ok;
}

void App::beginSettingsFieldEdit(int field) {
  if (settingsCategory_ == SettingsCategory::Appearance && appearancePickerOpen_) {
    closeAppearancePicker(true);
  }
  settingsField_ = field;
  settingsEditingField_ = true;
  switch (settingsCategory_) {
    case SettingsCategory::General:
      inputBuffer_ = field == 0 ? to_string(settingsDraft_.lowStockThreshold) : string();
      break;
    case SettingsCategory::Updates:
      inputBuffer_.clear();
      break;
    case SettingsCategory::Appearance:
      if (field >= 0 && field < static_cast<int>(kAppearanceColorCount)) {
        inputBuffer_ = appearanceColorHex(settingsDraft_.appearance.colors[static_cast<size_t>(field)]);
      } else {
        inputBuffer_.clear();
      }
      break;
    case SettingsCategory::Printer:
      inputBuffer_ = field == 50 ? wireLabelText_ : string();
      break;
    case SettingsCategory::QuickLabels:
      inputBuffer_ = field >= 0 && field < static_cast<int>(settingsDraft_.quickLabelPresets.size())
                         ? settingsDraft_.quickLabelPresets[field]
                         : string();
      break;
    case SettingsCategory::InventatoryScan:
      inputBuffer_ = field == 0 ? to_string(settingsDraft_.deviceServicePort) : string();
      break;
    case SettingsCategory::DigiKey:
      switch (field) {
        case 0: inputBuffer_ = settingsDraft_.digiKeyClientId; break;
        case 1: inputBuffer_.clear(); break;
        case 2: inputBuffer_ = settingsDraft_.digiKeyAccountId; break;
        case 3: inputBuffer_ = settingsDraft_.digiKeySite; break;
        case 4: inputBuffer_ = settingsDraft_.digiKeyLanguage; break;
        case 5: inputBuffer_ = settingsDraft_.digiKeyCurrency; break;
        default: inputBuffer_.clear(); break;
      }
      break;
  }
  dirty_ = true;
}

void App::commitSettingsFieldEdit() {
  if (!settingsEditingField_) return;
  if (settingsCategory_ == SettingsCategory::Printer && settingsField_ == 50) {
    wireLabelText_ = trim(inputBuffer_);
  } else if (settingsCategory_ == SettingsCategory::General) {
    if (settingsField_ == 0) {
      int threshold = 0;
      if (!parseIntegerInRange(trim(inputBuffer_), 1, (numeric_limits<int>::max)(), threshold)) {
        setMessage("Low-stock threshold must be a positive whole number", 4);
        return;
      }
      settingsDraft_.lowStockThreshold = threshold;
    }
  } else if (settingsCategory_ == SettingsCategory::Appearance) {
    if (settingsField_ < 0 || settingsField_ >= static_cast<int>(kAppearanceColorCount)) return;
    uint32_t parsed = 0;
    if (!parseAppearanceColorHex(inputBuffer_, parsed)) {
      setMessage("Use a six-digit color such as #58B9B0", 4);
      return;
    }
    settingsDraft_.appearance.colors[static_cast<size_t>(settingsField_)] = parsed;
    applyUiAppearance(settingsDraft_.appearance);
    settingsDirty_ = settingsDraftHasChanges();
  } else if (settingsCategory_ == SettingsCategory::QuickLabels) {
    const auto preset = trim(inputBuffer_);
    if (preset.empty() || preset.size() > kQuickLabelPresetTextLimit) {
      setMessage("Quick labels must contain 1 to 24 characters", 4);
      return;
    }
    if (settingsField_ >= 0 && settingsField_ < static_cast<int>(settingsDraft_.quickLabelPresets.size())) {
      settingsDraft_.quickLabelPresets[settingsField_] = preset;
    }
  } else if (settingsCategory_ == SettingsCategory::InventatoryScan) {
    if (settingsField_ == 0) {
      int port = 0;
      if (!parseIntegerInRange(trim(inputBuffer_), 1, 65535, port)) {
        setMessage("Device service port must be between 1 and 65535", 4);
        return;
      }
      settingsDraft_.deviceServicePort = static_cast<uint16_t>(port);
    }
  } else if (settingsCategory_ == SettingsCategory::DigiKey) {
    switch (settingsField_) {
      case 0: settingsDraft_.digiKeyClientId = trim(inputBuffer_); break;
      case 1:
        stagedDigiKeySecret_ = inputBuffer_;
        stagedDigiKeySecretChanged_ = true;
        break;
      case 2: settingsDraft_.digiKeyAccountId = trim(inputBuffer_); break;
      case 3: settingsDraft_.digiKeySite = trim(inputBuffer_); break;
      case 4: settingsDraft_.digiKeyLanguage = trim(inputBuffer_); break;
      case 5: settingsDraft_.digiKeyCurrency = trim(inputBuffer_); break;
      default: break;
    }
  }
  settingsEditingField_ = false;
  settingsDirty_ = true;
  inputBuffer_.clear();
  dirty_ = true;
}

void App::resetSelectedAppearanceColor() {
  if (settingsCategory_ != SettingsCategory::Appearance || settingsField_ < 0 ||
      settingsField_ >= static_cast<int>(kAppearanceColorCount)) {
    return;
  }
  settingsDraft_.appearance.colors[static_cast<size_t>(settingsField_)] =
      AppearanceSettings{}.colors[static_cast<size_t>(settingsField_)];
  appearancePickerOpen_ = false;
  applyUiAppearance(settingsDraft_.appearance);
  settingsDirty_ = settingsDraftHasChanges();
  dirty_ = true;
  setMessage("Selected color reset; save settings to keep it", 3);
}

void App::resetAppearanceColors() {
  if (settingsCategory_ != SettingsCategory::Appearance) return;
  settingsDraft_.appearance = AppearanceSettings{};
  appearancePickerOpen_ = false;
  applyUiAppearance(settingsDraft_.appearance);
  settingsDirty_ = settingsDraftHasChanges();
  dirty_ = true;
  setMessage("Appearance reset to defaults; save settings to keep it", 3);
}

void App::openAppearancePicker() {
  if (settingsCategory_ != SettingsCategory::Appearance || settingsField_ < 0 ||
      settingsField_ >= static_cast<int>(kAppearanceColorCount)) {
    return;
  }
  const auto color = settingsDraft_.appearance.colors[static_cast<size_t>(settingsField_)];
  const auto coordinates = pickerCoordinates(color);
  appearancePickerHue_ = coordinates.hue;
  appearancePickerValue_ = coordinates.value;
  appearancePickerOriginalColor_ = color;
  appearancePickerOpen_ = true;
  settingsEditingField_ = false;
  inputBuffer_.clear();
  dirty_ = true;
}

void App::moveAppearancePicker(int hueDelta, int valueDelta) {
  if (!appearancePickerOpen_) return;
  appearancePickerHue_ = (appearancePickerHue_ + hueDelta + kAppearancePickerHueSteps) %
                          kAppearancePickerHueSteps;
  appearancePickerValue_ = clamp(appearancePickerValue_ + valueDelta, 0, kAppearancePickerValueSteps - 1);
  applyAppearancePickerColor();
}

void App::applyAppearancePickerColor() {
  if (!appearancePickerOpen_ || settingsField_ < 0 ||
      settingsField_ >= static_cast<int>(kAppearanceColorCount)) {
    return;
  }
  settingsDraft_.appearance.colors[static_cast<size_t>(settingsField_)] =
      pickerColor(appearancePickerHue_, appearancePickerValue_);
  applyUiAppearance(settingsDraft_.appearance);
  settingsDirty_ = settingsDraft_.appearance.colors != settings_.appearance.colors;
  dirty_ = true;
}

void App::closeAppearancePicker(bool accept) {
  if (!appearancePickerOpen_) return;
  if (!accept && settingsField_ >= 0 && settingsField_ < static_cast<int>(kAppearanceColorCount)) {
    settingsDraft_.appearance.colors[static_cast<size_t>(settingsField_)] = appearancePickerOriginalColor_;
    applyUiAppearance(settingsDraft_.appearance);
    settingsDirty_ = settingsDraftHasChanges();
  }
  appearancePickerOpen_ = false;
  dirty_ = true;
}

bool App::saveSettingsDraft() {
  if (settingsDraft_.dataDirectory.empty()) {
    setMessage("Choose a valid Inventatory data directory", 4);
    return false;
  }
  error_code error;
  filesystem::create_directories(settingsDraft_.dataDirectory, error);
  if (error) {
    setMessage("Unable to create data directory: " + error.message(), 5);
    return false;
  }

  const bool dataChanged = settingsDraft_.dataDirectory != dataPath_;
  const bool portChanged = settingsDraft_.deviceServicePort != settings_.deviceServicePort;
  const bool backgroundChanged = settingsDraft_.backgroundServiceEnabled != settings_.backgroundServiceEnabled;
  const bool quickLabelsChanged = settingsDraft_.quickLabelPresets != settings_.quickLabelPresets;
  if (quickLabelsChanged) {
    settingsDraft_.quickLabelRevision = settings_.quickLabelRevision == UINT32_MAX
                                            ? 1U
                                            : max(1U, settings_.quickLabelRevision + 1U);
  }

  // Fully read the candidate before stopping the active service or changing
  // the persisted settings pointer.  A missing database is a valid empty
  // workspace; an existing database and its sidecars must be readable.
  InventoryStore candidateStore;
  AppSettings oldSettings = settings_;
  const auto oldPaths = makeInventatoryDataPaths(dataPath_);
  const auto oldContext = currentWorkspaceContext();
  vector<ActivityEntry> candidateActivities;
  vector<string> candidateQuickLabels;
  uint32_t candidateQuickLabelRevision = settingsDraft_.quickLabelRevision;
  bool candidateQuickLabelsLoaded = false;
  auto candidatePaths = makeInventatoryDataPaths(settingsDraft_.dataDirectory);
  if (dataChanged) {
    if (filesystem::exists(candidatePaths.inventory, error)) {
      SqliteConnection candidateConnection;
      string candidateValidationError;
      if (!openDatabaseReadOnly(candidatePaths.inventory, candidateConnection)) {
        setMessage("The selected folder contains an inventory database Inventatory cannot load", 5);
        return false;
      }
      if (!validateInventoryDatabase(candidateConnection, &candidateValidationError)) {
        setMessage("The selected folder contains an unsupported or invalid inventory database", 6);
        return false;
      } else if (!candidateStore.load(candidatePaths.inventory)) {
        setMessage("The selected folder contains an inventory database Inventatory cannot load", 5);
        return false;
      }
    }
    if (filesystem::exists(candidatePaths.activity, error) &&
        !loadActivities(candidatePaths.activity, candidateActivities)) {
      setMessage("The selected folder contains unreadable activity history", 5);
      return false;
    }
    candidateQuickLabels.clear();
    if (filesystem::exists(quickLabelsPath(settingsDraft_.dataDirectory), error) &&
        !loadQuickLabels(quickLabelsPath(settingsDraft_.dataDirectory), candidateQuickLabels,
                         candidateQuickLabelRevision)) {
      setMessage("The selected folder contains unreadable Quick Labels settings", 5);
      return false;
    }
    candidateQuickLabelsLoaded = filesystem::exists(quickLabelsPath(settingsDraft_.dataDirectory), error);
    InventatoryScanConfig candidateScan;
    if (filesystem::exists(candidatePaths.scanConfig, error) &&
        !loadInventatoryScanConfig(candidatePaths.scanConfig, candidateScan)) {
      setMessage("The selected folder contains unreadable scanner settings", 5);
      return false;
    }
    if (!quickLabelsChanged && candidateQuickLabelsLoaded) {
      settingsDraft_.quickLabelPresets = candidateQuickLabels;
      settingsDraft_.quickLabelRevision = candidateQuickLabelRevision;
    }
  }

  // Do not allow a switch while a user-visible sync is mid-flight.  Other
  // workspace-bound lookups are quiesced below; their results are never
  // allowed to cross the generation boundary.
  if (dataChanged && importSyncRunning_) {
    setMessage("Wait for DigiKey sync to finish before changing the data folder", 5);
    return false;
  }

  const bool serviceWasRunning = server_.running();
  const auto restartOldService = [&] {
    if (serviceWasRunning) restartDeviceService();
  };
  const auto oldDigiKeySecret = CredentialStore::read(kDigiKeySecretName);
  bool startupChanged = false;
  bool credentialChanged = false;

  if (dataChanged) {
    mdnsService_.stop();
    server_.stop();
    stopWorkspaceBoundWork();
    if (!saveState()) {
      restartOldService();
      setMessage(persistenceError_.empty() ? "Unable to save the current Inventatory data" : persistenceError_, 5);
      return false;
    }
  }
  if (stagedDigiKeySecretChanged_ && !CredentialStore::write(kDigiKeySecretName, stagedDigiKeySecret_)) {
    restartOldService();
    setMessage("Unable to save the DigiKey secret securely", 5);
    return false;
  }
  credentialChanged = stagedDigiKeySecretChanged_;
  if (backgroundChanged) {
    string startupError;
    if (!setBackgroundStartupEnabled(settingsDraft_.backgroundServiceEnabled, startupError)) {
      if (credentialChanged) {
        if (oldDigiKeySecret.has_value()) CredentialStore::write(kDigiKeySecretName, *oldDigiKeySecret);
        else CredentialStore::erase(kDigiKeySecretName);
      }
      restartOldService();
      setMessage("Unable to update Windows startup: " + startupError, 5);
      return false;
    }
    startupChanged = true;
    settingsDraft_.backgroundConsentAsked = true;
  }
  if (!saveAppSettings(settingsPath_, settingsDraft_)) {
    if (startupChanged) {
      string ignored;
      setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, ignored);
    }
    if (credentialChanged) {
      if (oldDigiKeySecret.has_value()) CredentialStore::write(kDigiKeySecretName, *oldDigiKeySecret);
      else CredentialStore::erase(kDigiKeySecretName);
    }
    restartOldService();
    setMessage("Unable to save Inventatory settings", 5);
    return false;
  }
  appSettingsSavePending_ = false;

  if (dataChanged && quickLabelsChanged &&
      !saveQuickLabels(quickLabelsPath(settingsDraft_.dataDirectory), settingsDraft_.quickLabelPresets,
                       settingsDraft_.quickLabelRevision)) {
    const bool rollbackSaved = saveAppSettings(settingsPath_, oldSettings);
    settings_ = oldSettings;
    settingsDraft_ = oldSettings;
    if (startupChanged) {
      string ignored;
      setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, ignored);
    }
    if (credentialChanged) {
      if (oldDigiKeySecret.has_value()) CredentialStore::write(kDigiKeySecretName, *oldDigiKeySecret);
      else CredentialStore::erase(kDigiKeySecretName);
    }
    restartOldService();
    setMessage(rollbackSaved ? "Unable to save Quick Labels in the selected data folder"
                             : "Unable to save Quick Labels and restore previous settings; verify the settings file",
               7);
    return false;
  }

  if (dataChanged) {
    settings_ = settingsDraft_;
    settings_.dataDirectory = candidatePaths.dataDirectory;
    settingsDraft_ = settings_;
    inventatoryScanConfig_ = {};
    activateWorkspaceContext(candidatePaths);
    loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
    if (!quickLabelsChanged) {
      settingsDraft_.quickLabelPresets.clear();
      settingsDraft_.quickLabelRevision = 1;
      loadQuickLabels(quickLabelsPath_, settingsDraft_.quickLabelPresets, settingsDraft_.quickLabelRevision);
    }
    settings_.quickLabelPresets = settingsDraft_.quickLabelPresets;
    settings_.quickLabelRevision = settingsDraft_.quickLabelRevision;
    settingsDraft_ = settings_;
    loadState();
    if (inventoryRecoveryRequired_) {
      settings_ = oldSettings;
      settingsDraft_ = oldSettings;
      const bool rollbackSaved = saveAppSettings(settingsPath_, oldSettings);
      if (oldContext != nullptr) {
        activateWorkspaceContext(oldContext->paths);
      } else {
        activateWorkspaceContext(oldPaths);
      }
      inventatoryScanConfig_ = {};
      loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
      loadState();
      restartOldService();
      setMessage(rollbackSaved
                     ? "The selected workspace could not be activated; the previous workspace was restored"
                     : "The selected workspace failed and previous settings could not be restored; verify settings.conf",
                 7);
      return false;
    }
    server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                 inventatoryScanReplayStatePath(dataPath_));
  }

  if (quickLabelsChanged && !dataChanged &&
      !saveQuickLabels(quickLabelsPath_, settingsDraft_.quickLabelPresets, settingsDraft_.quickLabelRevision)) {
    const bool rollbackSaved = saveAppSettings(settingsPath_, oldSettings);
    settings_ = oldSettings;
    settingsDraft_ = oldSettings;
    if (startupChanged) {
      string ignored;
      setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, ignored);
    }
    if (credentialChanged) {
      if (oldDigiKeySecret.has_value()) CredentialStore::write(kDigiKeySecretName, *oldDigiKeySecret);
      else CredentialStore::erase(kDigiKeySecretName);
    }
    restartOldService();
    setMessage(rollbackSaved
                   ? "Unable to save quick-label settings"
                   : "Unable to save quick-label settings; previous settings could not be restored",
               7);
    return false;
  }

  {
    lock_guard<mutex> lock(quickLabelMutex_);
    settings_ = settingsDraft_;
  }
  applyUiAppearance(settings_.appearance);
  if (stagedDigiKeySecretChanged_) hasStoredDigiKeySecret_ = !stagedDigiKeySecret_.empty();
  autoPrintScannedLabels_ = settings_.autoPrintScannedLabels;
  bool bridgeRestarted = true;
  if (portChanged) {
    restartDeviceService();
    bridgeRestarted = server_.running();
  } else if (dataChanged) {
    restartDeviceService();
    bridgeRestarted = server_.running();
  }
  if (backgroundChanged) {
    if (settings_.backgroundServiceEnabled) {
      const bool backgroundStarted = backgroundController_.start(true, false, [this] {
        backgroundQuitRequested_.store(true);
      });
      if (!backgroundStarted) {
        // Keep the setting truthful if the tray/controller could not be
        // created.  A later launch should not repeatedly claim an available
        // background service that never started.
        settings_.backgroundServiceEnabled = false;
        settingsDraft_.backgroundServiceEnabled = false;
        string startupError;
        setBackgroundStartupEnabled(false, startupError);
        if (!saveAppSettings(settingsPath_, settings_)) {
          appSettingsSavePending_ = true;
          persistenceError_ = "Background service could not start and its disabled state could not be saved.";
          setMessage(persistenceError_ + " Press R to retry.", 7);
          settingsDirty_ = true;
          return false;
        }
        setMessage("Settings saved, but the background service could not start; it was disabled", 7);
        settingsDirty_ = false;
        return false;
      }
    } else {
      backgroundController_.stop();
    }
  }
  if (!settings_.printerQueue.empty()) {
    printerService_.setConfiguredPrinter(settings_.printerQueue);
    if (!printerService_.saveConfig(printerPath_)) {
      persistenceError_ = "Could not save printer settings; changes remain in memory.";
      setMessage(persistenceError_ + " Press R to retry.", 6);
      settingsDirty_ = true;
      return false;
    }
    printerCheck_ = {false, "Checking printer queue..."};
    if (!enqueuePrinterProbe(settings_.printerQueue)) {
      setMessage("Settings saved, but the printer check could not be queued", 5);
    }
  }
  settingsDirty_ = false;
  appearancePickerOpen_ = false;
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  setMessage((portChanged || dataChanged) ? (bridgeRestarted ? "Settings saved; device bridge restarted"
                                                              : "Settings saved, but the device bridge could not restart")
                         : "Settings saved",
             4);
  return true;
}

void App::cancelSettingsDraft() {
  settingsDraft_ = settings_;
  applyUiAppearance(settings_.appearance);
  settingsDirty_ = false;
  settingsEditingField_ = false;
  appearancePickerOpen_ = false;
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  inputBuffer_.clear();
  setMessage("Settings changes discarded", 3);
}

}  // namespace inventatory
