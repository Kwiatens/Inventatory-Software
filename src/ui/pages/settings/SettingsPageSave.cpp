// Inventatory - settings validation and persistence workflow.

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "platform/security/CredentialStore.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/system/StartupRegistration.h"
#include "core/storage/InventorySqlite.h"
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

}  // namespace inventatory
