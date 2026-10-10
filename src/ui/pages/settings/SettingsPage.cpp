// Inventatory - Settings page: one row model per category, rendered on the shared page skeleton.

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "core/storage/InventorySqlite.h"
#include "label_printer/symbols/RackSymbols.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;
using namespace settings_page_detail;

namespace {

// "~/Documents/Inventatory" rather than the full home path.
string homeRelative(const filesystem::path& path) {
  auto text = path.u8string();
  const char* home = getenv("HOME");
  if (home != nullptr && *home != '\0') {
    const string prefix(home);
    if (text.compare(0, prefix.size(), prefix) == 0 && (text.size() == prefix.size() || text[prefix.size()] == '/')) {
      text = "~" + text.substr(prefix.size());
    }
  }
  return text;
}

// Paths keep both ends: "~/Documents/.../settings.conf".
string middleEllipsize(const string& text, size_t width) {
  if (text.size() <= width || width < 8) return ellipsize(text, width);
  const size_t tail = (width - 3) / 2;
  const size_t head = width - 3 - tail;
  return text.substr(0, head) + "..." + text.substr(text.size() - tail);
}

ftxui::Color toneColor(VoiceTone tone) {
  switch (tone) {
    case VoiceTone::Plain: return uiSecondaryText();
    case VoiceTone::Muted: return uiMutedText();
    case VoiceTone::Strong: return uiPrimaryText();
    case VoiceTone::Slot: return uiFocusColor();
    case VoiceTone::Success: return uiSuccessColor();
    case VoiceTone::Warning: return uiWarnColor();
    case VoiceTone::Danger: return uiDangerColor();
  }
  return uiPrimaryText();
}

string onOff(bool value) { return value ? "On" : "Off"; }

string queueStatusWord(const string& statusText) {
  // CUPS reports "idle.  enabled since ..."; the first clause is the state.
  auto word = statusText.substr(0, statusText.find('.'));
  return trim(word);
}

}  // namespace

bool App::settingsCategoryDirty(SettingsCategory category) const {
  const auto& draft = settingsDraft_;
  const auto& saved = settings_;
  switch (category) {
    case SettingsCategory::General:
      return draft.dataDirectory != saved.dataDirectory || draft.backgroundServiceEnabled != saved.backgroundServiceEnabled ||
             draft.lowStockThreshold != saved.lowStockThreshold;
    case SettingsCategory::Appearance:
      return draft.appearance.colors != saved.appearance.colors;
    case SettingsCategory::Updates:
      return draft.updateChecksEnabled != saved.updateChecksEnabled;
    case SettingsCategory::Printer:
      return draft.printerQueue != saved.printerQueue || draft.autoPrintScannedLabels != saved.autoPrintScannedLabels ||
             draft.symbolStandard != saved.symbolStandard;
    case SettingsCategory::QuickLabels:
      return draft.quickLabelPresets != saved.quickLabelPresets;
    case SettingsCategory::InventatoryScan:
      return draft.deviceServicePort != saved.deviceServicePort;
    case SettingsCategory::DigiKey:
      return draft.digiKeyClientId != saved.digiKeyClientId || draft.digiKeyAccountId != saved.digiKeyAccountId ||
             draft.digiKeySite != saved.digiKeySite || draft.digiKeyLanguage != saved.digiKeyLanguage ||
             draft.digiKeyCurrency != saved.digiKeyCurrency || stagedDigiKeySecretChanged_;
  }
  return false;
}

App::SettingsPageModel App::settingsPageModel() const {
  auto self = const_cast<App*>(this);
  const auto& draft = settingsDraft_;
  const auto& saved = settings_;
  SettingsPageModel model;
  auto& rows = model.rows;

  const auto button = [](string label, string key, string id, function<void()> run, bool primary = false,
                         bool danger = false, bool enabled = true) {
    SettingsButton result;
    result.label = move(label);
    result.key = move(key);
    result.targetId = move(id);
    result.run = move(run);
    result.primary = primary;
    result.danger = danger;
    result.enabled = enabled;
    return result;
  };
  // A staged change shows the new value in the warning colour and the saved one beside it.
  const auto markChange = [](SettingsRow& row, bool changed, const string& was) {
    if (!changed) return;
    row.tone = VoiceTone::Warning;
    row.note = "was " + was;
    row.noteTone = VoiceTone::Muted;
  };
  const auto toggleRow = [&](string group, string label, bool AppSettings::*member, string id) {
    SettingsRow row;
    row.group = move(group);
    row.label = move(label);
    row.value = onOff(draft.*member);
    row.targetId = move(id);
    row.activate = [self, member] {
      self->settingsDraft_.*member = !(self->settingsDraft_.*member);
      if (member == &AppSettings::backgroundServiceEnabled) self->settingsDraft_.backgroundConsentAsked = true;
      self->settingsDirty_ = self->settingsDraftHasChanges();
      self->dirty_ = true;
    };
    markChange(row, draft.*member != saved.*member, onOff(saved.*member));
    return row;
  };
  const auto editRow = [&](string group, string label, string value, int field, string id) {
    SettingsRow row;
    row.group = move(group);
    row.label = move(label);
    const bool editing = settingsEditingField_ && settingsField_ == field;
    row.value = editing ? inputBuffer_ + "_" : move(value);
    row.editing = editing;
    if (editing) row.tone = VoiceTone::Slot;
    row.field = field;
    row.targetId = move(id);
    row.activate = [self, field] { self->beginSettingsFieldEdit(field); };
    return row;
  };
  const auto infoRow = [&](string group, string label, string value, VoiceTone tone = VoiceTone::Plain) {
    SettingsRow row;
    row.group = move(group);
    row.label = move(label);
    row.value = move(value);
    row.tone = tone;
    return row;
  };

  switch (settingsCategory_) {
    case SettingsCategory::General: {
      model.status = {{voiceCount(static_cast<int>(store_.items().size()), "part", "parts"), VoiceTone::Strong},
                      {" in "},
                      {voiceCount(static_cast<int>(store_.racks().size()), "rack", "racks"), VoiceTone::Strong},
                      {", kept in " + homeRelative(saved.dataDirectory) + "."}};
      auto folder = infoRow("Data", "Folder", homeRelative(draft.dataDirectory), VoiceTone::Strong);
      folder.targetId = "settings.data.browse";
      folder.activate = [self] { self->stageInventatoryFolder(); };
      folder.buttons = {button("Change", "b", "settings.data.browse.button", [self] { self->stageInventatoryFolder(); })};
      markChange(folder, draft.dataDirectory != saved.dataDirectory, homeRelative(saved.dataDirectory));
      rows.push_back(move(folder));

      auto backup = infoRow("", "Backup", "All inventory data", VoiceTone::Muted);
      backup.targetId = "settings.data.backup.row";
      backup.activate = [self] { self->backupData(); };
      backup.buttons = {button("Back up", "k", "settings.data.backup", [self] { self->backupData(); }),
                        button("Restore", "r", "settings.data.restore", [self] { self->restoreData(); }, false, true)};
      rows.push_back(move(backup));

      auto exportRow = infoRow("", "Export", "Every part as CSV", VoiceTone::Muted);
      exportRow.targetId = "settings.data.export.row";
      exportRow.activate = [self] { self->exportInventory(); };
      exportRow.buttons = {button("Export", "x", "settings.data.export", [self] { self->exportInventory(); })};
      rows.push_back(move(exportRow));

      rows.push_back(toggleRow("Behaviour", "Keep running when closed", &AppSettings::backgroundServiceEnabled,
                               "settings.general.background"));
      auto threshold = editRow("", "Low stock at or below", to_string(draft.lowStockThreshold) + " pcs", 0,
                               "settings.general.low_stock_threshold");
      if (!threshold.editing) {
        markChange(threshold, draft.lowStockThreshold != saved.lowStockThreshold, to_string(saved.lowStockThreshold) + " pcs");
      }
      rows.push_back(move(threshold));
      rows.push_back(infoRow("Files", "Settings file", homeRelative(settingsPath_), VoiceTone::Muted));
      break;
    }

    case SettingsCategory::Appearance: {
      const AppearanceSettings defaults;
      int changedFromDefault = 0;
      for (size_t index = 0; index < kAppearanceColorCount; ++index) {
        if (draft.appearance.colors[index] != defaults.colors[index]) ++changedFromDefault;
      }
      if (changedFromDefault == 0) {
        model.status = VoiceLine{{"Using the default Slate colours."}};
      } else {
        model.status = VoiceLine{{voiceCount(changedFromDefault, "colour", "colours"), VoiceTone::Strong},
                                 {changedFromDefault == 1 ? " differs from the defaults." : " differ from the defaults."}};
      }
      const auto addGroup = [&](const string& group, initializer_list<AppearanceColorRole> roles) {
        bool first = true;
        for (const auto role : roles) {
          const auto index = static_cast<int>(role);
          const auto color = draft.appearance.colors[static_cast<size_t>(index)];
          const bool editing = settingsEditingField_ && settingsField_ == index;
          SettingsRow row = infoRow(first ? group : "", appearanceColorLabel(role),
                                    editing ? inputBuffer_ + "_" : appearanceColorHex(color),
                                    editing ? VoiceTone::Slot : VoiceTone::Strong);
          row.editing = editing;
          row.swatch = color;
          row.field = index;
          row.targetId = "settings.appearance.color." + to_string(index);
          row.activate = [self, index] {
            self->settingsField_ = index;
            self->openAppearancePicker();
          };
          row.buttonsOnFocus = true;
          row.buttons = {button("Pick", "p", "settings.appearance.picker.open", [self] { self->openAppearancePicker(); }),
                         button("Hex", "e", "settings.appearance.hex.edit",
                                [self] { self->beginSettingsFieldEdit(self->settingsField_); }),
                         button("Reset", "r", "settings.appearance.reset.selected",
                                [self] { self->resetSelectedAppearanceColor(); })};
          if (!editing) markChange(row, color != saved.appearance.colors[static_cast<size_t>(index)],
                                   appearanceColorHex(saved.appearance.colors[static_cast<size_t>(index)]));
          rows.push_back(move(row));
          first = false;
        }
      };
      addGroup("Surfaces", {AppearanceColorRole::CanvasBg, AppearanceColorRole::SurfaceBg, AppearanceColorRole::RaisedSurfaceBg,
                            AppearanceColorRole::HoverBg, AppearanceColorRole::SelectionBg, AppearanceColorRole::Divider});
      addGroup("Text", {AppearanceColorRole::PrimaryText, AppearanceColorRole::SecondaryText, AppearanceColorRole::MutedText,
                        AppearanceColorRole::FocusText});
      addGroup("Accents", {AppearanceColorRole::Interactive, AppearanceColorRole::Link, AppearanceColorRole::Success,
                           AppearanceColorRole::WarningText, AppearanceColorRole::DangerText});
      addGroup("Status fills", {AppearanceColorRole::ActiveBg, AppearanceColorRole::ActiveSoftBg, AppearanceColorRole::WarningBg,
                                AppearanceColorRole::DangerBg, AppearanceColorRole::DangerFlashBg});
      auto all = infoRow("", "All colours", changedFromDefault == 0 ? "Defaults" : to_string(changedFromDefault) + " changed",
                         VoiceTone::Muted);
      all.buttons = {button("Reset all", "d", "settings.appearance.reset.all", [self] { self->resetAppearanceColors(); },
                            false, true, changedFromDefault > 0)};
      rows.push_back(move(all));
      break;
    }

    case SettingsCategory::Updates: {
      const bool checking = updateCheckFuture_.valid();
      const auto version = softwareVersion();
      const bool canUpdate = !checking && isVersionNewer(saved.latestAvailableVersion, version);
      const bool hasLastCheck = saved.lastUpdateCheckUnixSeconds > 0;
      const auto lastCheck = hasLastCheck ? nowTimestampString(static_cast<time_t>(saved.lastUpdateCheckUnixSeconds))
                                          : string("Never");
      if (checking) {
        model.status = {{"Checking for updates..."}};
      } else if (updateCheckFailed_) {
        model.status = {{"The last update check failed.", VoiceTone::Danger}};
      } else if (canUpdate) {
        model.status = {{"Inventatory "}, {saved.latestAvailableVersion, VoiceTone::Warning}, {" is available. You have " + version + "."}};
      } else if (!hasLastCheck) {
        model.status = {{"Updates have not been checked yet."}};
      } else {
        model.status = {{"Inventatory "}, {version, VoiceTone::Strong}, {" is up to date. Last checked " + lastCheck + "."}};
      }

      auto software = infoRow("Software", "Inventatory", version, VoiceTone::Strong);
      if (checking) {
        software.note = "Checking...";
      } else if (updateCheckFailed_) {
        software.note = "Check failed";
        software.noteTone = VoiceTone::Danger;
      } else if (canUpdate) {
        software.note = saved.latestAvailableVersion + " available";
        software.noteTone = VoiceTone::Warning;
      } else if (hasLastCheck) {
        software.note = "Up to date";
        software.noteTone = VoiceTone::Success;
      }
      software.targetId = "settings.updates.row";
      software.activate = [self, canUpdate] { canUpdate ? self->beginSoftwareUpdate() : self->beginUpdateChecks(); };
      if (canUpdate) {
        software.buttons.push_back(button("Update", "u", "settings.updates.update", [self] { self->beginSoftwareUpdate(); }, true));
      }
      software.buttons.push_back(button("Check", "c", "settings.updates.check", [self] { self->beginUpdateChecks(); }, false,
                                        false, !checking));
      rows.push_back(move(software));
      rows.push_back(toggleRow("", "Check every day", &AppSettings::updateChecksEnabled, "settings.updates.autocheck"));
      rows.push_back(infoRow("", "Last check", lastCheck, VoiceTone::Muted));

      auto firmware = infoRow("Scanner", "Firmware", deviceFirmwareVersion_.empty() ? "Not reported" : deviceFirmwareVersion_,
                              deviceFirmwareVersion_.empty() ? VoiceTone::Muted : VoiceTone::Plain);
      if (scanFirmwareFuture_.valid()) {
        firmware.note = "Checking...";
      } else if (scanFirmwareCheckFailed_) {
        firmware.note = "Check failed";
        firmware.noteTone = VoiceTone::Danger;
      } else if (!deviceFirmwareVersion_.empty() && isVersionNewer(scanFirmwareLatestVersion_, deviceFirmwareVersion_)) {
        firmware.note = scanFirmwareLatestVersion_ + " available";
        firmware.noteTone = VoiceTone::Warning;
      }
      rows.push_back(move(firmware));
      rows.push_back(infoRow("", "Hardware", "R1"));
      break;
    }

    case SettingsCategory::Printer: {
      const PrinterQueueInfo* configured = nullptr;
      for (const auto& queue : printerQueues_) {
        if (queue.name == draft.printerQueue) configured = &queue;
      }
      if (draft.printerQueue.empty()) {
        if (printerQueues_.empty()) {
          model.status = VoiceLine{{"No printer is set up, and none was found.", VoiceTone::Warning}};
        } else {
          model.status = VoiceLine{{"No printer is selected.", VoiceTone::Warning}};
        }
      } else if (configured == nullptr) {
        model.status = {{draft.printerQueue, VoiceTone::Strong}, {" was not found.", VoiceTone::Warning}};
      } else {
        model.status = {{draft.printerQueue, VoiceTone::Strong}, {" is " + toLower(queueStatusWord(configured->statusText)) + "."}};
      }

      auto queue = infoRow("Printing", "Print to", draft.printerQueue.empty() ? "None" : draft.printerQueue,
                           draft.printerQueue.empty() ? VoiceTone::Muted : VoiceTone::Strong);
      queue.targetId = "settings.printer.queue";
      queue.activate = [self] { self->cycleStagedPrinterQueue(); };
      queue.buttons = {button("Test print", "t", "settings.printer.test", [self] { self->testStagedPrinter(); }, false, false,
                              !draft.printerQueue.empty())};
      markChange(queue, draft.printerQueue != saved.printerQueue, saved.printerQueue.empty() ? "None" : saved.printerQueue);
      rows.push_back(move(queue));

      auto found = infoRow("", "Printers found", to_string(printerQueues_.size()));
      found.targetId = "settings.printer.found";
      found.activate = [self] { self->refreshPrinterState(); };
      found.buttons = {button("Refresh", "r", "settings.printer.refresh", [self] { self->refreshPrinterState(); })};
      rows.push_back(move(found));
      rows.push_back(toggleRow("", "Label after each scan", &AppSettings::autoPrintScannedLabels, "settings.printer.autolabel"));

      auto symbols = infoRow("Labels", "Rack symbols", symbolStandardLabel(draft.symbolStandard), VoiceTone::Strong);
      symbols.targetId = "settings.printer.symbols";
      symbols.activate = [self] { self->toggleSymbolStandard(); };
      markChange(symbols, draft.symbolStandard != saved.symbolStandard, symbolStandardLabel(saved.symbolStandard));
      rows.push_back(move(symbols));

      auto presets = infoRow("", "Quick labels",
                             to_string(draft.quickLabelPresets.size()) + " of " + to_string(kQuickLabelPresetLimit) + " presets");
      presets.targetId = "settings.printer.quick_labels";
      presets.activate = [self] { self->selectSettingsCategory(SettingsCategory::QuickLabels); };
      presets.buttons = {button("Edit", "", "settings.printer.quick_labels.edit",
                                [self] { self->selectSettingsCategory(SettingsCategory::QuickLabels); })};
      rows.push_back(move(presets));
      break;
    }

    case SettingsCategory::QuickLabels: {
      const auto& presets = draft.quickLabelPresets;
      const int count = static_cast<int>(presets.size());
      model.status = {{to_string(count) + " of " + to_string(kQuickLabelPresetLimit), VoiceTone::Strong},
                      {" presets in use."}};
      for (int index = 0; index < count; ++index) {
        auto row = editRow(index == 0 ? "Presets" : "", "Preset " + to_string(index + 1), presets[static_cast<size_t>(index)],
                           index, "settings.quick_label." + to_string(index));
        const bool changed = static_cast<size_t>(index) >= saved.quickLabelPresets.size() ||
                             saved.quickLabelPresets[static_cast<size_t>(index)] != presets[static_cast<size_t>(index)];
        if (!row.editing && changed) row.tone = VoiceTone::Warning;
        row.buttonsOnFocus = true;
        row.buttons = {button("Test", "t", "settings.quick_label.test", [self] { self->testQuickLabelPreset(); }),
                       button("Remove", "x", "settings.quick_label.remove", [self] { self->deleteQuickLabelPreset(); }, false, true),
                       button("Up", "[", "settings.quick_label.up", [self] { self->moveQuickLabelPreset(-1); }, false, false,
                              index > 0),
                       button("Down", "]", "settings.quick_label.down", [self] { self->moveQuickLabelPreset(1); }, false, false,
                              index + 1 < count)};
        rows.push_back(move(row));
      }
      const bool canAdd = presets.size() < kQuickLabelPresetLimit;
      auto add = infoRow(count == 0 ? "Presets" : "", "New preset",
                         canAdd ? to_string(kQuickLabelPresetLimit - presets.size()) + " free" : "All 12 in use", VoiceTone::Muted);
      add.targetId = "settings.quick_label.add.row";
      if (canAdd) add.activate = [self] { self->addQuickLabelPreset(); };
      add.buttons = {button("Add", "a", "settings.quick_label.add.primary", [self] { self->addQuickLabelPreset(); }, false,
                            false, canAdd)};
      rows.push_back(move(add));

      auto wire = editRow("Custom", "Wire label", wireLabelText_.empty() ? "Not set" : wireLabelText_, count,
                          "settings.quick_label.wire");
      if (!wire.editing && wireLabelText_.empty()) wire.tone = VoiceTone::Muted;
      wire.buttons = {button("Print", "w", "settings.quick_label.wire.custom",
                             [self] { self->printWireLabel(self->wireLabelText_); }, false, false, !wireLabelText_.empty())};
      rows.push_back(move(wire));
      break;
    }

    case SettingsCategory::InventatoryScan: {
      const bool setupComplete = inventatoryScanConfig_.setupComplete || !inventatoryScanConfig_.deviceId.empty();
      if (!setupComplete) {
        model.status = {{"The R1 is not set up yet."}};
        auto status = infoRow("Scanner", "Status", "Not set up", VoiceTone::Muted);
        status.targetId = "settings.scan.status";
        status.activate = [self] { self->openInventatoryScanSetup(); };
        status.buttons = {button("Begin setup", "b", "settings.scan.begin_setup", [self] { self->openInventatoryScanSetup(); }, true)};
        rows.push_back(move(status));
        break;
      }
      const auto now = time(nullptr);
      const bool hasIdentity = !inventatoryScanConfig_.deviceId.empty();
      const bool online = deviceLastSeen_ > 0 && now - deviceLastSeen_ <= 15;
      size_t waiting = 0;
      size_t failed = 0;
      for (const auto& record : deviceEventRecords_) {
        if (record.state == "failed") ++failed;
        else if (record.state == "received") ++waiting;
      }
      const auto lastContact = deviceLastSeen_ > 0 ? nowTimestampString(deviceLastSeen_) : string();
      if (!hasIdentity) {
        model.status = {{"Waiting for the R1 to report in."}};
      } else if (online) {
        model.status = {{"The R1 is online."}};
      } else {
        model.status = {{"The R1 is offline.", VoiceTone::Warning}};
        if (!lastContact.empty()) model.status.push_back({" Last contact " + lastContact + "."});
      }
      if (failed > 0) {
        model.status.push_back({" "});
        model.status.push_back({voiceCount(static_cast<int>(failed), "scan", "scans") + " could not be saved.", VoiceTone::Danger});
      }

      auto status = infoRow("Scanner", "Status", !hasIdentity ? "Waiting" : online ? "Online" : "Offline",
                            !hasIdentity ? VoiceTone::Muted : online ? VoiceTone::Success : VoiceTone::Warning);
      if (hasIdentity && deviceLastSeen_ > 0) status.note = "signal " + to_string(deviceRssi_) + " dBm";
      rows.push_back(move(status));
      auto device = infoRow("", "Device", hasIdentity ? inventatoryScanConfig_.deviceId : "Not reported yet",
                            hasIdentity ? VoiceTone::Plain : VoiceTone::Muted);
      device.targetId = "settings.scan.device";
      device.activate = [self] { self->openInventatoryScanSetup(); };
      device.buttons = {button("Pair new", "p", "settings.scan.pair", [self] { self->openInventatoryScanSetup(); })};
      rows.push_back(move(device));
      rows.push_back(infoRow("", "Firmware", deviceFirmwareVersion_.empty() ? "Not reported" : deviceFirmwareVersion_,
                             deviceFirmwareVersion_.empty() ? VoiceTone::Muted : VoiceTone::Plain));

      auto waitingRow = infoRow("Scans", "Waiting to save", to_string(waiting));
      waitingRow.targetId = "settings.scan.events";
      waitingRow.activate = [self] { self->refreshDeviceEventRecords(); };
      waitingRow.buttons = {button("Refresh", "v", "settings.scan.events.refresh", [self] { self->refreshDeviceEventRecords(); })};
      rows.push_back(move(waitingRow));
      auto failedRow = infoRow("", "Failed to save", to_string(failed), failed > 0 ? VoiceTone::Danger : VoiceTone::Plain);
      failedRow.targetId = "settings.scan.events.failed";
      if (failed > 0) failedRow.activate = [self] { self->retryFailedDeviceEvents(); };
      failedRow.buttons = {
          button("Retry", "y", "settings.scan.events.retry", [self] { self->retryFailedDeviceEvents(); }, false, false, failed > 0),
          button("Discard", "x", "settings.scan.events.discard", [self] { self->discardFailedDeviceEvents(); }, false, true,
                 failed > 0)};
      rows.push_back(move(failedRow));

      auto port = editRow("Service", "Port", to_string(draft.deviceServicePort), 0, "settings.scan.port");
      if (!port.editing) markChange(port, draft.deviceServicePort != saved.deviceServicePort, to_string(saved.deviceServicePort));
      port.buttons = {button("Restart", "h", "settings.scan.restart", [self] { self->restartDeviceService(); })};
      rows.push_back(move(port));

      auto token = infoRow("Security", "Access token", "Hidden", VoiceTone::Muted);
      token.buttons = {button("Copy", "t", "settings.scan.token.copy", [self] { self->copyInventatoryScanToken(); }),
                       button("Regenerate", "r", "settings.scan.token.regenerate",
                              [self] { self->regenerateInventatoryScanToken(); }, false, true)};
      rows.push_back(move(token));
      auto pairing = infoRow("", "Pairing", hasIdentity ? inventatoryScanConfig_.deviceId : "Set up", VoiceTone::Plain);
      pairing.buttons = {button("Forget", "c", "settings.scan.clear", [self] { self->clearInventatoryScanPairing(); }, false, true)};
      rows.push_back(move(pairing));
      break;
    }

    case SettingsCategory::DigiKey: {
      const bool configured = !trim(saved.digiKeyClientId).empty() && hasStoredDigiKeySecret_;
      if (!configured) {
        model.status = {{"DigiKey is not set up yet."}};
        auto status = infoRow("Account", "Status", "Not set up", VoiceTone::Muted);
        status.targetId = "settings.digikey.status";
        status.activate = [self] { self->openDigiKeySetup(); };
        status.buttons = {button("Begin setup", "b", "settings.digikey.begin_setup", [self] { self->openDigiKeySetup(); }, true)};
        rows.push_back(move(status));
        break;
      }
      const bool refreshRunning = !digiKeyRefreshQueue_.empty() || digiKeyRefreshFuture_.valid();
      if (refreshRunning) {
        model.status = {{"Refreshing DigiKey data: "},
                        {to_string(digiKeyRefreshCompleted_) + " of " + to_string(digiKeyRefreshTotal_), VoiceTone::Strong},
                        {" parts done."}};
      } else if (!digiKeyRefreshLastError_.empty()) {
        model.status = {{"The last DigiKey refresh failed: ", VoiceTone::Danger}, {digiKeyRefreshLastError_}};
      } else {
        model.status = {{"DigiKey is set up for "}, {draft.digiKeySite + ", " + draft.digiKeyCurrency, VoiceTone::Strong}, {"."}};
      }
      const bool hasSecret = stagedDigiKeySecretChanged_ ? !stagedDigiKeySecret_.empty() : hasStoredDigiKeySecret_;
      struct Field {
        const char* group;
        const char* label;
        string value;
        string savedValue;
      };
      const vector<Field> fields = {
          {"Account", "Client ID", draft.digiKeyClientId, saved.digiKeyClientId},
          {"", "Client secret", hasSecret ? "Hidden" : "Not set", hasSecret ? "Hidden" : "Not set"},
          {"", "Account ID", draft.digiKeyAccountId, saved.digiKeyAccountId},
          {"Region", "Site", draft.digiKeySite, saved.digiKeySite},
          {"", "Language", draft.digiKeyLanguage, saved.digiKeyLanguage},
          {"", "Currency", draft.digiKeyCurrency, saved.digiKeyCurrency},
      };
      for (size_t index = 0; index < fields.size(); ++index) {
        const auto& field = fields[index];
        auto row = editRow(field.group, field.label, field.value.empty() ? "Not set" : field.value, static_cast<int>(index),
                           "settings.digikey." + to_string(index));
        if (row.editing && index == 1) row.value = string(inputBuffer_.size(), '*') + "_";
        if (!row.editing && field.value.empty()) row.tone = VoiceTone::Muted;
        if (!row.editing && index == 1 && stagedDigiKeySecretChanged_) {
          row.tone = VoiceTone::Warning;
          row.note = "changed";
        } else if (!row.editing) {
          markChange(row, field.value != field.savedValue, field.savedValue.empty() ? "Not set" : field.savedValue);
        }
        if (index == 0) row.buttons = {button("Test", "t", "settings.digikey.test", [self] { self->testStagedDigiKey(); })};
        rows.push_back(move(row));
      }
      string refresh = "Not run yet";
      auto refreshTone = VoiceTone::Muted;
      if (refreshRunning) {
        refresh = to_string(digiKeyRefreshCompleted_) + " of " + to_string(digiKeyRefreshTotal_) + " done";
        refreshTone = VoiceTone::Plain;
      } else if (digiKeyRefreshTotal_ > 0) {
        refresh = to_string(digiKeyRefreshSucceeded_) + " updated, " + to_string(digiKeyRefreshFailed_) + " failed";
        refreshTone = digiKeyRefreshFailed_ > 0 ? VoiceTone::Warning : VoiceTone::Plain;
      }
      auto refreshRow = infoRow("Inventory", "Last refresh", refresh, refreshTone);
      const bool refreshEnabled = !refreshRunning && !settingsDirty_;
      refreshRow.targetId = "settings.digikey.refresh.row";
      if (refreshEnabled) refreshRow.activate = [self] { self->beginDigiKeyRefresh(); };
      refreshRow.buttons = {button("Refresh", "r", "settings.digikey.refresh", [self] { self->beginDigiKeyRefresh(); }, false,
                                   false, refreshEnabled)};
      rows.push_back(move(refreshRow));
      break;
    }
  }
  return model;
}

ftxui::Element App::renderSettingsUi() const {
  auto self = const_cast<App*>(this);
  const auto* active = ftxui::ScreenInteractive::Active();
  const int screenWidth = active != nullptr ? active->dimx() : 120;
  const int navWidth = screenWidth >= 110 ? 26 : 25;
  const int contentWidth = max(60, screenWidth - navWidth);
  const auto surface = uiSurfaceBg();
  const auto canvas = uiCanvasBg();

  // ---- navigation: the selected item shares the content surface, like a tab joined to its page ----
  const bool updateAvailable = isVersionNewer(settings_.latestAvailableVersion, softwareVersion());
  const bool scanSetUp = inventatoryScanConfig_.setupComplete || !inventatoryScanConfig_.deviceId.empty();
  size_t failedScans = 0;
  for (const auto& record : deviceEventRecords_) failedScans += record.state == "failed" ? 1 : 0;
  const bool digiKeySetUp = !trim(settings_.digiKeyClientId).empty() && hasStoredDigiKeySecret_;
  const auto navNote = [&](SettingsCategory category) -> pair<string, ftxui::Color> {
    if (settingsCategoryDirty(category)) return {"Unsaved", uiWarnColor()};
    switch (category) {
      case SettingsCategory::Updates:
        if (updateAvailable) return {"New", uiWarnColor()};
        break;
      case SettingsCategory::Printer:
        if (settings_.printerQueue.empty()) return {"Off", uiMutedText()};
        break;
      case SettingsCategory::InventatoryScan:
        if (!scanSetUp) return {"Off", uiMutedText()};
        if (failedScans > 0) return {"Error", uiDangerColor()};
        break;
      case SettingsCategory::DigiKey:
        if (!digiKeySetUp) return {"Off", uiMutedText()};
        break;
      default:
        break;
    }
    return {"", uiMutedText()};
  };

  ftxui::Elements nav;
  const auto groupLabel = [&](const string& text) { nav.push_back(styledText(" " + text, uiMutedText(), canvas)); };
  for (const auto& entry : settingsCategoryEntries()) {
    const auto category = static_cast<SettingsCategory>(entry.categoryIndex);
    if (entry.categoryIndex == 0) groupLabel("System");
    if (entry.categoryIndex == 3) {
      nav.push_back(ftxui::text(""));
      groupLabel("Devices");
    }
    if (entry.categoryIndex == 6) {
      nav.push_back(ftxui::text(""));
      groupLabel("Integrations");
    }
    const bool selected = category == settingsCategory_;
    const auto bg = selected ? surface : canvas;
    const auto note = navNote(category);
    auto row = ftxui::hbox({
                   styledText(selected ? " >" : "  ", uiFocusColor(), bg) | ftxui::bold,
                   styledText(string(1 + entry.indent * 2, ' '), uiSecondaryText(), bg),
                   selected ? uiHeaderText(settingsCategoryName(category), uiFocusColor(), bg)
                            : styledText(settingsCategoryName(category), uiSecondaryText(), bg),
                   ftxui::filler(),
                   styledText(note.first, note.second, bg),
                   ftxui::text(" "),
               }) |
               ftxui::bgcolor(bg);
    nav.push_back(target(move(row), "settings.category." + settingsCategoryName(category), UiTargetKind::Category,
                         [self, category] { self->selectSettingsCategory(category); }));
  }

  // ---- content ----
  const auto model = settingsPageModel();
  // The action column is as wide as the widest button group on the page, so every row's first
  // button starts at the same column.
  int actionWidth = 0;
  for (const auto& row : model.rows) {
    int width = 0;
    for (const auto& button : row.buttons) {
      width += (width > 0 ? 1 : 0) + static_cast<int>(button.label.size()) + 2 +
               (button.key.empty() ? 0 : static_cast<int>(button.key.size()) + 1);
    }
    actionWidth = max(actionWidth, width);
  }
  // Columns are sized from this page's own content: the longest group and label set their columns,
  // the widest button group sets the action column, and the value takes the rest.
  int longestGroup = 0;
  int longestLabel = 0;
  for (const auto& row : model.rows) {
    longestGroup = max(longestGroup, static_cast<int>(row.group.size()));
    longestLabel = max(longestLabel, static_cast<int>(row.label.size()));
  }
  const int markerWidth = 2;
  // On narrow terminals the group names move from the gutter to their own line above the group.
  const bool groupsInGutter = contentWidth >= 90;
  const int groupWidth = groupsInGutter ? clamp(longestGroup + 3, 10, 15) : 0;
  int labelWidth = clamp(longestLabel + 3, 14, 30);
  int valueWidth = contentWidth - markerWidth - groupWidth - labelWidth - actionWidth - 3;
  if (valueWidth < 16) {
    labelWidth = max(14, labelWidth - (16 - valueWidth));
    valueWidth = contentWidth - markerWidth - groupWidth - labelWidth - actionWidth - 3;
  }

  size_t cursor = settingsRow_;
  if (!model.rows.empty() && (cursor >= model.rows.size() || !model.rows[cursor].focusable())) {
    cursor = model.rows.size();
    for (size_t index = 0; index < model.rows.size(); ++index) {
      if (model.rows[index].focusable()) {
        cursor = index;
        break;
      }
    }
  }

  ftxui::Elements body;
  for (size_t index = 0; index < model.rows.size(); ++index) {
    const auto& row = model.rows[index];
    if (!row.group.empty() && index > 0) body.push_back(ftxui::text(""));
    if (!row.group.empty() && !groupsInGutter) {
      body.push_back(ftxui::hbox({ftxui::text(string(markerWidth, ' ')), styledText(row.group, uiMutedText())}));
    }
    const bool focused = index == cursor;
    const auto bg = focused ? uiSelectionBg() : surface;

    ftxui::Elements valueParts;
    int used = 0;
    if (row.swatch) {
      const auto rgb = *row.swatch;
      valueParts.push_back(styledText("  ", uiPrimaryText(),
                                      ftxui::Color::RGB(static_cast<uint8_t>((rgb >> 16) & 0xFFu),
                                                        static_cast<uint8_t>((rgb >> 8) & 0xFFu),
                                                        static_cast<uint8_t>(rgb & 0xFFu))));
      valueParts.push_back(ftxui::text(" "));
      used += 3;
    }
    // Paths keep both ends; everything else is cut at the end.
    const auto valueRoom = static_cast<size_t>(max(4, valueWidth - used - 1));
    const auto valueText = row.value.find('/') != string::npos ? middleEllipsize(row.value, valueRoom)
                                                               : ellipsize(row.value, valueRoom);
    const bool strong = row.tone == VoiceTone::Strong || row.tone == VoiceTone::Warning || row.tone == VoiceTone::Slot;
    valueParts.push_back(strong ? uiHeaderText(valueText, focused && row.tone == VoiceTone::Strong ? uiFocusColor() : toneColor(row.tone))
                                : styledText(valueText, toneColor(row.tone)));
    used += static_cast<int>(valueText.size());
    if (!row.note.empty() && used + 3 < valueWidth) {
      valueParts.push_back(styledText("  " + ellipsize(row.note, static_cast<size_t>(valueWidth - used - 3)), toneColor(row.noteTone)));
    }
    valueParts.push_back(ftxui::filler());

    auto left = ftxui::hbox({
                    styledText(focused ? ">" : " ", uiFocusColor()) | ftxui::bold |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, markerWidth),
                    styledText(groupsInGutter ? row.group : string(), uiMutedText()) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, groupWidth),
                    styledText(ellipsize(row.label, static_cast<size_t>(labelWidth - 1)), focused ? uiFocusColor() : uiSecondaryText()) |
                        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth),
                    ftxui::hbox(move(valueParts)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, valueWidth),
                }) |
                ftxui::bgcolor(bg);
    if (row.focusable() && !row.targetId.empty()) {
      left = target(move(left), row.targetId, UiTargetKind::Field, [self, index] {
        self->settingsRow_ = index;
        self->activateSettingsRow();
      });
    }

    ftxui::Elements buttons;
    if (!row.buttonsOnFocus || focused) {
      for (const auto& spec : row.buttons) {
        if (!buttons.empty()) buttons.push_back(ftxui::text(" "));
        const auto kind = spec.primary ? UiButtonKind::Primary : spec.danger ? UiButtonKind::Danger : UiButtonKind::Normal;
        buttons.push_back(target(uiButton(spec.label, spec.key, kind), spec.targetId, UiTargetKind::Button, spec.run,
                                 spec.enabled));
      }
    }
    buttons.push_back(ftxui::filler());
    auto line = ftxui::hbox({move(left), ftxui::text("  "),
                             ftxui::hbox(move(buttons)) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, actionWidth),
                             ftxui::filler()}) |
                ftxui::bgcolor(bg);
    if (focused) line = line | ftxui::select;
    body.push_back(move(line));
  }

  if (appearancePickerOpen_) {
    body.push_back(ftxui::text(""));
    for (auto& element : renderSettingsAppearancePicker()) body.push_back(move(element));
  }

  ftxui::Element right = nullptr;
  if (settingsDirty_) {
    right = ftxui::hbox({
        uiHeaderText("Unsaved changes", uiWarnColor()),
        ftxui::text("  "),
        target(uiButton("Save", "s", UiButtonKind::Primary), "settings.save", UiTargetKind::Button,
               [self] { self->saveSettingsDraft(); }),
        ftxui::text(" "),
        target(uiButton("Discard", "Esc"), "settings.cancel", UiTargetKind::Button, [self] { self->requestSettingsDiscard(); }),
    });
  }
  auto header = uiPageHeader(uiHeaderText(settingsCategoryName(settingsCategory_), uiPrimaryText()), uiVoiceLine(model.status, nullopt, contentWidth - 3),
                             move(right));

  return ftxui::hbox({
      ftxui::vbox({ftxui::vbox(move(nav)), ftxui::filler()}) | ftxui::bgcolor(canvas) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, navWidth),
      ftxui::vbox({move(header), ftxui::vbox(move(body)) | ftxui::yframe | ftxui::flex}) | ftxui::bgcolor(surface) |
          ftxui::flex,
  });
}

}  // namespace inventatory
