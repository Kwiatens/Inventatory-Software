// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - credentials form for DigiKey, TME and Mouser.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <string>
#include <vector>

namespace inventatory {

using namespace std;

namespace {

struct SetupField {
  const char* label;
  const char* prompt;
  bool secret;
};

struct SetupVendorInfo {
  const char* name;
  const char* intro;
  vector<SetupField> fields;
};

// The index is the App::VendorSetupVendor value: DigiKey, TME, Mouser.
const SetupVendorInfo& setupInfo(int vendor) {
  static const SetupVendorInfo digiKey = {
      "DigiKey",
      "Create an application in the DigiKey developer portal, then copy its Client ID and Client secret.",
      {{"Client ID", "Paste the Client ID issued by DigiKey.", false},
       {"Client secret", "Paste the Client secret issued by DigiKey.", true},
       {"Account ID (optional)", "Your DigiKey account ID. Leave it empty if you have none.", false}}};
  static const SetupVendorInfo tme = {
      "TME",
      "Create an application in the TME API portal, then copy its token and application secret.",
      {{"Token", "Paste the anonymous or private token issued by TME.", true},
       {"Application secret", "Paste the application secret issued by TME.", true}}};
  static const SetupVendorInfo mouser = {
      "Mouser",
      "Request a Search API key in your Mouser account, then copy it here.",
      {{"API key", "Paste the Search API key issued by Mouser.", true}}};
  return vendor == 1 ? tme : vendor == 2 ? mouser : digiKey;
}

}  // namespace

void App::clearStagedVendorCredentials() {
  stagedDigiKeySecret_.assign(stagedDigiKeySecret_.size(), '\0');
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  clearStagedTmeCredentials();
  clearStagedMouserCredentials();
  inputBuffer_.assign(inputBuffer_.size(), '\0');
  inputBuffer_.clear();
}

void App::openVendorSetup(VendorSetupVendor vendor) {
  vendorSetupVendor_ = vendor;
  vendorSetupStep_ = 0;
  // Navigate first: with staged Settings edits, changePage opens the save/discard prompt, and the
  // draft must survive it.
  changePage(Page::VendorSetup);
  if (page_ != Page::VendorSetup) return;
  settingsDraft_ = settings_;
  settingsDirty_ = false;
  settingsEditingField_ = false;
  clearStagedVendorCredentials();
  setMessage(string(setupInfo(static_cast<int>(vendor)).name) + " setup started", 3);
}

ftxui::Element App::renderVendorSetupUi() const {
  const auto& info = setupInfo(static_cast<int>(vendorSetupVendor_));
  const int fieldCount = static_cast<int>(info.fields.size());
  const string name = info.name;
  ftxui::Elements rows;
  rows.push_back(uiHeaderText("Set up " + name, uiTitleColor()));
  rows.push_back(ftxui::text(""));
  if (vendorSetupStep_ == 0) {
    rows.push_back(styledText(info.intro, uiTitleColor()));
#ifdef _WIN32
    rows.push_back(styledText("Secrets are stored in Windows Credential Manager and never in settings.conf.", uiSecondaryText()));
#else
    rows.push_back(styledText("Secrets are stored in the system keyring and never in settings.conf.", uiSecondaryText()));
#endif
    rows.push_back(ftxui::text(""));
    rows.push_back(styledText("> Press Enter to begin  |  Esc to cancel", uiInteractiveColor()));
  } else if (vendorSetupStep_ <= fieldCount) {
    const auto& field = info.fields[static_cast<size_t>(vendorSetupStep_ - 1)];
    rows.push_back(styledText("Step " + to_string(vendorSetupStep_) + " of " + to_string(fieldCount), uiMutedText()));
    rows.push_back(styledText(field.prompt, uiTitleColor()));
    rows.push_back(ftxui::text(""));
    const string shown = field.secret ? string(inputBuffer_.size(), '*') : inputBuffer_;
    rows.push_back(styledText(string(field.label) + "> " + shown + "_", uiInteractiveColor()));
    rows.push_back(ftxui::text(""));
    rows.push_back(styledText("Enter continues  |  Esc cancels", uiMutedText()));
  } else {
    rows.push_back(styledText("Check what will be saved.", uiTitleColor()));
    rows.push_back(ftxui::text(""));
    for (size_t index = 0; index < info.fields.size(); ++index) {
      const auto& field = info.fields[index];
      string value = "********";
      if (!field.secret) {
        value = vendorSetupVendor_ == VendorSetupVendor::DigiKey && index == 0 ? settingsDraft_.digiKeyClientId
                                                                                : settingsDraft_.digiKeyAccountId;
        if (value.empty()) value = "Not set";
      }
      rows.push_back(styledText(string(field.label) + ": " + value, uiTitleColor()));
    }
    rows.push_back(ftxui::text(""));
    rows.push_back(styledText("> Press Enter to save and turn " + name + " on", uiInteractiveColor()));
    rows.push_back(styledText("  Esc cancels without changing " + name + " settings.", uiMutedText()));
  }
  auto body = ftxui::vbox(move(rows)) | ftxui::bgcolor(uiPanelLeftBg()) | ftxui::flex;
  return ftxui::window(ftxui::text(""), body) | ftxui::bgcolor(uiCanvasBg());
}

void App::handleVendorSetupKey(const KeyEvent& key) {
  const auto vendor = vendorSetupVendor_;
  const auto& info = setupInfo(static_cast<int>(vendor));
  const int fieldCount = static_cast<int>(info.fields.size());
  const auto leave = [this, vendor] {
    settingsCategory_ = SettingsCategory::Vendors;
    changePage(Page::Settings);
    focusVendorRow(vendor);
  };

  if (key.type == KeyType::Escape) {
    settingsDraft_ = settings_;
    settingsDirty_ = false;
    settingsEditingField_ = false;
    clearStagedVendorCredentials();
    leave();
    setMessage(string(info.name) + " setup cancelled", 3);
    return;
  }

  const bool typing = vendorSetupStep_ >= 1 && vendorSetupStep_ <= fieldCount;
  if (typing) {
    if (key.type == KeyType::Character) {
      appendKeyText(inputBuffer_, key);
      dirty_ = true;
      return;
    }
    if (key.type == KeyType::Backspace) {
      if (!inputBuffer_.empty()) eraseLastCharacter(inputBuffer_);
      dirty_ = true;
      return;
    }
  }
  if (key.type != KeyType::Enter) return;

  if (vendorSetupStep_ == 0) {
    vendorSetupStep_ = 1;
  } else if (typing) {
    if (!commitVendorCredentialField(vendor, vendorSetupStep_ - 1)) return;  // Rejected: stay on the step
    inputBuffer_.assign(inputBuffer_.size(), '\0');
    inputBuffer_.clear();
    settingsDirty_ = true;
    ++vendorSetupStep_;
  } else {
    if (vendor == VendorSetupVendor::DigiKey) settingsDraft_.digiKeyEnabled = true;
    if (vendor == VendorSetupVendor::Tme) settingsDraft_.tmeEnabled = true;
    if (vendor == VendorSetupVendor::Mouser) settingsDraft_.mouserEnabled = true;
    if (!saveSettingsDraft()) return;
    leave();
    setMessage(string(info.name) + " setup saved", 4);
    return;
  }
  // Prefill the next plain field with what is stored; secrets always start empty.
  if (vendorSetupStep_ >= 1 && vendorSetupStep_ <= fieldCount && !info.fields[static_cast<size_t>(vendorSetupStep_ - 1)].secret) {
    inputBuffer_ = vendor == VendorSetupVendor::DigiKey && vendorSetupStep_ == 1 ? settingsDraft_.digiKeyClientId
                                                                                  : settingsDraft_.digiKeyAccountId;
  } else {
    inputBuffer_.clear();
  }
  dirty_ = true;
}

}  // namespace inventatory
