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

namespace {
ftxui::Element settingsGutter(bool selected) {
  return styledText(selected ? " > " : "   ", selected ? uiFocusColor() : uiSecondaryText()) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kSettingsGutterWidth);
}

ftxui::Element settingRow(const string& label, ftxui::Element value, int width, bool selected) {
  const int labelWidth = settingsLabelWidth(width);
  auto row = ftxui::hbox({
                 settingsGutter(selected),
                 // Reserve one column so a full-width label never abuts its value.
                 styledText(ellipsize(label, static_cast<size_t>(max(2, labelWidth - 1))),
                            selected ? uiFocusColor() : uiSecondaryText()) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth),
                 move(value),
                 ftxui::filler(),
             }) |
             ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
  if (selected) row = row | ftxui::select;
  return row;
}

size_t settingValueWidth(int width) {
  return static_cast<size_t>(max(8, width - settingsLabelWidth(width) - kSettingsGutterWidth - 1));
}
}  // namespace

ftxui::Element settingLine(const string& label, const string& value, int width, bool selected) {
  return settingRow(label, styledText(ellipsize(value, settingValueWidth(width)), uiPrimaryText()), width, selected);
}

ftxui::Element settingToggleLine(const string& label, bool enabled, int width, bool selected) {
  return settingRow(label, styledText(enabled ? "On" : "Off", enabled ? uiSuccessColor() : uiMutedText()), width,
                    selected);
}

ftxui::Element settingStatusLine(const string& label, const string& value, ftxui::Color valueColor, int width) {
  return settingRow(label, styledText(ellipsize(value, settingValueWidth(width)), valueColor), width, false);
}

ftxui::Element settingListLine(const string& name, const string& status, ftxui::Color statusColor, int width,
                               bool selected) {
  const int statusWidth = status.empty() ? 0 : max(10, min(18, static_cast<int>(status.size()) + 2));
  const int nameWidth = max(12, width - statusWidth - kSettingsGutterWidth - 2);
  auto row = ftxui::hbox({
                 settingsGutter(selected),
                 styledText(ellipsize(name, static_cast<size_t>(nameWidth)),
                            selected ? uiFocusColor() : uiPrimaryText()),
                 ftxui::filler(),
                 styledText(status, statusColor),
                 ftxui::text(" "),
             }) |
             ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
  if (selected) row = row | ftxui::select;
  return row;
}

ftxui::Element settingNoteLine(const string& text, ftxui::Color color, int width) {
  return ftxui::hbox({settingsGutter(false),
                      styledText(ellipsize(text, static_cast<size_t>(max(8, width - kSettingsGutterWidth))), color),
                      ftxui::filler()});
}

ftxui::Element settingsSectionHeader(const string& title, ftxui::Element meta) {
  ftxui::Elements parts{uiHeaderText(" " + title, uiSecondaryText()), ftxui::filler()};
  if (meta) {
    parts.push_back(move(meta));
    parts.push_back(ftxui::text(" "));
  }
  return ftxui::hbox(move(parts)) | ftxui::bgcolor(uiSurfaceBg());
}

// Buttons must not stretch: without the trailing filler the raised/filled
// background spans the whole panel and reads as a bar.
ftxui::Element settingsActionRow(ftxui::Elements buttons) {
  ftxui::Elements parts{settingsGutter(false)};
  for (size_t index = 0; index < buttons.size(); ++index) {
    if (index > 0) parts.push_back(ftxui::text("  "));
    parts.push_back(move(buttons[index]));
  }
  parts.push_back(ftxui::filler());
  return ftxui::hbox(move(parts));
}

void appendSettingsSection(ftxui::Elements& panel, const string& title, ftxui::Elements rows, ftxui::Element meta) {
  if (!panel.empty()) panel.push_back(ftxui::text(""));
  panel.push_back(settingsSectionHeader(title, move(meta)));
  for (auto& row : rows) panel.push_back(move(row));
}

// Versions table shared by the header row and data rows: a fixed label column,
// capped Current/Latest columns, and a Status column that takes what is left.
struct VersionRowData {
  string label;
  string current;
  string latest;
  string status;
  bool mutedCurrent = false;  // "no data available", not a real version
  ftxui::Color latestColor = uiInteractiveColor();
  ftxui::Color statusColor = uiMutedText();
};

int versionTableValueWidth(int width) {  // each of the two version columns
  const int remainder = max(0, width - kSettingsGutterWidth - settingsLabelWidth(width));
  return min(max(8, remainder * 2 / 7), 16);
}

ftxui::Element versionTableCell(const string& text, ftxui::Color color, int width) {
  return styledText(text.empty() ? string() : ellipsize(text, static_cast<size_t>(max(1, width))), color) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, max(1, width));
}

ftxui::Element versionTableHeader(int width) {
  const int valueWidth = versionTableValueWidth(width);
  return ftxui::hbox({
             ftxui::text(string(kSettingsGutterWidth, ' ')),
             styledText("Label", uiDimColor()) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, settingsLabelWidth(width)),
             styledText("Current", uiDimColor()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, valueWidth),
             styledText("Latest", uiDimColor()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, valueWidth),
             styledText("Status", uiDimColor()),
         }) |
         ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element versionTableRow(const VersionRowData& data, int width) {
  const int labelWidth = settingsLabelWidth(width);
  const int valueWidth = versionTableValueWidth(width);
  // Status takes the remaining width: success = verified current state,
  // warn = real error, interactive = an update exists to act on,
  // muted/blank = no data yet.
  return ftxui::hbox({
             ftxui::text(string(kSettingsGutterWidth, ' ')),
             styledText(data.label, uiSecondaryText()) |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth),
             versionTableCell(data.current, data.mutedCurrent ? uiMutedText() : uiPrimaryText(), valueWidth),
             versionTableCell(data.latest, data.latestColor, valueWidth),
             versionTableCell(data.status, data.status.empty() ? uiMutedText() : data.statusColor,
                              max(1, width - kSettingsGutterWidth - labelWidth - 2 * valueWidth)),
         }) |
         ftxui::bgcolor(uiSurfaceBg());
}

ftxui::Element appearanceColorLine(AppearanceColorRole role, int width, bool selected) {
  // Gutter, swatch, gap, and the right-aligned "#RRGGBB " take 15 columns;
  // one more keeps a full-width label from abutting the hex value.
  const auto labelWidth = max(10, width - 16);
  auto row = ftxui::hbox({
                 styledText(selected ? " > " : "   ", selected ? uiFocusColor() : uiSecondaryText()),
                 styledText("   ", uiPrimaryText(), uiAppearanceColor(role)),
                 styledText(" ", uiPrimaryText()),
                 styledText(ellipsize(appearanceColorLabel(role), static_cast<size_t>(labelWidth - 1)),
                            selected ? uiFocusColor() : uiSecondaryText()) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth),
                 ftxui::filler(),
                 styledText(appearanceColorHex(activeUiAppearance().colors[static_cast<size_t>(role)]),
                            selected ? uiPrimaryText() : uiMutedText()),
                 styledText(" "),
             }) |
             ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
  if (selected) row = row | ftxui::select;
  return row;
}

}  // namespace settings_page_detail

ftxui::Element App::renderSettingsUi() const {
  auto self = const_cast<App*>(this);
  constexpr int categoryWidth = 24;
  const auto* active = ftxui::ScreenInteractive::Active();
  const int contentWidth = max(60, (active != nullptr ? active->dimx() : 120) - categoryWidth - 3);

  ftxui::Elements categories;
  categories.push_back(styledText(" Settings", uiMutedText()));
  const auto addCategory = [&](SettingsCategory category, int indent) {
    const bool selected = category == settingsCategory_;
    const string marker = indent > 0 ? (selected ? "    > " : "      ") : (selected ? "  > " : "    ");
    auto row = fullLine(marker + settingsCategoryName(category),
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
  for (const auto& entry : settingsCategoryEntries()) {
    const auto category = static_cast<SettingsCategory>(entry.categoryIndex);
    if (entry.categoryIndex == 0) categories.push_back(styledText(" System", uiDimColor()));
    if (entry.categoryIndex == 3) categories.push_back(styledText(" Devices", uiDimColor()));
    if (entry.categoryIndex == 6) categories.push_back(styledText(" Integrations", uiDimColor()));
    addCategory(category, entry.indent);
  }

  auto categoryHeader = ftxui::hbox({
      uiHeaderText(" " + settingsCategoryName(settingsCategory_), uiPrimaryText()),
      ftxui::filler(),
      styledText(settingsDirty_ ? "Unsaved changes" : "Saved", settingsDirty_ ? uiWarnColor() : uiSuccessColor()),
      ftxui::text(" "),
  }) | ftxui::bgcolor(uiSurfaceBg());

  ftxui::Elements rows;
  const auto toggleSetting = [self](bool& value) {
    value = !value;
    self->settingsDirty_ = true;
    self->dirty_ = true;
  };

  if (settingsCategory_ == SettingsCategory::General) {
    appendSettingsSection(rows, "Data storage", {
        settingLine("Inventatory folder", settingsDraft_.dataDirectory.string(), contentWidth),
        settingsActionRow({
            target(uiSecondaryButton("Change folder"), "settings.data.browse", UiTargetKind::Button,
                   [self] { self->stageInventatoryFolder(); }),
        }),
    });
    appendSettingsSection(rows, "Backup and export", {
        settingsActionRow({
            target(uiSecondaryButton("Export CSV"), "settings.data.export", UiTargetKind::Button,
                   [self] { self->exportInventory(); }),
            target(uiSecondaryButton("Backup folder"), "settings.data.backup", UiTargetKind::Button,
                   [self] { self->backupData(); }),
            target(uiSecondaryButton("Restore backup"), "settings.data.restore", UiTargetKind::Button,
                   [self] { self->restoreData(); }),
        }),
    });
    const bool thresholdEditing = settingsEditingField_ && settingsField_ == 0;
    appendSettingsSection(rows, "Application", {
        target(settingToggleLine("Background & startup", settingsDraft_.backgroundServiceEnabled, contentWidth),
               "settings.general.background", UiTargetKind::Field,
               [self, toggleSetting] {
                 toggleSetting(self->settingsDraft_.backgroundServiceEnabled);
                 self->settingsDraft_.backgroundConsentAsked = true;
               }),
        target(settingLine("Low-stock threshold",
                           thresholdEditing ? inputBuffer_ + "_" : to_string(settingsDraft_.lowStockThreshold),
                           contentWidth, thresholdEditing),
               "settings.general.low_stock_threshold", UiTargetKind::Field,
               [self] { self->beginSettingsFieldEdit(0); }),
        settingStatusLine("Settings file", settingsPath_.string(), uiMutedText(), contentWidth),
    });
  } else if (settingsCategory_ == SettingsCategory::Updates) {
    const bool softwareChecking = updateCheckFuture_.valid();
    const auto softwareVersionText = softwareVersion();
    const bool canUpdate = !softwareChecking && isVersionNewer(settings_.latestAvailableVersion, softwareVersion());

    // Real last-check time from the persisted settings; 0 means no check has
    // ever completed.
    const bool hasLastCheck = settings_.lastUpdateCheckUnixSeconds > 0;
    auto lastChecked = ftxui::hbox({
        styledText("Last checked: ", uiMutedText()),
        styledText(hasLastCheck ? nowTimestampString(static_cast<time_t>(settings_.lastUpdateCheckUnixSeconds))
                                : string("Never"),
                   hasLastCheck ? uiSecondaryText() : uiMutedText()),
    });

    // Consistent Label | Current | Latest | Status table. Columns stay blank
    // where a row has no data instead of inventing placeholder values.
    VersionRowData software;
    software.label = "Inventatory software";
    software.current = softwareVersionText;
    software.latest = updateCheckFailed_ ? string() : settings_.latestAvailableVersion;
    if (softwareChecking) {
      software.statusColor = uiMutedText();
      software.status = string("Checking ") + uiLoadingSpinner();
    } else if (updateCheckFailed_) {
      software.statusColor = uiWarnColor();
      software.status = "Check failed";
    } else if (!hasLastCheck) {
      software.status = "Not checked yet";  // muted by default
    } else if (canUpdate) {
      software.statusColor = uiInteractiveColor();
      software.status = "Update available";
    } else {
      software.statusColor = uiSuccessColor();
      software.status = "Up to date";
    }

    VersionRowData firmware;
    firmware.label = "Inventascan firmware";
    firmware.current = deviceFirmwareVersion_.empty() ? string("Not reported") : deviceFirmwareVersion_;
    firmware.mutedCurrent = deviceFirmwareVersion_.empty();
    const bool firmwareNewerKnown = !scanFirmwareFuture_.valid() && !deviceFirmwareVersion_.empty() &&
                                    isVersionNewer(scanFirmwareLatestVersion_, deviceFirmwareVersion_);
    if (scanFirmwareFuture_.valid()) {
      firmware.statusColor = uiMutedText();
      firmware.status = string("Checking ") + uiLoadingSpinner();
    } else if (scanFirmwareCheckFailed_) {
      // A failed run clears the cached latest, so this precedes the newer check.
      firmware.statusColor = uiWarnColor();
      firmware.status = "Check failed";
    } else if (firmwareNewerKnown) {
      firmware.latest = scanFirmwareLatestVersion_;  // interactive color by default
      firmware.statusColor = uiInteractiveColor();
      firmware.status = "Update available";
    }

    VersionRowData hardware;
    hardware.label = "Inventascan hardware";
    hardware.current = "R1";  // latest/status: no such data for hardware revisions

    const auto checkLabel = softwareChecking ? "Searching for software updates " + uiLoadingSpinner()
                                             : "Check for software updates";
    ftxui::Elements updateActions;
    // Check is a repeatable maintenance action; the filled primary button is
    // reserved for actually updating once a newer version is known.
    updateActions.push_back(target(uiSecondaryButton(checkLabel, uiInteractiveColor(), !softwareChecking),
                                   "settings.updates.check", UiTargetKind::Button,
                                   [self] { self->beginUpdateChecks(); }, !softwareChecking));
    if (canUpdate) {
      updateActions.push_back(target(uiPrimaryButton("Update to " + settings_.latestAvailableVersion),
                                     "settings.updates.update", UiTargetKind::Button,
                                     [self] { self->beginSoftwareUpdate(); }));
    }

    appendSettingsSection(rows, "Versions", {
        versionTableHeader(contentWidth),
        versionTableRow(software, contentWidth),
        versionTableRow(firmware, contentWidth),
        versionTableRow(hardware, contentWidth),
        settingsActionRow(move(updateActions)),
    }, move(lastChecked));
    appendSettingsSection(rows, "Preferences", {
        target(settingToggleLine("Auto-check for updates", settingsDraft_.updateChecksEnabled, contentWidth),
               "settings.updates.autocheck", UiTargetKind::Field,
               [self, toggleSetting] { toggleSetting(self->settingsDraft_.updateChecksEnabled); }),
    });
  } else if (settingsCategory_ == SettingsCategory::Appearance) {
    auto appearanceRows = renderSettingsAppearanceRows(contentWidth);
    for (auto& row : appearanceRows) rows.push_back(move(row));
  } else if (settingsCategory_ == SettingsCategory::Printer) {
    appendSettingsSection(rows, "Print queue", {
        settingsDraft_.printerQueue.empty()
            ? settingStatusLine("Configured queue", "Not configured", uiWarnColor(), contentWidth)
            : settingLine("Configured queue", settingsDraft_.printerQueue, contentWidth),
        target(settingToggleLine("Auto-label", settingsDraft_.autoPrintScannedLabels, contentWidth),
               "settings.printer.autolabel", UiTargetKind::Field,
               [self, toggleSetting] { toggleSetting(self->settingsDraft_.autoPrintScannedLabels); }),
    });
    ftxui::Elements queueRows;
    if (printerQueues_.empty()) {
      queueRows.push_back(settingNoteLine("No printer queues detected", uiWarnColor(), contentWidth));
    } else {
      for (size_t index = 0; index < printerQueues_.size(); ++index) {
        const auto& printer = printerQueues_[index];
        queueRows.push_back(target(settingListLine(printer.name, printer.statusText,
                                                   printer.statusText == "Ready" ? uiSuccessColor() : uiSecondaryText(),
                                                   contentWidth, index == printerSelection_),
                                   "settings.printer." + to_string(index), UiTargetKind::Row, [self, index] {
                                     self->printerSelection_ = index;
                                     self->stageSelectedPrinterQueue();
                                   }));
      }
    }
    queueRows.push_back(settingsActionRow({
        target(uiSecondaryButton("Refresh"), "settings.printer.refresh", UiTargetKind::Button,
               [self] { self->refreshPrinterState(); }),
        target(uiSecondaryButton("Test selected"), "settings.printer.test", UiTargetKind::Button,
               [self] { self->testStagedPrinter(); }),
    }));
    appendSettingsSection(rows, "Detected queues", move(queueRows),
                          styledText(to_string(printerQueues_.size()) + " found", uiMutedText()));
  } else if (settingsCategory_ == SettingsCategory::QuickLabels) {
    const auto& presets = settingsDraft_.quickLabelPresets;
    const int presetCount = static_cast<int>(presets.size());
    const bool canAddPreset = presets.size() < kQuickLabelPresetLimit;
    ftxui::Elements presetRows;
    if (presets.empty()) presetRows.push_back(settingNoteLine("No quick labels yet", uiMutedText(), contentWidth));
    for (size_t index = 0; index < presets.size(); ++index) {
      const bool editing = settingsEditingField_ && settingsField_ == static_cast<int>(index);
      presetRows.push_back(target(settingLine("Preset " + to_string(index + 1),
                                              editing ? inputBuffer_ + "_" : presets[index], contentWidth,
                                              settingsField_ == static_cast<int>(index)),
                                  "settings.quick_label." + to_string(index), UiTargetKind::Field,
                                  [self, index] { self->beginSettingsFieldEdit(static_cast<int>(index)); }));
    }
    const bool presetSelected = settingsField_ >= 0 && settingsField_ < presetCount;
    const bool canMoveDown = settingsField_ >= 0 && settingsField_ + 1 < presetCount;
    presetRows.push_back(settingsActionRow({
        target(uiSecondaryButton("+ Add", nullopt, canAddPreset), "settings.quick_label.add.primary",
               UiTargetKind::Button, [self] { self->addQuickLabelPreset(); }, canAddPreset),
        target(uiSecondaryButton("Test", nullopt, presetSelected), "settings.quick_label.test", UiTargetKind::Button,
               [self] { self->testQuickLabelPreset(); }, presetSelected),
        target(uiSecondaryButton("Remove", uiWarnColor(), presetSelected), "settings.quick_label.remove",
               UiTargetKind::Button, [self] { self->deleteQuickLabelPreset(); }, presetSelected),
        target(uiSecondaryButton("Up", uiSecondaryText(), settingsField_ > 0), "settings.quick_label.up",
               UiTargetKind::Button, [self] { self->moveQuickLabelPreset(-1); }, settingsField_ > 0),
        target(uiSecondaryButton("Down", uiSecondaryText(), canMoveDown), "settings.quick_label.down",
               UiTargetKind::Button, [self] { self->moveQuickLabelPreset(1); }, canMoveDown),
    }));
    appendSettingsSection(rows, "Presets", move(presetRows),
                          styledText(to_string(presets.size()) + "/" + to_string(kQuickLabelPresetLimit),
                                     uiMutedText()));

    const int customLabelField = presetCount;
    const bool customLabelSelected = settingsField_ == customLabelField;
    const bool customLabelEditing = settingsEditingField_ && customLabelSelected;
    const auto wireValue = customLabelEditing ? inputBuffer_ + "_"
                                              : wireLabelText_.empty() ? "Enter custom wire text" : wireLabelText_;
    appendSettingsSection(rows, "Custom label", {
        target(settingLine("Wire label", wireValue, contentWidth, customLabelSelected), "settings.quick_label.wire",
               UiTargetKind::Field, [self, customLabelField] { self->beginSettingsFieldEdit(customLabelField); }),
        settingsActionRow({
            target(uiPrimaryButton("Print custom label", !wireLabelText_.empty()), "settings.quick_label.wire.custom",
                   UiTargetKind::Button, [self] { self->printWireLabel(self->wireLabelText_); },
                   !wireLabelText_.empty()),
        }),
    });
  } else if (settingsCategory_ == SettingsCategory::InventatoryScan) {
    const bool setupComplete = inventatoryScanConfig_.setupComplete || !inventatoryScanConfig_.deviceId.empty();
    if (!setupComplete) {
      appendSettingsSection(rows, "Setup", {
          settingStatusLine("Status", "Not set up", uiMutedText(), contentWidth),
          settingsActionRow({target(uiPrimaryButton("Begin Setup"), "settings.scan.begin_setup", UiTargetKind::Button,
                                    [self] { self->openInventatoryScanSetup(); })}),
      });
    } else {
      // Pairing is the reason this panel exists, so it leads. Token and
      // diagnostics operations stay on the Actions sheet.
      const auto now = time(nullptr);
      const bool online = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15;
      const bool hasDeviceIdentity = !inventatoryScanConfig_.deviceId.empty();
      ftxui::Elements deviceRows;
      deviceRows.push_back(settingStatusLine(
          "Status", !hasDeviceIdentity ? "Waiting for device" : online ? "Online" : "Offline",
          !hasDeviceIdentity ? uiMutedText() : online ? uiSuccessColor() : uiWarnColor(), contentWidth));
      if (hasDeviceIdentity) {
        // Signal strength and last-contact are only meaningful once the device
        // has actually reported in; before that they would read as fake zeros.
        deviceRows.push_back(settingLine("Device", inventatoryScanConfig_.deviceId, contentWidth));
        if (deviceLastSeen_ > 0) {
          deviceRows.push_back(settingLine("Signal", to_string(deviceRssi_) + " dBm", contentWidth));
          deviceRows.push_back(settingLine(
              "Last seen", to_string(static_cast<long long>(now - deviceLastSeen_)) + "s ago", contentWidth));
        }
      }
      deviceRows.push_back(settingsActionRow({target(uiPrimaryButton("Pair new device"), "settings.scan.pair",
                                                     UiTargetKind::Button,
                                                     [self] { self->openInventatoryScanSetup(); })}));
      appendSettingsSection(rows, "Device", move(deviceRows));

      const auto portValue = settingsEditingField_ ? inputBuffer_ + "_" : to_string(settingsDraft_.deviceServicePort);
      appendSettingsSection(rows, "Bridge service", {
          target(settingLine("Service port", portValue, contentWidth, settingsEditingField_), "settings.scan.port",
                 UiTargetKind::Field, [self] { self->beginSettingsFieldEdit(0); }),
          settingsActionRow({target(uiSecondaryButton("Restart bridge"), "settings.scan.restart",
                                    UiTargetKind::Button, [self] { self->restartDeviceService(); })}),
      });
    }
  } else if (settingsCategory_ == SettingsCategory::DigiKey) {
    const bool configured = !trim(settings_.digiKeyClientId).empty() && hasStoredDigiKeySecret_;
    if (!configured) {
      appendSettingsSection(rows, "Setup", {
          settingStatusLine("Status", "Not configured", uiMutedText(), contentWidth),
          settingsActionRow({target(uiPrimaryButton("Begin Setup"), "settings.digikey.begin_setup",
                                    UiTargetKind::Button, [self] { self->openDigiKeySetup(); })}),
      });
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
      ftxui::Elements credentialRows;
      for (size_t index = 0; index < fields.size(); ++index) {
        const bool editing = settingsEditingField_ && settingsField_ == static_cast<int>(index);
        const auto value = editing ? (index == 1 ? string(inputBuffer_.size(), '*') : inputBuffer_) + "_"
                                   : fields[index].second;
        credentialRows.push_back(target(settingLine(fields[index].first, value, contentWidth, editing),
                                        "settings.digikey." + to_string(index), UiTargetKind::Field,
                                        [self, index] { self->beginSettingsFieldEdit(static_cast<int>(index)); }));
      }
      credentialRows.push_back(settingsActionRow({target(uiSecondaryButton("Test credentials"),
                                                         "settings.digikey.test", UiTargetKind::Button,
                                                         [self] { self->testStagedDigiKey(); })}));
      appendSettingsSection(rows, "API credentials", move(credentialRows));

      const bool refreshRunning = !digiKeyRefreshQueue_.empty() || digiKeyRefreshFuture_.valid();
      string refreshStatus = "Not run";
      auto refreshColor = uiMutedText();
      if (refreshRunning) {
        refreshStatus = "Refreshing " + to_string(digiKeyRefreshCompleted_) + "/" +
                        to_string(digiKeyRefreshTotal_) + " items";
        refreshColor = uiInteractiveColor();
      } else if (digiKeyRefreshTotal_ > 0) {
        refreshStatus = "Last run: " + to_string(digiKeyRefreshSucceeded_) + " updated, " +
                        to_string(digiKeyRefreshFailed_) + " failed";
        refreshColor = digiKeyRefreshFailed_ > 0 ? uiWarnColor() : uiSuccessColor();
      }
      ftxui::Elements enrichmentRows;
      enrichmentRows.push_back(settingStatusLine("Refresh status", refreshStatus, refreshColor, contentWidth));
      if (!refreshRunning && !digiKeyRefreshLastError_.empty()) {
        enrichmentRows.push_back(settingStatusLine("Last error", digiKeyRefreshLastError_, uiWarnColor(), contentWidth));
      }
      const bool refreshEnabled = !refreshRunning && !settingsDirty_;
      enrichmentRows.push_back(settingsActionRow({target(
          uiPrimaryButton(refreshRunning ? uiLoadingSpinner() + " Refreshing inventory data" : "Refresh inventory data",
                          refreshEnabled),
          "settings.digikey.refresh", UiTargetKind::Button, [self] { self->beginDigiKeyRefresh(); }, refreshEnabled)}));
      appendSettingsSection(rows, "Inventory enrichment", move(enrichmentRows));
    }
  }

  auto settingsBody = ftxui::vbox(move(rows)) | ftxui::yframe | ftxui::vscroll_indicator |
                      ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex;
  auto settingsFooter = ftxui::hbox({
      ftxui::text(" "),
      target(uiPrimaryButton("Save", settingsDirty_), "settings.save", UiTargetKind::Button,
             [self] { self->saveSettingsDraft(); }, settingsDirty_),
      ftxui::text("  "),
      target(uiSecondaryButton("Cancel", uiSecondaryText(), settingsDirty_), "settings.cancel", UiTargetKind::Button,
             [self] { self->cancelSettingsDraft(); }, settingsDirty_),
      ftxui::filler(),
      styledText(appearancePickerOpen_ ? "arrows picker  Enter accept  Esc cancel"
                                       : "↑↓ categories  j/k lists  Tab focus  Enter activate",
                   uiMutedText()),
  });

  return ftxui::hbox({
      ftxui::vbox(move(categories)) | ftxui::bgcolor(uiCanvasBg()) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, categoryWidth),
      uiDivider(),
      ftxui::vbox({move(categoryHeader), uiDivider(), move(settingsBody), uiDivider(), move(settingsFooter)}) |
          ftxui::bgcolor(uiSurfaceBg()) | ftxui::flex,
  });
}

}  // namespace inventatory
