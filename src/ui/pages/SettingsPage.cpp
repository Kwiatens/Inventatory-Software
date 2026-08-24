// Inventatory - Hardware Inventory Management System
// Central application settings workspace and staged configuration workflow.

#include "App.h"

#include "platform/CredentialStore.h"
#include "platform/DigiKeyApi.h"
#include "platform/StartupRegistration.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

constexpr const char* kDigiKeySecretName = "digikey-client-secret";

// Every label/value row in Settings shares one label column so values align
// down the panel regardless of which row rendered them.
int settingsLabelWidth(int width) {
  // 30 keeps the stock-alert setting label readable while still leaving
  // visible air before its value on compact terminals.
  return min(30, max(16, width / 3));
}

ftxui::Element settingLine(const string& label, const string& value, int width, bool selected = false) {
  const int labelWidth = settingsLabelWidth(width);
  return ftxui::hbox({
             // Reserve one column so a full-width label never abuts its value.
             styledText(ellipsize(" " + label, static_cast<size_t>(max(2, labelWidth - 1))),
                        selected ? uiFocusColor() : uiSecondaryText()) |
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

// A button on its own row must not stretch: without a trailing filler the
// raised/filled background spans the whole panel and reads as a bar.
ftxui::Element buttonRow(ftxui::Element button) {
  return ftxui::hbox({move(button), ftxui::filler()});
}

constexpr int kAppearancePickerHueSteps = 12;
constexpr int kAppearancePickerValueSteps = 6;

uint32_t hsvToRgb(double hue, double saturation, double value) {
  hue = fmod(hue, 360.0);
  if (hue < 0.0) hue += 360.0;
  const double chroma = value * saturation;
  const double segment = hue / 60.0;
  const double intermediate = chroma * (1.0 - abs(fmod(segment, 2.0) - 1.0));
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  if (segment < 1.0) {
    red = chroma;
    green = intermediate;
  } else if (segment < 2.0) {
    red = intermediate;
    green = chroma;
  } else if (segment < 3.0) {
    green = chroma;
    blue = intermediate;
  } else if (segment < 4.0) {
    green = intermediate;
    blue = chroma;
  } else if (segment < 5.0) {
    red = intermediate;
    blue = chroma;
  } else {
    red = chroma;
    blue = intermediate;
  }
  const double match = value - chroma;
  const auto channel = [match](double component) {
    return static_cast<uint32_t>(round((component + match) * 255.0));
  };
  return (channel(red) << 16) | (channel(green) << 8) | channel(blue);
}

struct HsvCoordinates {
  int hue = 0;
  int value = 0;
};

HsvCoordinates pickerCoordinates(uint32_t rgb) {
  const double red = static_cast<double>((rgb >> 16) & 0xFFu) / 255.0;
  const double green = static_cast<double>((rgb >> 8) & 0xFFu) / 255.0;
  const double blue = static_cast<double>(rgb & 0xFFu) / 255.0;
  const double maximum = max({red, green, blue});
  const double minimum = min({red, green, blue});
  const double delta = maximum - minimum;

  double hue = 0.0;
  if (delta > 0.0001) {
    if (maximum == red) {
      hue = 60.0 * fmod((green - blue) / delta, 6.0);
    } else if (maximum == green) {
      hue = 60.0 * ((blue - red) / delta + 2.0);
    } else {
      hue = 60.0 * ((red - green) / delta + 4.0);
    }
    if (hue < 0.0) hue += 360.0;
  }
  const int hueIndex = static_cast<int>(round(hue / 360.0 * kAppearancePickerHueSteps)) %
                       kAppearancePickerHueSteps;
  const int valueIndex = static_cast<int>(round((maximum * kAppearancePickerValueSteps) - 1.0));
  return {hueIndex, clamp(valueIndex, 0, kAppearancePickerValueSteps - 1)};
}

double pickerValue(int row) {
  static constexpr double values[kAppearancePickerValueSteps] = {0.30, 0.44, 0.58, 0.72, 0.86, 1.0};
  return values[clamp(row, 0, kAppearancePickerValueSteps - 1)];
}

uint32_t pickerColor(int hue, int value) {
  const auto normalizedHue = (hue + kAppearancePickerHueSteps) % kAppearancePickerHueSteps;
  return hsvToRgb(static_cast<double>(normalizedHue) * 360.0 / kAppearancePickerHueSteps, 0.82,
                  pickerValue(value));
}

ftxui::Element appearanceColorLine(AppearanceColorRole role, int width, bool selected) {
  const auto labelWidth = max(20, min(29, width - 20));
  return ftxui::hbox({
             styledText(selected ? " > " : "   ", selected ? uiFocusColor() : uiSecondaryText()),
             styledText("   ", uiPrimaryText(), uiAppearanceColor(role)),
             styledText(" ", uiPrimaryText()),
             styledText(ellipsize(appearanceColorLabel(role), static_cast<size_t>(labelWidth)),
                        selected ? uiFocusColor() : uiSecondaryText()) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth),
             ftxui::filler(),
             styledText(appearanceColorHex(activeUiAppearance().colors[static_cast<size_t>(role)]),
                        selected ? uiPrimaryText() : uiMutedText()),
             styledText(" "),
         }) |
         ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
}

}  // namespace

string App::settingsCategoryName(SettingsCategory category) const {
  switch (category) {
    case SettingsCategory::General: return "General / Data";
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
  hasStoredDigiKeySecret_ = CredentialStore::read(kDigiKeySecretName).has_value() ||
                            !loadDigiKeyConfig().clientSecret.empty();
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
  if (settingsCategory_ == SettingsCategory::Appearance && appearancePickerOpen_) {
    closeAppearancePicker(true);
  }
  settingsField_ = field;
  settingsEditingField_ = true;
  switch (settingsCategory_) {
    case SettingsCategory::General:
      inputBuffer_ = field == 0 ? to_string(settingsDraft_.lowStockThreshold) : string();
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
      try {
        const auto threshold = stoi(trim(inputBuffer_));
        if (threshold <= 0) throw out_of_range("threshold");
        settingsDraft_.lowStockThreshold = threshold;
      } catch (...) {
        setMessage("Low-stock threshold must be a positive whole number", 4);
        return;
      }
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
      try {
        const auto port = stoi(inputBuffer_);
        if (port < 1 || port > 65535) throw out_of_range("port");
        settingsDraft_.deviceServicePort = static_cast<uint16_t>(port);
      } catch (...) {
        setMessage("Device service port must be between 1 and 65535", 4);
        return;
      }
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
  InventatoryDataPaths switchedPaths;
  if (dataChanged) {
    stopDigiKeyRefresh();
  }
  if (quickLabelsChanged) {
    settingsDraft_.quickLabelRevision = settings_.quickLabelRevision == UINT32_MAX
                                            ? 1U
                                            : max(1U, settings_.quickLabelRevision + 1U);
  }
  if (dataChanged) {
    const auto candidateDatabase = settingsDraft_.dataDirectory / "inventory.db";
    if (filesystem::exists(candidateDatabase, error)) {
      InventoryStore candidate;
      if (!candidate.load(candidateDatabase)) {
        setMessage("The selected folder contains an inventory database Inventatory cannot load", 5);
        return false;
      }
    }
    auto activePaths = InventatoryDataPaths{dataPath_, inventoryPath_, printerPath_, activityPath_, inventatoryScanConfigPath_};
    if (!switchInventatoryDataPathsAfterSaving(activePaths, settingsDraft_.dataDirectory,
                                               [this] { return saveState(); })) {
      setMessage(persistenceError_.empty() ? "Unable to save the current Inventatory data" : persistenceError_, 5);
      return false;
    }
    switchedPaths = move(activePaths);
  }
  if (stagedDigiKeySecretChanged_ && !CredentialStore::write(kDigiKeySecretName, stagedDigiKeySecret_)) {
    setMessage("Unable to save the DigiKey secret securely", 5);
    return false;
  }
  if (backgroundChanged) {
    string startupError;
    if (!setBackgroundStartupEnabled(settingsDraft_.backgroundServiceEnabled, startupError)) {
      setMessage("Unable to update Windows startup: " + startupError, 5);
      return false;
    }
    settingsDraft_.backgroundConsentAsked = true;
  }
  if (!saveAppSettings(settingsPath_, settingsDraft_)) {
    if (backgroundChanged) {
      string ignored;
      setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, ignored);
    }
    setMessage("Unable to save Inventatory settings", 5);
    return false;
  }

  if (dataChanged) {
    dataPath_ = move(switchedPaths.dataDirectory);
    inventoryPath_ = move(switchedPaths.inventory);
    printerPath_ = move(switchedPaths.printer);
    activityPath_ = move(switchedPaths.activity);
    inventatoryScanConfigPath_ = move(switchedPaths.scanConfig);
    quickLabelsPath_ = dataPath_ / "quick_labels.conf";
    loadInventatoryScanConfig(inventatoryScanConfigPath_, inventatoryScanConfig_);
    loadState();
    server_.setDeviceCredentials(inventatoryScanConfig_.deviceId, inventatoryScanConfig_.token,
                                 appSettingsDirectory() / "inventatory-scan-replay.state");
  }

  if ((quickLabelsChanged || dataChanged) &&
      !saveQuickLabels(quickLabelsPath_, settingsDraft_.quickLabelPresets, settingsDraft_.quickLabelRevision)) {
    setMessage("Unable to save quick-label settings", 5);
    return false;
  }

  {
    lock_guard<mutex> lock(quickLabelMutex_);
    settings_ = settingsDraft_;
  }
  applyUiAppearance(settings_.appearance);
  if (stagedDigiKeySecretChanged_) hasStoredDigiKeySecret_ = !stagedDigiKeySecret_.empty();
  autoPrintScannedLabels_ = settings_.autoPrintScannedLabels;
  if (backgroundChanged) {
    if (settings_.backgroundServiceEnabled) {
      backgroundController_.start(true, false, [this] { backgroundQuitRequested_.store(true); });
    } else {
      backgroundController_.stop();
    }
  }
  if (!settings_.printerQueue.empty()) {
    printerService_.setConfiguredPrinter(settings_.printerQueue);
    printerCheck_ = printerService_.probeConfiguredPrinter();
    printerService_.saveConfig(printerPath_);
  }
  settingsDirty_ = false;
  appearancePickerOpen_ = false;
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  bleWifiPassword_.assign(bleWifiPassword_.size(), '\0');
  bleWifiPassword_.clear();
  setMessage(portChanged ? "Settings saved; restart Inventatory to apply the device service port" : "Settings saved", 4);
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

ftxui::Element App::renderSettingsUi() const {
  auto self = const_cast<App*>(this);
  constexpr int categoryWidth = 24;
  const auto* active = ftxui::ScreenInteractive::Active();
  const int contentWidth = max(60, (active != nullptr ? active->dimx() : 120) - categoryWidth - 3);

  ftxui::Elements categories;
  categories.push_back(styledText(" SETTINGS", uiMutedText()));
  const auto addCategory = [&](SettingsCategory category) {
    const bool selected = category == settingsCategory_;
    auto row = fullLine(string(selected ? "  > " : "    ") + settingsCategoryName(category),
                        selected ? uiFocusColor() : uiSecondaryText(),
                        selected ? uiSelectionBg() : uiCanvasBg());
    categories.push_back(target(row, "settings.category." + settingsCategoryName(category), UiTargetKind::Category,
                                [self, category] {
                                  self->settingsCategory_ = category;
                                   self->settingsField_ = 0;
                                   self->settingsEditingField_ = false;
                                   self->appearancePickerOpen_ = false;
                                  if (category == SettingsCategory::Printer) self->refreshPrinterState();
                                  self->dirty_ = true;
                                }));
  };
  categories.push_back(styledText(" SYSTEM", uiDimColor()));
  addCategory(SettingsCategory::General);
  addCategory(SettingsCategory::Appearance);
  categories.push_back(styledText(" OUTPUT", uiDimColor()));
  addCategory(SettingsCategory::Printer);
  addCategory(SettingsCategory::QuickLabels);
  categories.push_back(styledText(" DEVICES", uiDimColor()));
  addCategory(SettingsCategory::InventatoryScan);
  categories.push_back(styledText(" INTEGRATIONS", uiDimColor()));
  addCategory(SettingsCategory::DigiKey);

  ftxui::Elements rows;
  rows.push_back(ftxui::hbox({
      uiHeaderText(settingsCategoryName(settingsCategory_), uiPrimaryText()),
      ftxui::filler(),
      styledText(settingsDirty_ ? "Unsaved changes" : "Saved", settingsDirty_ ? uiWarnColor() : uiSuccessColor()),
      ftxui::text(" "),
  }));
  rows.push_back(uiDivider());

  if (settingsCategory_ == SettingsCategory::General) {
    rows.push_back(uiHeaderText("DATA STORAGE", uiSecondaryText()));
    rows.push_back(settingLine("Inventatory folder", settingsDraft_.dataDirectory.string(), contentWidth));
    rows.push_back(target(settingLine("Change folder", "Browse...", contentWidth), "settings.data.browse",
                          UiTargetKind::Button, [self] { self->stageInventatoryFolder(); }));
    rows.push_back(uiDivider());
    rows.push_back(uiHeaderText("APPLICATION", uiSecondaryText()));
    rows.push_back(settingLine("Settings file", settingsPath_.string(), contentWidth));
    rows.push_back(target(settingLine("Background & startup", settingsDraft_.backgroundServiceEnabled ? "On" : "Off",
                                      contentWidth),
                          "settings.general.background", UiTargetKind::Field, [self] {
                            self->settingsDraft_.backgroundServiceEnabled = !self->settingsDraft_.backgroundServiceEnabled;
                            self->settingsDraft_.backgroundConsentAsked = true;
                            self->settingsDirty_ = true;
                            self->dirty_ = true;
                          }));
    rows.push_back(target(settingLine("Low-stock warning threshold",
                                      settingsEditingField_ && settingsField_ == 0
                                          ? inputBuffer_ + "_"
                                          : to_string(settingsDraft_.lowStockThreshold),
                                      contentWidth, settingsEditingField_ && settingsField_ == 0),
                          "settings.general.low_stock_threshold", UiTargetKind::Field,
                          [self] { self->beginSettingsFieldEdit(0); }));
    rows.push_back(uiDivider());
    rows.push_back(uiHeaderText("PUBLIC BETA UPDATES", uiSecondaryText()));
    rows.push_back(target(settingLine("Daily GitHub check", settingsDraft_.updateChecksEnabled ? "On" : "Off", contentWidth),
                          "settings.general.updates", UiTargetKind::Field, [self] {
                            self->settingsDraft_.updateChecksEnabled = !self->settingsDraft_.updateChecksEnabled;
                            self->settingsDirty_ = true;
                            self->dirty_ = true;
                          }));
    const auto available = settings_.latestAvailableVersion.empty() ? "Up to date" : "Version " + settings_.latestAvailableVersion + " available";
    rows.push_back(settingLine("Release status", available, contentWidth));
    rows.push_back(buttonRow(target(uiSecondaryButton("Check now"), "settings.general.check_updates",
                                    UiTargetKind::Button, [self] {
                                      self->settings_.lastUpdateCheckUnixSeconds = 0;
                                      self->beginUpdateCheckIfDue();
                                      self->setMessage("Checking the public beta release...", 4);
                                    })));
  } else if (settingsCategory_ == SettingsCategory::Appearance) {
    const int colorCellWidth = max(28, (contentWidth - 2) / 2);
    const auto addAppearanceSection = [&](const string& title,
                                          initializer_list<AppearanceColorRole> roles) {
      rows.push_back(uiHeaderText(title, uiSecondaryText()));
      vector<AppearanceColorRole> section(roles);
      for (size_t offset = 0; offset < section.size(); offset += 2) {
        ftxui::Elements columns;
        for (size_t column = 0; column < 2; ++column) {
          const auto roleIndex = offset + column;
          if (roleIndex >= section.size()) {
            columns.push_back(ftxui::filler() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, colorCellWidth));
            continue;
          }
          const auto role = section[roleIndex];
          const auto index = static_cast<int>(role);
          columns.push_back(target(
              appearanceColorLine(role, colorCellWidth, settingsField_ == index),
              "settings.appearance.color." + to_string(index), UiTargetKind::Field,
              [self, index] {
                self->settingsField_ = index;
                self->settingsEditingField_ = false;
                self->appearancePickerOpen_ = false;
                self->inputBuffer_.clear();
                self->dirty_ = true;
              }));
        }
        rows.push_back(ftxui::hbox(move(columns)));
      }
    };

    if (!appearancePickerOpen_) {
      addAppearanceSection("BACKGROUNDS", {
          AppearanceColorRole::CanvasBg,
          AppearanceColorRole::SurfaceBg,
          AppearanceColorRole::RaisedSurfaceBg,
          AppearanceColorRole::HoverBg,
          AppearanceColorRole::SelectionBg,
          AppearanceColorRole::Divider,
      });
      addAppearanceSection("TEXT", {
          AppearanceColorRole::PrimaryText,
          AppearanceColorRole::SecondaryText,
          AppearanceColorRole::MutedText,
          AppearanceColorRole::FocusText,
      });
      addAppearanceSection("ACCENTS AND STATUS", {
          AppearanceColorRole::Interactive,
          AppearanceColorRole::Success,
          AppearanceColorRole::Link,
          AppearanceColorRole::WarningText,
          AppearanceColorRole::DangerText,
      });
      addAppearanceSection("STATUS BACKGROUNDS", {
          AppearanceColorRole::ActiveBg,
          AppearanceColorRole::ActiveSoftBg,
          AppearanceColorRole::WarningBg,
          AppearanceColorRole::DangerBg,
          AppearanceColorRole::DangerFlashBg,
      });
    }

    const auto selectedIndex = clamp(settingsField_, 0, static_cast<int>(kAppearanceColorCount) - 1);
    const auto selectedRole = static_cast<AppearanceColorRole>(selectedIndex);
    rows.push_back(uiDivider());
    rows.push_back(uiHeaderText("EDIT COLOR", uiSecondaryText()));
    rows.push_back(ftxui::hbox({
        styledText(" Selected", uiSecondaryText()) |
            ftxui::size(ftxui::WIDTH, ftxui::EQUAL, settingsLabelWidth(contentWidth)),
        styledText(appearanceColorLabel(selectedRole), uiPrimaryText()),
        ftxui::filler(),
        styledText("   ", uiPrimaryText(), uiAppearanceColor(selectedRole)),
        styledText(" " + appearanceColorHex(settingsDraft_.appearance.colors[static_cast<size_t>(selectedIndex)]),
                   uiPrimaryText()),
        styledText(" "),
    }));

    const auto hexValue = settingsEditingField_ && settingsField_ == selectedIndex
                              ? inputBuffer_ + "_"
                              : appearanceColorHex(settingsDraft_.appearance.colors[static_cast<size_t>(selectedIndex)]);
    rows.push_back(target(settingLine("Hex value", hexValue, contentWidth,
                                      settingsEditingField_ && settingsField_ == selectedIndex),
                          "settings.appearance.hex", UiTargetKind::Field,
                          [self] { self->beginSettingsFieldEdit(self->settingsField_); }));

    if (appearancePickerOpen_) {
      rows.push_back(uiHeaderText("HUE / VALUE PICKER", uiSecondaryText()));
      for (int value = kAppearancePickerValueSteps - 1; value >= 0; --value) {
        ftxui::Elements pickerRow;
        for (int hue = 0; hue < kAppearancePickerHueSteps; ++hue) {
          const bool selected = hue == appearancePickerHue_ && value == appearancePickerValue_;
          const auto color = pickerColor(hue, value);
          auto cell = styledText(selected ? "[]" : "  ", uiPrimaryText(),
                                 ftxui::Color::RGB(static_cast<uint8_t>((color >> 16) & 0xFFu),
                                                   static_cast<uint8_t>((color >> 8) & 0xFFu),
                                                   static_cast<uint8_t>(color & 0xFFu)));
          pickerRow.push_back(target(move(cell), "settings.appearance.picker." + to_string(hue) + "." +
                                                       to_string(value),
                                     UiTargetKind::Cell,
                                     [self, hue, value] {
                                       self->appearancePickerHue_ = hue;
                                       self->appearancePickerValue_ = value;
                                       self->applyAppearancePickerColor();
                                     },
                                     true, false));
        }
        rows.push_back(ftxui::hbox(move(pickerRow)));
      }
      rows.push_back(styledText("Arrows change hue/value   Enter accept   Esc cancel", uiMutedText()));
    }

    ftxui::Elements appearanceActions;
    if (appearancePickerOpen_) {
      appearanceActions.push_back(target(uiPrimaryButton("Accept picker"), "settings.appearance.picker.accept",
                                         UiTargetKind::Button, [self] { self->closeAppearancePicker(true); }));
      appearanceActions.push_back(ftxui::text("  "));
      appearanceActions.push_back(target(uiSecondaryButton("Cancel picker", uiSecondaryText()),
                                         "settings.appearance.picker.cancel", UiTargetKind::Button,
                                         [self] { self->closeAppearancePicker(false); }));
    } else {
      appearanceActions.push_back(target(uiPrimaryButton("Open picker"), "settings.appearance.picker.open",
                                         UiTargetKind::Button, [self] { self->openAppearancePicker(); }));
      appearanceActions.push_back(ftxui::text("  "));
      appearanceActions.push_back(target(uiSecondaryButton("Edit hex", uiSecondaryText()),
                                         "settings.appearance.hex.edit", UiTargetKind::Button,
                                         [self] { self->beginSettingsFieldEdit(self->settingsField_); }));
    }
    appearanceActions.push_back(ftxui::text("  "));
    appearanceActions.push_back(target(uiSecondaryButton("Reset selected", uiWarnColor()),
                                       "settings.appearance.reset.selected", UiTargetKind::Button,
                                       [self] { self->resetSelectedAppearanceColor(); }));
    appearanceActions.push_back(ftxui::text("  "));
    appearanceActions.push_back(target(uiSecondaryButton("Reset all", uiDangerColor()),
                                       "settings.appearance.reset.all", UiTargetKind::Button,
                                       [self] { self->resetAppearanceColors(); }));
    rows.push_back(ftxui::hbox(move(appearanceActions)));
  } else if (settingsCategory_ == SettingsCategory::Printer) {
    rows.push_back(uiHeaderText("PRINT QUEUE", uiSecondaryText()));
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
    rows.push_back(uiHeaderText("DETECTED QUEUES", uiSecondaryText()));
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
        target(uiSecondaryButton("Refresh"), "settings.printer.refresh", UiTargetKind::Button,
               [self] { self->refreshPrinterState(); }),
        ftxui::text("  "),
        target(uiSecondaryButton("Test selected"), "settings.printer.test", UiTargetKind::Button,
               [self] { self->testStagedPrinter(); }),
    }));
    rows.push_back(uiDivider());
    rows.push_back(uiHeaderText("CUSTOM LABEL", uiSecondaryText()));
    const auto wireValue = settingsEditingField_ && settingsField_ == 50 ? inputBuffer_ + "_"
                                                                           : wireLabelText_.empty() ? "Enter custom wire text" : wireLabelText_;
    rows.push_back(target(settingLine("Wire label", wireValue, contentWidth, settingsEditingField_ && settingsField_ == 50),
                          "settings.printer.wire", UiTargetKind::Field,
                          [self] { self->beginSettingsFieldEdit(50); }));
    rows.push_back(ftxui::text(""));
    rows.push_back(buttonRow(target(uiPrimaryButton("Print custom label", !wireLabelText_.empty()),
                                    "settings.printer.wire.custom", UiTargetKind::Button,
                                    [self] { self->printWireLabel(self->wireLabelText_); },
                                    !wireLabelText_.empty())));
  } else if (settingsCategory_ == SettingsCategory::QuickLabels) {
    rows.push_back(uiHeaderText("QUICK LABELS / PRESETS", uiSecondaryText()));
    const bool canAddPreset = settingsDraft_.quickLabelPresets.size() < kQuickLabelPresetLimit;
    rows.push_back(buttonRow(target(uiPrimaryButton("+ Add quick label", canAddPreset),
                                    "settings.quick_label.add.primary", UiTargetKind::Button,
                                    [self] { self->addQuickLabelPreset(); }, canAddPreset)));
    rows.push_back(uiDivider());
    if (settingsDraft_.quickLabelPresets.empty()) {
      rows.push_back(styledText("No quick labels yet", uiWarnColor()));
    }
    for (size_t index = 0; index < settingsDraft_.quickLabelPresets.size(); ++index) {
      const bool editing = settingsEditingField_ && settingsField_ == static_cast<int>(index);
      rows.push_back(target(settingLine(to_string(index + 1), editing ? inputBuffer_ + "_"
                                                               : settingsDraft_.quickLabelPresets[index], contentWidth,
                                        settingsField_ == static_cast<int>(index)),
                            "settings.quick_label." + to_string(index), UiTargetKind::Field,
                            [self, index] { self->beginSettingsFieldEdit(static_cast<int>(index)); }));
    }
    const bool presetSelected =
        settingsField_ >= 0 && settingsField_ < static_cast<int>(settingsDraft_.quickLabelPresets.size());
    rows.push_back(ftxui::hbox({
        target(uiSecondaryButton("Test", nullopt, presetSelected), "settings.quick_label.test", UiTargetKind::Button,
               [self] { self->testQuickLabelPreset(); }, presetSelected),
        ftxui::text("  "),
        target(uiSecondaryButton("Remove", uiWarnColor(), presetSelected), "settings.quick_label.remove",
               UiTargetKind::Button, [self] { self->deleteQuickLabelPreset(); }, presetSelected),
        ftxui::text("  "),
        target(uiSecondaryButton("Up", uiSecondaryText(), settingsField_ > 0), "settings.quick_label.up",
               UiTargetKind::Button, [self] { self->moveQuickLabelPreset(-1); }, settingsField_ > 0),
        ftxui::text("  "),
        target(uiSecondaryButton("Down", uiSecondaryText(),
                                 settingsField_ >= 0 &&
                                     settingsField_ + 1 < static_cast<int>(settingsDraft_.quickLabelPresets.size())),
               "settings.quick_label.down", UiTargetKind::Button, [self] { self->moveQuickLabelPreset(1); },
               settingsField_ >= 0 && settingsField_ + 1 < static_cast<int>(settingsDraft_.quickLabelPresets.size())),
    }));
  } else if (settingsCategory_ == SettingsCategory::InventatoryScan) {
    const bool setupComplete = inventatoryScanConfig_.setupComplete || !inventatoryScanConfig_.deviceId.empty();
    if (!setupComplete) {
      rows.push_back(buttonRow(target(uiPrimaryButton("Begin Setup"), "settings.scan.begin_setup", UiTargetKind::Button,
                                    [self] { self->openInventatoryScanSetup(); })));
    } else {
      // Pairing is the reason this panel exists, so it leads. Token and
      // diagnostics operations stay on the Actions sheet.
      rows.push_back(uiHeaderText("DEVICE", uiSecondaryText()));
      rows.push_back(buttonRow(target(uiPrimaryButton("Pair new device"), "settings.scan.pair", UiTargetKind::Button,
                                      [self] { self->openInventatoryScanSetup(); })));
      rows.push_back(ftxui::text(""));
      const auto now = time(nullptr);
      const bool online = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15;
      const bool hasDeviceIdentity = !inventatoryScanConfig_.deviceId.empty();
      const auto stateColor = !hasDeviceIdentity ? uiMutedText() : online ? uiSuccessColor() : uiWarnColor();
      const auto stateWord = !hasDeviceIdentity ? "Waiting for device" : online ? "Online" : "Offline";
      rows.push_back(ftxui::hbox({
          styledText(" Status", uiSecondaryText()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, settingsLabelWidth(contentWidth)),
          styledText(stateWord, stateColor),
          ftxui::filler(),
      }));
      if (hasDeviceIdentity) {
        // Signal strength and last-contact are only meaningful once the device
        // has actually reported in; before that they would read as fake zeros.
        auto detail = inventatoryScanConfig_.deviceId;
        if (deviceLastSeen_ > 0) {
          detail += "  \xC2\xB7  " + to_string(deviceRssi_) + " dBm  \xC2\xB7  seen " +
                    to_string(static_cast<long long>(now - deviceLastSeen_)) + "s ago";
        }
        rows.push_back(ftxui::hbox({
            styledText(" Device", uiSecondaryText()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, settingsLabelWidth(contentWidth)),
            styledText(ellipsize(detail, static_cast<size_t>(max(8, contentWidth - settingsLabelWidth(contentWidth) - 2))),
                       uiPrimaryText()),
            ftxui::filler(),
        }));
      }
      rows.push_back(uiDivider());
      const auto portValue = settingsEditingField_ ? inputBuffer_ + "_" : to_string(settingsDraft_.deviceServicePort);
      rows.push_back(target(settingLine("Service port", portValue, contentWidth, settingsEditingField_),
                            "settings.scan.port", UiTargetKind::Field,
                            [self] { self->beginSettingsFieldEdit(0); }));
      rows.push_back(settingLine("Firmware", scanFirmwareStatus(), contentWidth));
      rows.push_back(buttonRow(target(uiSecondaryButton("Check for firmware updates"), "settings.scan.firmware",
                                      UiTargetKind::Button, [self] { self->beginScanFirmwareCheck(); })));
    }
  } else {
    const bool configured = !trim(settings_.digiKeyClientId).empty() && hasStoredDigiKeySecret_;
    if (!configured) {
      rows.push_back(buttonRow(target(uiPrimaryButton("Begin Setup"), "settings.digikey.begin_setup", UiTargetKind::Button,
                                    [self] { self->openDigiKeySetup(); })));
    } else {
      rows.push_back(uiHeaderText("DIGIKEY API CREDENTIALS", uiSecondaryText()));
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
      rows.push_back(buttonRow(target(uiSecondaryButton("Test credentials"), "settings.digikey.test",
                                       UiTargetKind::Button, [self] { self->testStagedDigiKey(); })));
      rows.push_back(uiDivider());
      rows.push_back(uiHeaderText("INVENTORY ENRICHMENT", uiSecondaryText()));
      const bool refreshRunning = !digiKeyRefreshQueue_.empty() || digiKeyRefreshFuture_.valid();
      string refreshStatus = "Not run";
      if (refreshRunning) {
        refreshStatus = "Refreshing " + to_string(digiKeyRefreshCompleted_) + "/" +
                        to_string(digiKeyRefreshTotal_) + " items";
      } else if (digiKeyRefreshTotal_ > 0) {
        refreshStatus = "Last run: " + to_string(digiKeyRefreshSucceeded_) + " updated, " +
                        to_string(digiKeyRefreshFailed_) + " failed";
      }
      rows.push_back(settingLine("Refresh status", refreshStatus, contentWidth));
      if (!refreshRunning && !digiKeyRefreshLastError_.empty()) {
        rows.push_back(styledText(
            ellipsize("Last error: " + digiKeyRefreshLastError_, static_cast<size_t>(max(8, contentWidth))),
            uiWarnColor()));
      }
      const bool refreshEnabled = !refreshRunning && !settingsDirty_;
      rows.push_back(buttonRow(target(uiPrimaryButton(refreshRunning ? "Refreshing..." : "Refresh inventory data",
                                                        refreshEnabled),
                                       "settings.digikey.refresh", UiTargetKind::Button,
                                       [self] { self->beginDigiKeyRefresh(); }, refreshEnabled)));
    }
  }

  rows.push_back(ftxui::filler());
  rows.push_back(uiDivider());
  rows.push_back(ftxui::hbox({
      target(uiPrimaryButton("Save", settingsDirty_), "settings.save", UiTargetKind::Button,
             [self] { self->saveSettingsDraft(); }, settingsDirty_),
      ftxui::text("  "),
      target(uiSecondaryButton("Cancel", uiSecondaryText(), settingsDirty_), "settings.cancel", UiTargetKind::Button,
             [self] { self->cancelSettingsDraft(); }, settingsDirty_),
      ftxui::filler(),
       styledText(appearancePickerOpen_ ? "arrows picker  Enter accept  Esc cancel"
                                       : "↑↓ categories  j/k lists  Tab focus  Enter activate",
                  uiMutedText()),
  }));

  return ftxui::hbox({
      ftxui::vbox(move(categories)) | ftxui::bgcolor(uiCanvasBg()) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, categoryWidth),
      uiDivider(),
      ftxui::vbox(move(rows)) | ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex,
  });
}

void App::handleSettingsKey(const KeyEvent& key) {
  if (appearancePickerOpen_) {
    if (key.type == KeyType::Left) {
      moveAppearancePicker(-1, 0);
    } else if (key.type == KeyType::Right) {
      moveAppearancePicker(1, 0);
    } else if (key.type == KeyType::Up) {
      moveAppearancePicker(0, 1);
    } else if (key.type == KeyType::Down) {
      moveAppearancePicker(0, -1);
    } else if (key.type == KeyType::Character && (key.ch == 'j' || key.ch == 'k')) {
      moveAppearancePicker(0, key.ch == 'j' ? -1 : 1);
    } else if (key.type == KeyType::Enter) {
      closeAppearancePicker(true);
    } else if (key.type == KeyType::Escape) {
      closeAppearancePicker(false);
    }
    return;
  }

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
    else if (ch == 'b' && settingsCategory_ == SettingsCategory::General) stageInventatoryFolder();
    else if (ch == 't' && settingsCategory_ == SettingsCategory::Printer) testStagedPrinter();
    else if (ch == 'a' && settingsCategory_ == SettingsCategory::QuickLabels) addQuickLabelPreset();
    else if (ch == 'x' && settingsCategory_ == SettingsCategory::QuickLabels) deleteQuickLabelPreset();
    else if (ch == '[' && settingsCategory_ == SettingsCategory::QuickLabels) moveQuickLabelPreset(-1);
    else if (ch == ']' && settingsCategory_ == SettingsCategory::QuickLabels) moveQuickLabelPreset(1);
    else if (ch == 't' && settingsCategory_ == SettingsCategory::QuickLabels) testQuickLabelPreset();
    else if (ch == 't' && settingsCategory_ == SettingsCategory::DigiKey) testStagedDigiKey();
    else if (ch == 'p' && settingsCategory_ == SettingsCategory::Appearance) openAppearancePicker();
    else if (ch == 'r' && settingsCategory_ == SettingsCategory::Appearance) resetSelectedAppearanceColor();
    else if (ch == 'd' && settingsCategory_ == SettingsCategory::Appearance) resetAppearanceColors();
    else if (ch == 'e' && (settingsCategory_ == SettingsCategory::General || settingsCategory_ == SettingsCategory::Appearance ||
                           settingsCategory_ == SettingsCategory::QuickLabels ||
                           settingsCategory_ == SettingsCategory::InventatoryScan ||
                           settingsCategory_ == SettingsCategory::DigiKey)) beginSettingsFieldEdit(settingsField_);
    if (ch != 'j' && ch != 'k') return;
    if (settingsCategory_ == SettingsCategory::Appearance) {
      if (ch == 'j' && settingsField_ + 1 < static_cast<int>(kAppearanceColorCount)) ++settingsField_;
      if (ch == 'k' && settingsField_ > 0) --settingsField_;
      dirty_ = true;
    } else if (settingsCategory_ == SettingsCategory::QuickLabels) {
      if (ch == 'j' && settingsField_ + 1 < static_cast<int>(settingsDraft_.quickLabelPresets.size())) ++settingsField_;
      if (ch == 'k' && settingsField_ > 0) --settingsField_;
      dirty_ = true;
    } else if (settingsCategory_ == SettingsCategory::Printer) {
      if (ch == 'j' && printerSelection_ + 1 < printerQueues_.size()) ++printerSelection_;
      if (ch == 'k' && printerSelection_ > 0) --printerSelection_;
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Left) {
    settingsCategory_ = static_cast<SettingsCategory>(max(0, static_cast<int>(settingsCategory_) - 1));
    settingsField_ = 0;
    appearancePickerOpen_ = false;
    dirty_ = true;
  } else if (key.type == KeyType::Right) {
    settingsCategory_ = static_cast<SettingsCategory>(min(5, static_cast<int>(settingsCategory_) + 1));
    settingsField_ = 0;
    appearancePickerOpen_ = false;
    if (settingsCategory_ == SettingsCategory::Printer) refreshPrinterState();
    dirty_ = true;
  } else if (key.type == KeyType::Up) {
    settingsCategory_ = static_cast<SettingsCategory>(max(0, static_cast<int>(settingsCategory_) - 1));
    settingsField_ = 0;
    appearancePickerOpen_ = false;
    if (settingsCategory_ == SettingsCategory::Printer) refreshPrinterState();
    dirty_ = true;
  } else if (key.type == KeyType::Down) {
    settingsCategory_ = static_cast<SettingsCategory>(min(5, static_cast<int>(settingsCategory_) + 1));
    settingsField_ = 0;
    appearancePickerOpen_ = false;
    if (settingsCategory_ == SettingsCategory::Printer) refreshPrinterState();
    dirty_ = true;
  } else if (key.type == KeyType::Escape) {
    if (settingsDirty_) cancelSettingsDraft();
    else changePage(Page::Home);
  }
}

}  // namespace inventatory
