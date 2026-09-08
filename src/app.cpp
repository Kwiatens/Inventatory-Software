// Inventatory - Application construction and initial workspace loading.

#include "App.h"

#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/StartupRegistration.h"
#include "platform/system/UpdateService.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <thread>
#include <future>

namespace inventatory {

using namespace std;

App::App(bool startInBackground, BackgroundController& backgroundController)
    : backgroundController_(backgroundController),
      startInBackground_(startInBackground),
      root_(filesystem::current_path()),
      settingsPath_(appSettingsPath()),
      dataPath_(discoverInventatoryDataPath()),
      inventoryPath_(dataPath_ / "inventory.db"),
      printerPath_(dataPath_ / "printer.conf"),
      activityPath_(dataPath_ / "activity.tsv"),
      inventatoryScanConfigPath_(dataPath_ / "inventatory_scan.conf"),
      quickLabelsPath_(dataPath_ / "quick_labels.conf") {
  error_code settingsFileError;
  const bool settingsFileExists = filesystem::exists(settingsPath_, settingsFileError);
  const bool loadedSettings = loadAppSettings(settingsPath_, settings_);
  applyUiAppearance(settings_.appearance);
  if (loadedSettings && !settings_.dataDirectory.empty()) {
    dataPath_ = settings_.dataDirectory;
    inventoryPath_ = dataPath_ / "inventory.db";
    printerPath_ = dataPath_ / "printer.conf";
    activityPath_ = dataPath_ / "activity.tsv";
    inventatoryScanConfigPath_ = dataPath_ / "inventatory_scan.conf";
    quickLabelsPath_ = dataPath_ / "quick_labels.conf";
  } else {
    settings_.dataDirectory = dataPath_;
  }
  string restoreRecoveryNotice;
  if (!recoverInventatoryRestore(dataPath_, settingsPath_, restoreRecoveryNotice)) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = restoreRecoveryNotice.empty()
                                   ? "An interrupted restore could not be recovered safely"
                                   : restoreRecoveryNotice;
  }
  activateWorkspaceContext(makeInventatoryDataPaths(dataPath_));
  error_code quickLabelsError;
  const bool quickLabelsFileExists = filesystem::exists(quickLabelsPath_, quickLabelsError);
  if (quickLabelsError ||
      (quickLabelsFileExists &&
       !loadQuickLabels(quickLabelsPath_, settings_.quickLabelPresets, settings_.quickLabelRevision))) {
    inventoryRecoveryRequired_ = true;
    inventoryRecoveryDetail_ = "Inventatory could not read Quick Labels settings: " + quickLabelsPath_.string() +
                               ". The original file was preserved.";
    persistenceError_ = inventoryRecoveryDetail_;
  }
  settingsDraft_ = settings_;
  autoPrintScannedLabels_ = settings_.autoPrintScannedLabels;
  hasStoredDigiKeySecret_ = CredentialStore::read("digikey-client-secret").has_value();
  loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
  if (!inventoryRecoveryRequired_) {
    loadState();
    if (!restoreRecoveryNotice.empty()) setMessage(restoreRecoveryNotice, 8);
  }
  if (!loadedSettings) {
    settings_.printerQueue = printerService_.configuredPrinter();
    const auto environment = loadDigiKeyConfig();
    settings_.digiKeyClientId = environment.clientId;
    settings_.digiKeyAccountId = environment.accountId;
    settings_.digiKeySite = environment.site;
    settings_.digiKeyLanguage = environment.language;
    settings_.digiKeyCurrency = environment.currency;
    settingsDraft_ = settings_;
    if (!settingsFileExists) {
      if (!saveAppSettings(settingsPath_, settings_)) {
        appSettingsSavePending_ = true;
        persistenceError_ = "Could not save initial application settings; they remain in memory.";
        setMessage(persistenceError_ + " Press R to retry.", 6);
      }
    } else {
      setMessage("Settings file is invalid; defaults are in use temporarily. Finish setup or reset it explicitly.", 8);
    }
  } else if (!settings_.printerQueue.empty()) {
    printerService_.setConfiguredPrinter(settings_.printerQueue);
    // Queue the potentially slow Windows spooler probe.  Startup must remain
    // responsive even when a disconnected queue takes seconds to answer.
    refreshPrinterState();
  }
  if (onboardingRequired(startInBackground_, loadedSettings, settings_.completedOnboardingVersion)) {
    onboardingActive_ = true;
    page_ = Page::Onboarding;
  }
  const bool anotherInteractiveInstanceRunning =
      startInBackground_ && backgroundController_.interactiveInstanceRunning();
  const bool backgroundServiceAlreadyRunning = !startInBackground_ && backgroundController_.backgroundServiceRunning();
  if (!inventoryRecoveryRequired_ && !anotherInteractiveInstanceRunning && !backgroundServiceAlreadyRunning &&
      inventatoryScanConfig_.setupComplete && !inventatoryScanConfig_.token.empty() &&
      !scannerCredentialSavePending_) {
    server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                 inventatoryScanReplayStatePath(dataPath_));

    if (!server_.start(settings_.deviceServicePort,
                     [this](const DeviceSyncRequest& request, DeviceSyncResponse& response, string& error) {
                       return handleDeviceSync(request, response, error);
                     })) {
      setMessage("Inventatory Scan R1 service failed to start; terminal still works", 5);
    } else {
      if (mdnsService_.start(server_.port())) {
        setMessage("Inventatory Scan R1 service ready", 5);
      } else {
        setMessage("Inventatory Scan R1 service ready; network discovery unavailable", 5);
      }
    }
  } else if (!inventoryRecoveryRequired_ &&
             (!inventatoryScanConfig_.setupComplete || inventatoryScanConfig_.token.empty() ||
              scannerCredentialSavePending_)) {
    setMessage(!inventatoryScanConfig_.setupComplete
                   ? "Scan R1 service is disabled until this workspace is paired"
                   : "Scan R1 service is disabled until its pairing token is stored securely",
               6);
  }
  beginUpdateCheckIfDue();
}

}  // namespace inventatory
