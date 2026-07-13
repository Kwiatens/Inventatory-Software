// HIMS - Hardware Inventory Management System
// Central application settings workspace and staged configuration workflow.

#include "App.h"

#include "platform/CredentialStore.h"
#include "platform/DigiKeyApi.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace hims {

using namespace std;

namespace {

constexpr const char* kDigiKeySecretName = "digikey-client-secret";

ftxui::Element settingLine(const string& label, const string& value, int width, bool selected = false) {
  const int labelWidth = min(22, max(14, width / 3));
  return ftxui::hbox({
             styledText(" " + label, selected ? uiFocusColor() : uiSecondaryText()) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth),
             styledText(ellipsize(value, static_cast<size_t>(max(8, width - labelWidth - 2))),
                        selected ? uiPrimaryText() : uiPrimaryText()),
             ftxui::filler(),
         }) |
         ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
}

ftxui::Element printerQueueLine(const string& name, const string& status, int width, bool selected) {
  const int statusWidth = max(10, min(18, static_cast<int>(status.size()) + 2));
  const int nameWidth = max(12, width - statusWidth - 5);
  return ftxui::hbox({
             styledText(selected ? " > " : "   ", selected ? uiFocusColor() : uiSecondaryText()),
             styledText(ellipsize(name, static_cast<size_t>(nameWidth)), uiPrimaryText()) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, nameWidth),
             ftxui::filler(),
             styledText(status, status == "Ready" ? uiSuccessColor() : uiSecondaryText()),
             ftxui::text(" "),
         }) |
         ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
}

}  // namespace

string App::settingsCategoryName(SettingsCategory category) const {
  switch (category) {
    case SettingsCategory::General: return "General / Data";
    case SettingsCategory::Printer: return "Printer";
    case SettingsCategory::HimsScan: return "HIMS Scan";
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
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  hasStoredDigiKeySecret_ = CredentialStore::read(kDigiKeySecretName).has_value() ||
                            !loadDigiKeyConfig().clientSecret.empty();
  inputBuffer_.clear();
  changePage(Page::Settings);
  if (category == SettingsCategory::Printer) refreshPrinterState();
}

void App::beginSettingsEdit() {
  settingsDraft_ = settings_;
  settingsDirty_ = false;
}

bool App::stageHimsFolder() {
  filesystem::path selected;
  if (!openFolderDialog(selected, "Select HIMS data folder")) {
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
  const auto original = printerService_.configuredPrinter();
  settingsDraft_.printerQueue = printerQueues_[printerSelection_].name;
  printerService_.setConfiguredPrinter(settingsDraft_.printerQueue);
  printerCheck_ = printerService_.probeConfiguredPrinter();
  printerService_.setConfiguredPrinter(original);
  settingsDirty_ = settingsDraft_.printerQueue != settings_.printerQueue || settingsDirty_;
  setMessage(printerCheck_.message.empty() ? (printerCheck_.ok ? "Printer is ready" : "Printer test failed")
                                           : printerCheck_.message,
             4);
  return printerCheck_.ok;
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
  } else {
    config.clientSecret = loadDigiKeyConfig().clientSecret;
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
  if (settingsCategory_ == SettingsCategory::General || settingsCategory_ == SettingsCategory::Printer) return;
  settingsField_ = field;
  settingsEditingField_ = true;
  switch (settingsCategory_) {
    case SettingsCategory::General:
      inputBuffer_.clear();
      break;
    case SettingsCategory::Printer:
      inputBuffer_.clear();
      break;
    case SettingsCategory::HimsScan:
      inputBuffer_ = to_string(settingsDraft_.deviceServicePort);
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
  if (settingsCategory_ == SettingsCategory::HimsScan) {
    try {
      const auto port = stoi(inputBuffer_);
      if (port < 1 || port > 65535) throw out_of_range("port");
      settingsDraft_.deviceServicePort = static_cast<uint16_t>(port);
    } catch (...) {
      setMessage("Device service port must be between 1 and 65535", 4);
      return;
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

bool App::saveSettingsDraft() {
  if (settingsDraft_.dataDirectory.empty()) {
    setMessage("Choose a valid HIMS data directory", 4);
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
  if (dataChanged) {
    const auto candidateDatabase = settingsDraft_.dataDirectory / "inventory.db";
    if (filesystem::exists(candidateDatabase, error)) {
      InventoryStore candidate;
      if (!candidate.load(candidateDatabase)) {
        setMessage("The selected folder contains an inventory database HIMS cannot load", 5);
        return false;
      }
    }
  }
  if (stagedDigiKeySecretChanged_ && !CredentialStore::write(kDigiKeySecretName, stagedDigiKeySecret_)) {
    setMessage("Unable to save the DigiKey secret securely", 5);
    return false;
  }
  if (!saveAppSettings(settingsPath_, settingsDraft_)) {
    setMessage("Unable to save HIMS settings", 5);
    return false;
  }

  if (dataChanged) {
    saveState();
    dataPath_ = settingsDraft_.dataDirectory;
    inventoryPath_ = dataPath_ / "inventory.db";
    printerPath_ = dataPath_ / "printer.conf";
    activityPath_ = dataPath_ / "activity.tsv";
    himsScanConfigPath_ = dataPath_ / "hims_scan.conf";
    ensureInventoryDatabaseCopied(inventoryPath_);
    loadHimsScanConfig(himsScanConfigPath_, himsScanConfig_);
    loadState();
    server_.setDeviceCredentials(himsScanConfig_.deviceId, himsScanConfig_.token);
  }

  settings_ = settingsDraft_;
  if (stagedDigiKeySecretChanged_) hasStoredDigiKeySecret_ = !stagedDigiKeySecret_.empty();
  autoPrintScannedLabels_ = settings_.autoPrintScannedLabels;
  if (!settings_.printerQueue.empty()) {
    printerService_.setConfiguredPrinter(settings_.printerQueue);
    printerCheck_ = printerService_.probeConfiguredPrinter();
    printerService_.saveConfig(printerPath_);
  }
  settingsDirty_ = false;
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  setMessage(portChanged ? "Settings saved; restart HIMS to apply the device service port" : "Settings saved", 4);
  return true;
}

void App::cancelSettingsDraft() {
  settingsDraft_ = settings_;
  settingsDirty_ = false;
  settingsEditingField_ = false;
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  inputBuffer_.clear();
  setMessage("Settings changes discarded", 3);
}

ftxui::Element App::renderSettingsUi() const {
  auto self = const_cast<App*>(this);
  constexpr int categoryWidth = 24;
  const auto* active = ftxui::ScreenInteractive::Active();
  const int contentWidth = max(60, (active != nullptr ? active->dimx() : 120) - categoryWidth - 3);

  ftxui::Elements categories;
  categories.push_back(styledText(" SETTINGS", uiMutedText()));
  for (const auto category : {SettingsCategory::General, SettingsCategory::Printer,
                              SettingsCategory::HimsScan, SettingsCategory::DigiKey}) {
    const bool selected = category == settingsCategory_;
    auto row = fullLine(string(selected ? "  > " : "    ") + settingsCategoryName(category),
                        selected ? uiFocusColor() : uiSecondaryText(),
                        selected ? uiSelectionBg() : uiCanvasBg());
    categories.push_back(target(row, "settings.category." + settingsCategoryName(category), UiTargetKind::Category,
                                [self, category] {
                                  self->settingsCategory_ = category;
                                  self->settingsField_ = 0;
                                  self->settingsEditingField_ = false;
                                  if (category == SettingsCategory::Printer) self->refreshPrinterState();
                                  self->dirty_ = true;
                                }));
  }

  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      styledText(settingsCategoryName(settingsCategory_), uiPrimaryText()) | ftxui::bold,
      ftxui::filler(),
      styledText(settingsDirty_ ? "Unsaved changes" : "Saved", settingsDirty_ ? uiWarnColor() : uiSuccessColor()),
      ftxui::text(" "),
  }));
  rows.push_back(uiDivider());

  if (settingsCategory_ == SettingsCategory::General) {
    rows.push_back(styledText("Data", uiSecondaryText()));
    rows.push_back(settingLine("HIMS folder", settingsDraft_.dataDirectory.string(), contentWidth));
    rows.push_back(target(settingLine("Change folder", "Browse...", contentWidth), "settings.data.browse",
                          UiTargetKind::Button, [self] { self->stageHimsFolder(); }));
    rows.push_back(uiDivider());
    rows.push_back(styledText("Application", uiSecondaryText()));
    rows.push_back(settingLine("Settings file", settingsPath_.string(), contentWidth));
  } else if (settingsCategory_ == SettingsCategory::Printer) {
    rows.push_back(settingLine("Configured queue",
                               settingsDraft_.printerQueue.empty() ? "Not configured" : settingsDraft_.printerQueue,
                               contentWidth));
    rows.push_back(target(settingLine("Auto-label", settingsDraft_.autoPrintScannedLabels ? "On" : "Off", contentWidth),
                          "settings.printer.autolabel", UiTargetKind::Field, [self] {
                            self->settingsDraft_.autoPrintScannedLabels = !self->settingsDraft_.autoPrintScannedLabels;
                            self->settingsDirty_ = true;
                            self->dirty_ = true;
                          }));
    rows.push_back(uiDivider());
    rows.push_back(styledText("Detected printer queues", uiSecondaryText()));
    if (printerQueues_.empty()) {
      rows.push_back(styledText("No printer queues detected", uiWarnColor()));
    } else {
      for (size_t index = 0; index < printerQueues_.size(); ++index) {
        const auto& printer = printerQueues_[index];
        const bool selected = index == printerSelection_;
        rows.push_back(target(printerQueueLine(printer.name, printer.statusText, contentWidth, selected),
                              "settings.printer." + to_string(index), UiTargetKind::Row, [self, index] {
                                self->printerSelection_ = index;
                                self->settingsDraft_.printerQueue = self->printerQueues_[index].name;
                                self->settingsDirty_ = true;
                                self->dirty_ = true;
                              }));
      }
    }
    rows.push_back(ftxui::hbox({
        target(styledText(" Refresh ", uiInteractiveColor(), uiRaisedSurfaceBg()), "settings.printer.refresh",
               UiTargetKind::Button, [self] { self->refreshPrinterState(); }),
        ftxui::text("  "),
        target(styledText(" Test selected ", uiInteractiveColor(), uiRaisedSurfaceBg()), "settings.printer.test",
               UiTargetKind::Button, [self] { self->testStagedPrinter(); }),
    }));
  } else if (settingsCategory_ == SettingsCategory::HimsScan) {
    const auto now = time(nullptr);
    const bool online = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15;
    const auto portValue = settingsEditingField_ ? inputBuffer_ + "_" : to_string(settingsDraft_.deviceServicePort);
    rows.push_back(settingLine("R1 service", server_.running() ? "● ready" : "× unavailable", contentWidth));
    rows.push_back(settingLine("PC endpoint", server_.running() ? server_.baseUrl() : "Unavailable", contentWidth));
    rows.push_back(target(settingLine("Service port", portValue, contentWidth, settingsEditingField_),
                          "settings.scan.port", UiTargetKind::Field,
                          [self] { self->beginSettingsFieldEdit(0); }));
    rows.push_back(settingLine("Device", himsScanConfig_.deviceId.empty() ? "Not paired" : himsScanConfig_.deviceId,
                               contentWidth));
    rows.push_back(settingLine("Status", online ? "● online" : "× offline", contentWidth));
    rows.push_back(settingLine("Firmware", deviceFirmwareVersion_.empty() ? "n/a" : deviceFirmwareVersion_, contentWidth));
    rows.push_back(settingLine("RSSI", deviceLastSeen_ == 0 ? "n/a" : to_string(deviceRssi_) + " dBm", contentWidth));
    rows.push_back(settingLine("Last result", deviceLastResult_.empty() ? "n/a" : deviceLastResult_, contentWidth));
    rows.push_back(settingLine("Pairing token", himsScanConfig_.token.empty() ? "Not configured"
                                                                              : "Configured · use Copy token",
                               contentWidth));
    rows.push_back(ftxui::hbox({
        target(styledText(" Copy token ", uiInteractiveColor(), uiRaisedSurfaceBg()), "settings.scan.copy",
               UiTargetKind::Button, [self] { self->copyHimsScanToken(); }),
        ftxui::text("  "),
        target(styledText(" Regenerate ", uiWarnColor(), uiRaisedSurfaceBg()), "settings.scan.regenerate",
               UiTargetKind::Button, [self] { self->regenerateHimsScanToken(); }),
        ftxui::text("  "),
        target(styledText(" Clear device ", uiDangerColor(), uiRaisedSurfaceBg()), "settings.scan.clear",
               UiTargetKind::Button, [self] { self->clearHimsScanPairing(); }),
    }));
    rows.push_back(uiDivider());
    rows.push_back(styledText("Recent device diagnostics", uiSecondaryText()));
    const size_t start = deviceDebugLog_.size() > 10 ? deviceDebugLog_.size() - 10 : 0;
    for (size_t index = start; index < deviceDebugLog_.size(); ++index) {
      rows.push_back(styledText(ellipsize(deviceDebugLog_[index], static_cast<size_t>(contentWidth)), uiMutedText()));
    }
    if (deviceDebugLog_.empty()) rows.push_back(styledText("Waiting for device messages", uiMutedText()));
  } else {
    const bool hasSecret = stagedDigiKeySecretChanged_ ? !stagedDigiKeySecret_.empty() : hasStoredDigiKeySecret_;
    const vector<pair<string, string>> fields = {
        {"Client ID", settingsDraft_.digiKeyClientId},
        {"Client secret", hasSecret ? "••••••••" : "Not configured"},
        {"Account ID", settingsDraft_.digiKeyAccountId},
        {"Site", settingsDraft_.digiKeySite},
        {"Language", settingsDraft_.digiKeyLanguage},
        {"Currency", settingsDraft_.digiKeyCurrency},
    };
    for (size_t index = 0; index < fields.size(); ++index) {
      const bool editing = settingsEditingField_ && settingsField_ == static_cast<int>(index);
      const auto value = editing ? (index == 1 ? string(inputBuffer_.size(), '*') : inputBuffer_) + "_"
                                 : fields[index].second;
      rows.push_back(target(settingLine(fields[index].first, value, contentWidth, editing),
                            "settings.digikey." + to_string(index), UiTargetKind::Field,
                            [self, index] { self->beginSettingsFieldEdit(static_cast<int>(index)); }));
    }
    rows.push_back(target(styledText(" Test credentials ", uiInteractiveColor(), uiRaisedSurfaceBg()),
                          "settings.digikey.test", UiTargetKind::Button,
                          [self] { self->testStagedDigiKey(); }));
    rows.push_back(styledText("Secrets are stored in Windows Credential Manager", uiMutedText()));
  }

  rows.push_back(ftxui::filler());
  rows.push_back(uiDivider());
  rows.push_back(ftxui::hbox({
      target(styledText(" Save ", settingsDirty_ ? uiFocusColor() : uiMutedText(), uiRaisedSurfaceBg()),
             "settings.save", UiTargetKind::Button, [self] { self->saveSettingsDraft(); }, settingsDirty_),
      ftxui::text("  "),
      target(styledText(" Cancel ", settingsDirty_ ? uiSecondaryText() : uiMutedText(), uiRaisedSurfaceBg()),
             "settings.cancel", UiTargetKind::Button, [self] { self->cancelSettingsDraft(); }, settingsDirty_),
      ftxui::filler(),
      styledText("Tab focus  Enter activate  Space actions", uiMutedText()),
  }));

  return ftxui::hbox({
      ftxui::vbox(move(categories)) | ftxui::bgcolor(uiCanvasBg()) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, categoryWidth),
      uiDivider(),
      ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex,
  });
}

void App::handleSettingsKey(const KeyEvent& key) {
  if (settingsEditingField_) {
    if (key.type == KeyType::Character) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
    } else if (key.type == KeyType::Backspace) {
      if (!inputBuffer_.empty()) inputBuffer_.pop_back();
      dirty_ = true;
    } else if (key.type == KeyType::Enter) {
      commitSettingsFieldEdit();
    } else if (key.type == KeyType::Escape) {
      settingsEditingField_ = false;
      inputBuffer_.clear();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Character) {
    const auto ch = static_cast<char>(tolower(static_cast<unsigned char>(key.ch)));
    if (ch == 's' && settingsDirty_) saveSettingsDraft();
    else if (ch == 'b' && settingsCategory_ == SettingsCategory::General) stageHimsFolder();
    else if (ch == 't' && settingsCategory_ == SettingsCategory::Printer) testStagedPrinter();
    else if (ch == 't' && settingsCategory_ == SettingsCategory::DigiKey) testStagedDigiKey();
    else if (ch == 'e' && (settingsCategory_ == SettingsCategory::HimsScan ||
                           settingsCategory_ == SettingsCategory::DigiKey)) beginSettingsFieldEdit(settingsField_);
    return;
  }

  if (key.type == KeyType::Left) {
    settingsCategory_ = static_cast<SettingsCategory>(max(0, static_cast<int>(settingsCategory_) - 1));
    settingsField_ = 0;
    dirty_ = true;
  } else if (key.type == KeyType::Right) {
    settingsCategory_ = static_cast<SettingsCategory>(min(3, static_cast<int>(settingsCategory_) + 1));
    settingsField_ = 0;
    if (settingsCategory_ == SettingsCategory::Printer) refreshPrinterState();
    dirty_ = true;
  } else if (key.type == KeyType::Up && settingsCategory_ == SettingsCategory::Printer && printerSelection_ > 0) {
    --printerSelection_;
    dirty_ = true;
  } else if (key.type == KeyType::Down && settingsCategory_ == SettingsCategory::Printer &&
             printerSelection_ + 1 < printerQueues_.size()) {
    ++printerSelection_;
    dirty_ = true;
  } else if (key.type == KeyType::Escape) {
    if (settingsDirty_) cancelSettingsDraft();
    else changePage(Page::Home);
  }
}

}  // namespace hims
