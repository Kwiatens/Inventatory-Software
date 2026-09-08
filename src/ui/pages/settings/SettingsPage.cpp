// Inventatory - Settings page rendering and input support.

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "platform/security/CredentialStore.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/system/StartupRegistration.h"
#include "core/storage/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
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

namespace settings_page_detail {
int settingsLabelWidth(int width) {
  // 30 keeps the stock-alert setting label readable while still leaving
  // visible air before its value on compact terminals.
  return min(30, max(16, width / 3));
}

ftxui::Element settingLine(const string& label, const string& value, int width, bool selected) {
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

ftxui::Element versionLine(const string& label, const string& installedVersion,
                           const string& availableVersion, int width) {
  const int labelWidth = settingsLabelWidth(width);
  ftxui::Elements value;
  if (!availableVersion.empty() && isVersionNewer(availableVersion, installedVersion)) {
    value = {
        styledText(installedVersion, uiMutedText()),
        styledText(" <- ", uiMutedText()),
        styledText(availableVersion, uiInteractiveColor()),
        styledText("  Update available", uiMutedText()),
    };
  } else {
    value = {styledText(installedVersion, uiPrimaryText())};
  }

  ftxui::Elements row;
  row.push_back(styledText(ellipsize(" " + label, static_cast<size_t>(max(2, labelWidth - 1))),
                           uiSecondaryText()) |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth));
  for (auto& part : value) row.push_back(move(part));
  row.push_back(ftxui::filler());
  return ftxui::hbox(move(row)) | ftxui::bgcolor(uiSurfaceBg());
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

}  // namespace settings_page_detail

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
  addCategory(SettingsCategory::Updates);
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
    rows.push_back(ftxui::hbox({
        target(uiSecondaryButton("Export CSV"), "settings.data.export", UiTargetKind::Button,
               [self] { self->exportInventory(); }),
        ftxui::text("  "),
        target(uiSecondaryButton("Backup folder"), "settings.data.backup", UiTargetKind::Button,
               [self] { self->backupData(); }),
        ftxui::text("  "),
        target(uiSecondaryButton("Restore backup"), "settings.data.restore", UiTargetKind::Button,
               [self] { self->restoreData(); }),
    }));
    rows.push_back(styledText("Restore validates the bundle first, creates a pre-restore backup, and requires Scan R1 re-pairing.",
                              uiMutedText()));
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
  } else if (settingsCategory_ == SettingsCategory::Updates) {
    const bool softwareChecking = updateCheckFuture_.valid();
    const bool firmwareChecking = scanFirmwareFuture_.valid();
    const bool anythingChecking = softwareChecking || firmwareChecking;
    const auto softwareVersionText = softwareVersion();
    const auto firmwareVersionText = deviceFirmwareVersion_.empty() ? string("Not reported") : deviceFirmwareVersion_;
    rows.push_back(versionLine("Inventatory Software Version", softwareVersionText,
                               updateCheckFailed_ ? string() : settings_.latestAvailableVersion, contentWidth));
    rows.push_back(versionLine("Inventascan Firmware Version", firmwareVersionText,
                               scanFirmwareCheckFailed_ ? string() : scanFirmwareLatestVersion_, contentWidth));
    rows.push_back(ftxui::text(""));
    rows.push_back(versionLine("Inventascan Hardware Version", "R1", string(), contentWidth));
    rows.push_back(ftxui::text(""));

    const auto checkLabel = anythingChecking ? "Searching for updates " + uiLoadingSpinner()
                                             : "Check for updates";
    rows.push_back(buttonRow(target(uiPrimaryButton(checkLabel, !anythingChecking), "settings.updates.check",
                                                    UiTargetKind::Button,
                                                    [self] { self->beginUpdateChecks(); }, !anythingChecking)));
  } else if (settingsCategory_ == SettingsCategory::Appearance) {
    auto appearanceRows = renderSettingsAppearanceRows(contentWidth);
    for (auto& row : appearanceRows) rows.push_back(move(row));
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
                                self->stageSelectedPrinterQueue();
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
      rows.push_back(buttonRow(ftxui::hbox({
          target(uiSecondaryButton("Check for firmware updates"), "settings.scan.firmware", UiTargetKind::Button,
                 [self] { self->beginScanFirmwareCheck(); }),
          ftxui::text("  "),
          target(uiSecondaryButton("Restart Bridge"), "settings.scan.restart", UiTargetKind::Button,
                 [self] { self->restartDeviceService(); }),
      })));
    }
  } else if (settingsCategory_ == SettingsCategory::DigiKey) {
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
      rows.push_back(buttonRow(target(uiPrimaryButton(refreshRunning ? uiLoadingSpinner() + " Refreshing inventory data"
                                                                      : "Refresh inventory data",
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

}  // namespace inventatory
