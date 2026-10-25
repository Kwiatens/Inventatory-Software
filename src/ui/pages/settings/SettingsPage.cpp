// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Settings page: one row model per category, rendered on the shared page skeleton.

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "core/storage/InventorySqlite.h"
#include "core/vendor/VendorRateLimits.h"
#include "label_printer/symbols/RackSymbols.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;
using namespace settings_page_detail;

namespace {

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
  // CUPS status reads "idle.  enabled since ..."; only the first clause is the state.
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
    case SettingsCategory::Vendors:
      return draft.digiKeyEnabled != saved.digiKeyEnabled || draft.tmeEnabled != saved.tmeEnabled ||
             draft.mouserEnabled != saved.mouserEnabled ||
             sharedVendorCurrency(draft) != sharedVendorCurrency(saved) ||
             sharedVendorRegion(draft) != sharedVendorRegion(saved);
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
  // A supplier's request count against its published limits; red once the limit is reached.
  const auto usageRow = [&](Vendor vendor) {
    const auto snapshot = VendorRateLimits::instance().snapshot(vendor);
    auto tone = VoiceTone::Plain;
    if (snapshot.availableAt > 0) {
      tone = VoiceTone::Danger;
    } else if (any_of(snapshot.windows.begin(), snapshot.windows.end(), [](const WindowUsage& w) { return w.warn; })) {
      tone = VoiceTone::Warning;
    } else if (snapshot.requestsLastDay == 0) {
      tone = VoiceTone::Muted;
    }
    return infoRow("", "Usage", VendorRateLimits::usageSummary(snapshot), tone);
  };
  // A supplier's "Last refresh" row. counts describes a finished run; warn marks it as a warning.
  const auto refreshRow = [&](const auto& run, const string& id, bool enabled, function<void()> begin,
                              const string& counts, bool warn) {
    string refresh = "Not run yet";
    auto tone = VoiceTone::Muted;
    if (run.running()) {
      refresh = to_string(run.completed) + " of " + to_string(run.total) + " done";
      tone = VoiceTone::Plain;
    } else if (run.total > 0) {
      refresh = counts;
      tone = warn ? VoiceTone::Warning : VoiceTone::Plain;
    }
    auto row = infoRow("Inventory", "Last refresh", refresh, tone);
    row.targetId = "settings." + id + ".refresh.row";
    if (enabled) row.activate = begin;
    row.buttons = {button("Refresh", "r", "settings." + id + ".refresh", begin, false, false, enabled)};
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

      rows.push_back(toggleRow("Behaviour", "Keep running when closed", &AppSettings::backgroundServiceEnabled,
                               "settings.general.background"));
      auto threshold = editRow("", "Low stock at or below", to_string(draft.lowStockThreshold) + " pcs", 0,
                               "settings.general.low_stock_threshold");
      if (!threshold.editing) {
        markChange(threshold, draft.lowStockThreshold != saved.lowStockThreshold, to_string(saved.lowStockThreshold) + " pcs");
      }
      rows.push_back(move(threshold));
      rows.push_back(infoRow("Files", "Settings file", homeRelative(settingsPath_), VoiceTone::Muted));

      // Backup, restore and export are actions on the inventory, not settings, so they sit apart at the bottom.
      SettingsRow tools;
      tools.group = "Inventory data";
      tools.buttonsOnly = true;
      tools.targetId = "settings.data.tools";
      tools.buttons = {button("Back up", "k", "settings.data.backup", [self] { self->backupData(); }),
                       button("Export CSV", "x", "settings.data.export", [self] { self->exportInventory(); }),
                       button("Restore", "r", "settings.data.restore", [self] { self->restoreData(); }, false, true)};
      rows.push_back(move(tools));
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
                              !draft.printerQueue.empty()),
                       button("Refresh", "r", "settings.printer.refresh", [self] { self->refreshPrinterState(); })};
      markChange(queue, draft.printerQueue != saved.printerQueue, saved.printerQueue.empty() ? "None" : saved.printerQueue);
      rows.push_back(move(queue));

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
        SettingsRow setup;
        setup.buttonsOnly = true;
        setup.targetId = "settings.scan.status";
        setup.activate = [self] { self->openInventatoryScanSetup(); };
        setup.buttons = {button("Begin setup", "b", "settings.scan.begin_setup", [self] { self->openInventatoryScanSetup(); }, true)};
        rows.push_back(move(setup));
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
      status.targetId = "settings.scan.status.row";
      status.activate = [self] { self->openInventatoryScanSetup(); };
      status.buttons = {button("Pair new", "p", "settings.scan.pair", [self] { self->openInventatoryScanSetup(); }),
                        button("Forget", "c", "settings.scan.clear", [self] { self->clearInventatoryScanPairing(); }, false, true)};
      rows.push_back(move(status));
      auto device = infoRow("", "Device", hasIdentity ? inventatoryScanConfig_.deviceId : "Not reported yet",
                            hasIdentity ? VoiceTone::Plain : VoiceTone::Muted);
      if (!deviceFirmwareVersion_.empty()) device.note = "firmware " + deviceFirmwareVersion_;
      rows.push_back(move(device));

      // Scans that are not saved yet appear only while there are some.
      if (waiting > 0 || failed > 0) {
        auto scans = infoRow("", "Unsaved scans", to_string(waiting) + " waiting, " + to_string(failed) + " failed",
                             failed > 0 ? VoiceTone::Danger : VoiceTone::Plain);
        scans.targetId = "settings.scan.events";
        scans.activate = [self] { self->refreshDeviceEventRecords(); };
        scans.buttons = {
            button("Retry", "y", "settings.scan.events.retry", [self] { self->retryFailedDeviceEvents(); }, false, false, failed > 0),
            button("Discard", "x", "settings.scan.events.discard", [self] { self->discardFailedDeviceEvents(); }, false, true,
                   failed > 0)};
        rows.push_back(move(scans));
      }

      auto port = editRow("Service", "Port", to_string(draft.deviceServicePort), 0, "settings.scan.port");
      if (!port.editing) markChange(port, draft.deviceServicePort != saved.deviceServicePort, to_string(saved.deviceServicePort));
      port.buttons = {button("Restart", "h", "settings.scan.restart", [self] { self->restartDeviceService(); })};
      rows.push_back(move(port));

      auto token = infoRow("", "Access token", "Hidden", VoiceTone::Muted);
      token.buttons = {button("Copy", "t", "settings.scan.token.copy", [self] { self->copyInventatoryScanToken(); }),
                       button("Regenerate", "r", "settings.scan.token.regenerate",
                              [self] { self->regenerateInventatoryScanToken(); }, false, true)};
      rows.push_back(move(token));

      // The firmware package this app keeps and offers to the R1. Loading a file takes effect at once.
      const auto packageVersion = scanFirmware_.packageVersion();
      auto package = infoRow("Firmware", "Package", packageVersion.empty() ? "None" : packageVersion,
                             packageVersion.empty() ? VoiceTone::Muted : VoiceTone::Plain);
      package.targetId = "settings.scan.firmware";
      package.activate = [self] { self->loadScanFirmwareFile(); };
      package.buttons = {button("Load file", "f", "settings.scan.firmware.load", [self] { self->loadScanFirmwareFile(); })};
      rows.push_back(move(package));
      break;
    }

    case SettingsCategory::Vendors: {
      const auto currency = sharedVendorCurrency(draft);
      const auto savedCurrency = sharedVendorCurrency(saved);
      const auto region = sharedVendorRegion(draft);
      const auto savedRegion = sharedVendorRegion(saved);
      const bool digiKeyConfigured = !trim(saved.digiKeyClientId).empty() && hasStoredDigiKeySecret_;
      const bool tmeConfigured = hasStoredTmeToken_ && hasStoredTmeSecret_;
      // A MOUSER_API_KEY environment key also counts as set.
      const bool mouserConfigured = hasStoredMouserKey_ || mouserKeyFromEnvironment_;
      const auto working = [](bool enabled, bool configured) { return enabled && configured ? 1 : 0; };
      const int active = working(draft.digiKeyEnabled, digiKeyConfigured) + working(draft.tmeEnabled, tmeConfigured) +
                         working(draft.mouserEnabled, mouserConfigured);
      model.status = {{"Prices in "},
                      {currency.empty() ? "no currency yet" : currency, currency.empty() ? VoiceTone::Warning : VoiceTone::Strong},
                      {", region "},
                      {region.empty() ? "not set" : region, region.empty() ? VoiceTone::Warning : VoiceTone::Strong},
                      {". " + to_string(active) + " of 3 vendors on and set up."}};

      // One block per vendor: a switch, the keys with Edit and Test, and the request count.
      const auto vendorBlock = [&](const char* name, const string& id, Vendor vendor, int enabledField, int keyField,
                                   bool enabled, bool savedEnabled, bool configured,
                                   const function<void()>& edit, const function<void()>& test, bool testEnabled) {
        auto onRow = editRow(name, "Enabled", enabled ? "On" : "Off", enabledField, "settings." + id + ".enabled");
        onRow.tone = enabled ? VoiceTone::Strong : VoiceTone::Muted;
        onRow.activate = [self, enabledField] {
          auto& draft = self->settingsDraft_;
          switch (enabledField) {
            case kDigiKeyEnabledField: draft.digiKeyEnabled = !draft.digiKeyEnabled; break;
            case kTmeEnabledField: draft.tmeEnabled = !draft.tmeEnabled; break;
            default: draft.mouserEnabled = !draft.mouserEnabled; break;
          }
          self->settingsField_ = enabledField;
          self->settingsDirty_ = self->settingsDraftHasChanges();
          self->dirty_ = true;
        };
        markChange(onRow, enabled != savedEnabled, savedEnabled ? "On" : "Off");
        rows.push_back(move(onRow));

        const bool shown = revealedVendorField_ == keyField;
        auto keyRow = infoRow("", "API key", configured ? (shown ? "Shown" : "Hidden") : "Not set",
                              !configured ? VoiceTone::Warning : enabled ? VoiceTone::Plain : VoiceTone::Muted);
        keyRow.field = keyField;
        keyRow.targetId = "settings." + id + ".key";
        // Enter or a click on the row shows the keys after a confirmation; Edit changes them.
        if (configured) keyRow.activate = [self, keyField] { self->requestVendorKeyReveal(keyField); };
        else keyRow.activate = edit;
        if (configured) {
          keyRow.buttons = {button(shown ? "Hide" : "Show", "v", "settings." + id + ".show",
                                   [self, keyField] { self->requestVendorKeyReveal(keyField); }),
                            button("Edit", "e", "settings." + id + ".edit", edit),
                            button("Test", "t", "settings." + id + ".test", test, false, false, enabled && testEnabled)};
        } else {
          keyRow.buttons = {button("Set up", "e", "settings." + id + ".edit", edit, true)};
        }
        rows.push_back(move(keyRow));
        if (shown) {
          for (const auto& [label, value] : revealedVendorValues_) rows.push_back(infoRow("", label, value, VoiceTone::Strong));
        }

        auto usage = usageRow(vendor);
        if (!enabled) usage.tone = VoiceTone::Muted;
        rows.push_back(move(usage));
      };

      vendorBlock("DigiKey", "digikey", Vendor::DigiKey, kDigiKeyEnabledField, kDigiKeyKeyField, draft.digiKeyEnabled,
                  saved.digiKeyEnabled, digiKeyConfigured,
                  [self] { self->openVendorSetup(VendorSetupVendor::DigiKey); }, [self] { self->testStagedDigiKey(); },
                  true);
      vendorBlock("TME", "tme", Vendor::Tme, kTmeEnabledField, kTmeKeyField, draft.tmeEnabled, saved.tmeEnabled,
                  tmeConfigured,
                  [self] { self->openVendorSetup(VendorSetupVendor::Tme); }, [self] { self->testStagedTme(); },
                  !tmeRefresh_.running());
      vendorBlock("Mouser", "mouser", Vendor::Mouser, kMouserEnabledField, kMouserKeyField, draft.mouserEnabled,
                  saved.mouserEnabled, mouserConfigured,
                  [self] { self->openVendorSetup(VendorSetupVendor::Mouser); }, [self] { self->testStagedMouser(); },
                  !mouserRefresh_.running());

      auto currencyRow = editRow("All vendors", "Currency", currency.empty() ? "Not set" : currency, kCurrencyField,
                                 "settings.vendors.currency");
      if (!currencyRow.editing) {
        if (currency.empty()) currencyRow.tone = VoiceTone::Warning;
        markChange(currencyRow, currency != savedCurrency, savedCurrency.empty() ? "Not set" : savedCurrency);
      }
      rows.push_back(move(currencyRow));

      auto regionRow = editRow("", "Region", region.empty() ? "Not set" : region, kRegionField, "settings.vendors.region");
      if (!regionRow.editing) {
        if (region.empty()) {
          regionRow.tone = VoiceTone::Warning;
        }
        markChange(regionRow, region != savedRegion, savedRegion.empty() ? "Not set" : savedRegion);
      }
      rows.push_back(move(regionRow));

      // Part data comes from every vendor that is on; each part is refreshed at its own vendor.
      const bool anyRunning = digiKeyRefresh_.running() || tmeRefresh_.running() || mouserRefresh_.running();
      const bool anyReady = (draft.digiKeyEnabled && digiKeyConfigured) || (draft.tmeEnabled && tmeConfigured) ||
                            (draft.mouserEnabled && mouserConfigured);
      vector<string> parts;
      bool warn = false;
      const auto addPart = [&](const char* name, const auto& run, const string& counts) {
        if (run.running()) {
          parts.push_back(string(name) + " " + to_string(run.completed) + " of " + to_string(run.total));
        } else if (run.total > 0) {
          parts.push_back(string(name) + " " + counts);
        }
      };
      addPart("DigiKey", digiKeyRefresh_, to_string(digiKeyRefresh_.succeeded) + " updated, " + to_string(digiKeyRefresh_.errors) + " failed");
      addPart("TME", tmeRefresh_, catalogueRefreshCounts(tmeRefresh_));
      addPart("Mouser", mouserRefresh_, catalogueRefreshCounts(mouserRefresh_));
      warn = digiKeyRefresh_.errors > 0 || tmeRefresh_.ambiguous + tmeRefresh_.errors > 0 ||
             mouserRefresh_.ambiguous + mouserRefresh_.errors > 0;
      string summary = "Not run yet";
      if (!parts.empty()) {
        summary.clear();
        for (const auto& part : parts) summary += (summary.empty() ? "" : "; ") + part;
      }
      auto refreshRowAll = infoRow("", "Part data", summary,
                                   anyRunning ? VoiceTone::Plain : parts.empty() ? VoiceTone::Muted
                                              : warn                              ? VoiceTone::Warning
                                                                                  : VoiceTone::Plain);
      const bool canRefresh = anyReady && !anyRunning && !settingsDirty_;
      refreshRowAll.field = kRefreshField;
      refreshRowAll.targetId = "settings.vendors.refresh.row";
      if (canRefresh) refreshRowAll.activate = [self] { self->beginAllVendorRefresh(); };
      refreshRowAll.buttons = {button("Refresh all", "r", "settings.vendors.refresh", [self] { self->beginAllVendorRefresh(); },
                                      false, false, canRefresh)};
      rows.push_back(move(refreshRowAll));
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

  // The selected item shares the content surface, like a tab joined to its page.
  const bool updateAvailable = isVersionNewer(settings_.latestAvailableVersion, softwareVersion());
  const bool scanSetUp = inventatoryScanConfig_.setupComplete || !inventatoryScanConfig_.deviceId.empty();
  size_t failedScans = 0;
  for (const auto& record : deviceEventRecords_) failedScans += record.state == "failed" ? 1 : 0;
  const bool digiKeySetUp = !trim(settings_.digiKeyClientId).empty() && hasStoredDigiKeySecret_;
  const bool tmeSetUp = hasStoredTmeToken_ && hasStoredTmeSecret_;
  const bool mouserSetUp = hasStoredMouserKey_ || mouserKeyFromEnvironment_;
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
      case SettingsCategory::Vendors:
        if (!digiKeySetUp && !tmeSetUp && !mouserSetUp) return {"Off", uiMutedText()};
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

  const auto model = settingsPageModel();
  // Size the action column to the widest button group so the buttons align.
  int actionWidth = 0;
  for (const auto& row : model.rows) {
    if (row.buttonsOnly) continue;
    int width = 0;
    for (const auto& button : row.buttons) {
      width += (width > 0 ? 1 : 0) + static_cast<int>(button.label.size()) + 2 +
               (button.key.empty() ? 0 : static_cast<int>(button.key.size()) + 1);
    }
    actionWidth = max(actionWidth, width);
  }
  // Columns size to this page's content; the value column takes the rest.
  int longestGroup = 0;
  int longestLabel = 0;
  for (const auto& row : model.rows) {
    longestGroup = max(longestGroup, static_cast<int>(row.group.size()));
    longestLabel = max(longestLabel, static_cast<int>(row.label.size()));
  }
  const int markerWidth = 2;
  // Narrow terminals put group names on their own line above the group.
  const bool groupsInGutter = contentWidth >= 90;
  const int groupWidth = groupsInGutter ? clamp(longestGroup + 3, 10, 15) : 0;
  int labelWidth = clamp(longestLabel + 3, 14, 30);
  int valueWidth = contentWidth - markerWidth - groupWidth - labelWidth - actionWidth - 3;
  if (valueWidth < 16) {
    labelWidth = max(14, labelWidth - (16 - valueWidth));
    valueWidth = contentWidth - markerWidth - groupWidth - labelWidth - actionWidth - 3;
  }

  // On the Vendors page the buttons stay next to the values instead of at the far edge.
  if (settingsCategory_ == SettingsCategory::Vendors) valueWidth = min(valueWidth, 60);

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

    if (row.buttonsOnly) {
      // Buttons-only row: no label or value.
      auto marker = styledText(focused ? ">" : " ", uiFocusColor()) | ftxui::bold |
                    ftxui::size(ftxui::WIDTH, ftxui::EQUAL, markerWidth);
      if (row.focusable() && !row.targetId.empty()) {
        marker = target(move(marker), row.targetId, UiTargetKind::Field, [self, index] {
          self->settingsRow_ = index;
          self->activateSettingsRow();
        });
      }
      ftxui::Elements buttons;
      for (const auto& spec : row.buttons) {
        if (!buttons.empty()) buttons.push_back(ftxui::text(" "));
        const auto kind = spec.primary ? UiButtonKind::Primary : spec.danger ? UiButtonKind::Danger : UiButtonKind::Normal;
        buttons.push_back(target(uiButton(spec.label, spec.key, kind), spec.targetId, UiTargetKind::Button, spec.run, spec.enabled));
      }
      auto line = ftxui::hbox({move(marker), ftxui::hbox(move(buttons)), ftxui::filler()}) | ftxui::bgcolor(bg);
      if (focused) line = line | ftxui::select;
      body.push_back(move(line));
      continue;
    }

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
    // Paths keep their file name; other values are cut at the end.
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
