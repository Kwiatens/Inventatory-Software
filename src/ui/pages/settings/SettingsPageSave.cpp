// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - settings validation and persistence workflow.

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "platform/security/CredentialStore.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/mouser/MouserApi.h"
#include "platform/system/StartupRegistration.h"
#include "core/storage/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <cstdint>
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

  // Read the candidate fully before stopping the service or changing settings. A missing
  // database is an empty workspace; an existing one must load.
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

  // No workspace switch during a visible sync. Other workspace lookups stop below, and their
  // results must not cross the generation boundary.
  if (dataChanged && importSyncRunning_) {
    setMessage("Wait for DigiKey sync to finish before changing the data folder", 5);
    return false;
  }

  const bool serviceWasRunning = server_.running();
  const auto restartOldService = [&] {
    if (serviceWasRunning) restartDeviceService();
  };
  const auto oldDigiKeySecret = CredentialStore::lookup(kDigiKeySecretName);
  const auto oldTmeToken = CredentialStore::lookup(kTmeTokenCredentialName);
  const auto oldTmeSecret = CredentialStore::lookup(kTmeSecretCredentialName);
  const auto oldMouserKey = CredentialStore::lookup(kMouserApiKeyCredentialName);
  bool startupChanged = false;
  bool credentialChanged = false;
  bool tmeTokenChanged = false;
  bool tmeSecretChanged = false;
  bool mouserKeyChanged = false;
  // Restores one credential after a later step failed. An unreadable previous state erases nothing.
  const auto restoreCredential = [](const char* name, const CredentialLookup& previous) {
    if (previous.status == CredentialReadStatus::Found && previous.secret.has_value()) {
      CredentialStore::write(name, *previous.secret);
    } else if (previous.status == CredentialReadStatus::NotFound) {
      CredentialStore::erase(name);
    }
  };
  const auto rollbackCredentials = [&] {
    if (credentialChanged) restoreCredential(kDigiKeySecretName, oldDigiKeySecret);
    if (tmeTokenChanged) restoreCredential(kTmeTokenCredentialName, oldTmeToken);
    if (tmeSecretChanged) restoreCredential(kTmeSecretCredentialName, oldTmeSecret);
    if (mouserKeyChanged) restoreCredential(kMouserApiKeyCredentialName, oldMouserKey);
  };

  if (dataChanged) {
    mdnsService_.stop();
    server_.stop();
    stopWorkspaceBoundWork();
    if (!saveState()) {
      restartOldService();
      setMessage(persistenceError_.empty() ? "Unable to save the current Inventatory data" : persistenceError_, 5,
                 UiMessageSeverity::Error);
      return false;
    }
  }
  if (stagedDigiKeySecretChanged_ && !CredentialStore::write(kDigiKeySecretName, stagedDigiKeySecret_)) {
    restartOldService();
    setMessage("Unable to save the DigiKey secret securely", 5, UiMessageSeverity::Error);
    return false;
  }
  credentialChanged = stagedDigiKeySecretChanged_;
  if (stagedTmeTokenChanged_) {
    if (!CredentialStore::write(kTmeTokenCredentialName, stagedTmeToken_)) {
      rollbackCredentials();
      restartOldService();
      setMessage("Unable to save the TME token securely", 5, UiMessageSeverity::Error);
      return false;
    }
    tmeTokenChanged = true;
  }
  if (stagedTmeSecretChanged_) {
    if (!CredentialStore::write(kTmeSecretCredentialName, stagedTmeSecret_)) {
      rollbackCredentials();
      restartOldService();
      setMessage("Unable to save the TME application secret securely", 5, UiMessageSeverity::Error);
      return false;
    }
    tmeSecretChanged = true;
  }
  if (stagedMouserKeyChanged_) {
    // An empty staged key means the key was cleared, so the stored one is erased.
    const bool stored = stagedMouserKey_.empty() ? CredentialStore::erase(kMouserApiKeyCredentialName)
                                                 : CredentialStore::write(kMouserApiKeyCredentialName, stagedMouserKey_);
    if (!stored) {
      rollbackCredentials();
      restartOldService();
      setMessage("Unable to save the Mouser API key securely", 5, UiMessageSeverity::Error);
      return false;
    }
    mouserKeyChanged = true;
  }
  if (backgroundChanged) {
    string startupError;
    if (!setBackgroundStartupEnabled(settingsDraft_.backgroundServiceEnabled, startupError)) {
      rollbackCredentials();
      restartOldService();
#ifdef _WIN32
      setMessage(startupError.empty() ? "Unable to update Windows startup" : startupError, 5, UiMessageSeverity::Error);
#else
      setMessage(startupError.empty() ? "Unable to update background service" : startupError, 5, UiMessageSeverity::Error);
#endif
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
    rollbackCredentials();
    restartOldService();
    setMessage("Unable to save Inventatory settings", 5, UiMessageSeverity::Error);
    return false;
  }
  appSettingsSavePending_ = false;

  if (dataChanged && quickLabelsChanged &&
      !saveQuickLabels(quickLabelsPath(settingsDraft_.dataDirectory), settingsDraft_.quickLabelPresets,
                       settingsDraft_.quickLabelRevision)) {
    const bool rollbackSaved = saveAppSettings(settingsPath_, oldSettings);
    {
      lock_guard<mutex> lock(quickLabelMutex_);
      settings_ = oldSettings;
    }
    // Keep the draft so the user can retry or pick another folder without re-entering it.
    settingsDirty_ = true;
    if (startupChanged) {
      string ignored;
      setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, ignored);
    }
    rollbackCredentials();
    restartOldService();
    setMessage(rollbackSaved ? "Unable to save Quick Labels in the selected data folder"
                             : "Unable to save Quick Labels and restore previous settings; verify the settings file",
               7, UiMessageSeverity::Error);
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
      // Also undo the credential and startup changes made earlier in this save.
      if (startupChanged) {
        string ignored;
        setBackgroundStartupEnabled(oldSettings.backgroundServiceEnabled, ignored);
      }
      rollbackCredentials();
      stagedDigiKeySecret_.assign(stagedDigiKeySecret_.size(), '\0');
      stagedDigiKeySecret_.clear();
      stagedDigiKeySecretChanged_ = false;
      clearStagedTmeCredentials();
      clearStagedMouserCredentials();
      settingsDirty_ = false;
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
                 7, UiMessageSeverity::Error);
      return false;
    }
    server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                 inventatoryScanReplayStatePath(dataPath_));
  }

  if (quickLabelsChanged && !dataChanged &&
      !saveQuickLabels(quickLabelsPath_, settingsDraft_.quickLabelPresets, settingsDraft_.quickLabelRevision)) {
    const bool rollbackSaved = saveAppSettings(settingsPath_, oldSettings);
    {
      // The bridge is still serving here: its workers read the quick-label presets under this lock.
      lock_guard<mutex> lock(quickLabelMutex_);
      settings_ = oldSettings;
    }
    // Keep the draft so the user can retry.
    settingsDirty_ = true;
    if (startupChanged) {
      string ignored;
      setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, ignored);
    }
    rollbackCredentials();
    restartOldService();
    setMessage(rollbackSaved
                   ? "Unable to save quick-label settings"
                   : "Unable to save quick-label settings; previous settings could not be restored",
               7, UiMessageSeverity::Error);
    return false;
  }

  {
    lock_guard<mutex> lock(quickLabelMutex_);
    settings_ = settingsDraft_;
  }
  applyUiAppearance(settings_.appearance);
  if (stagedDigiKeySecretChanged_) hasStoredDigiKeySecret_ = !stagedDigiKeySecret_.empty();
  if (stagedTmeTokenChanged_) hasStoredTmeToken_ = !stagedTmeToken_.empty();
  if (stagedTmeSecretChanged_) hasStoredTmeSecret_ = !stagedTmeSecret_.empty();
  if (stagedMouserKeyChanged_) hasStoredMouserKey_ = !stagedMouserKey_.empty();
  autoPrintScannedLabels_ = settings_.autoPrintScannedLabels;
  auto bridgeRestart = DeviceServiceRestart::Restarted;
  if (portChanged || dataChanged) bridgeRestart = restartDeviceService();
  // Report a failed background start after the rest of the save, so nothing is left half applied.
  bool backgroundStartFailed = false;
  bool backgroundDisabledSaved = true;
  if (backgroundChanged) {
    if (settings_.backgroundServiceEnabled) {
      const bool backgroundStarted = backgroundController_.start(true, false, [this] {
        backgroundQuitRequested_.store(true);
      });
      if (!backgroundStarted) {
        // Store the setting as off, so a later launch does not claim a service that never started.
        settings_.backgroundServiceEnabled = false;
        settingsDraft_.backgroundServiceEnabled = false;
        string startupError;
        setBackgroundStartupEnabled(false, startupError);
        backgroundStartFailed = true;
        backgroundDisabledSaved = saveAppSettings(settingsPath_, settings_);
        if (!backgroundDisabledSaved) appSettingsSavePending_ = true;
      }
    } else {
      backgroundController_.disableBackgroundMode();
    }
  }
  if (!settings_.printerQueue.empty()) {
    printerService_.setConfiguredPrinter(settings_.printerQueue);
    publishConfiguredPrinter();
    if (!printerService_.saveConfig(printerPath_)) {
      persistenceError_ = "Could not save printer settings; changes remain in memory.";
      setMessage(persistenceError_ + " Press R to retry.", 6, UiMessageSeverity::Error);
      settingsDirty_ = true;
      return false;
    }
    printerCheck_ = {false, "Checking printer queue..."};
    if (!enqueuePrinterProbe(settings_.printerQueue)) {
      setMessage("Settings saved, but the printer check could not be queued", 5, UiMessageSeverity::Warning);
    }
  }
  settingsDirty_ = false;
  appearancePickerOpen_ = false;
  stagedDigiKeySecret_.assign(stagedDigiKeySecret_.size(), '\0');
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  clearStagedTmeCredentials();
  clearStagedMouserCredentials();
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  if (backgroundStartFailed) {
    if (!backgroundDisabledSaved) {
      persistenceError_ = "Background service could not start and its disabled state could not be saved.";
      setMessage(persistenceError_ + " Press R to retry.", 7, UiMessageSeverity::Error);
      settingsDirty_ = true;
      return false;
    }
    setMessage("Settings saved, but the background service could not start; it was disabled", 7,
               UiMessageSeverity::Warning);
    return false;
  }
  if (portChanged || dataChanged) {
    const auto notice = settingsBridgeNotice(bridgeRestart);
    setMessage(notice.text, 4, notice.severity);
  } else {
    setMessage("Settings saved", 4, UiMessageSeverity::Success);
  }
  return true;
}

}  // namespace inventatory
