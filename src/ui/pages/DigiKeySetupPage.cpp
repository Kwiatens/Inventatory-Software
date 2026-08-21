// Inventatory - Hardware Inventory Management System
// Guided DigiKey credential setup.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <string>

namespace inventatory {

using namespace std;

void App::openDigiKeySetup() {
  settingsDraft_ = settings_;
  settingsDirty_ = false;
  settingsEditingField_ = false;
  stagedDigiKeySecret_.clear();
  stagedDigiKeySecretChanged_ = false;
  inputBuffer_.clear();
  digiKeySetupStep_ = DigiKeySetupStep::Introduction;
  changePage(Page::DigiKeySetup);
  setMessage("DigiKey setup started", 3);
}

ftxui::Element App::renderDigiKeySetupUi() const {
  ftxui::Elements rows;
  switch (digiKeySetupStep_) {
    case DigiKeySetupStep::Introduction:
      rows.push_back(styledText("Enter the DigiKey API credentials for Inventatory" + uiAnimatedEllipsis(), uiTitleColor()));
      rows.push_back(styledText("Create or copy the Client ID and Client secret from the DigiKey developer portal.",
                                uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("> Press Enter to begin  |  Esc to cancel", uiInteractiveColor()));
      break;
    case DigiKeySetupStep::ClientId:
      rows.push_back(styledText("Paste the Client ID issued by DigiKey.", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("client-id> " + inputBuffer_ + "_", uiInteractiveColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Enter continues  |  Esc cancels", uiMutedText()));
      break;
    case DigiKeySetupStep::ClientSecret:
      rows.push_back(styledText("The secret is stored in Windows Credential Manager and never in settings.conf.",
                                uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("client-secret> " + string(inputBuffer_.size(), '*') + "_", uiInteractiveColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("Enter continues  |  Esc cancels", uiMutedText()));
      break;
    case DigiKeySetupStep::Review:
      rows.push_back(styledText("Client ID: " + settingsDraft_.digiKeyClientId, uiTitleColor()));
      rows.push_back(styledText("Client secret: ********", uiTitleColor()));
      rows.push_back(ftxui::text(""));
      rows.push_back(styledText("> Press Enter to save credentials", uiInteractiveColor()));
      rows.push_back(styledText("  Esc cancels without changing DigiKey settings.", uiMutedText()));
      break;
  }

  auto body = ftxui::vbox(move(rows)) | ftxui::bgcolor(uiPanelLeftBg()) | ftxui::flex;
  return ftxui::window(ftxui::text(""), body) | ftxui::bgcolor(uiCanvasBg());
}

void App::handleDigiKeySetupKey(const KeyEvent& key) {
  const auto cancel = [this] {
    settingsDraft_ = settings_;
    settingsDirty_ = false;
    settingsEditingField_ = false;
    stagedDigiKeySecret_.clear();
    stagedDigiKeySecretChanged_ = false;
    inputBuffer_.clear();
    settingsCategory_ = SettingsCategory::DigiKey;
    changePage(Page::Settings);
    setMessage("DigiKey setup cancelled", 3);
  };

  if (key.type == KeyType::Escape) {
    cancel();
    return;
  }

  if (digiKeySetupStep_ == DigiKeySetupStep::ClientId ||
      digiKeySetupStep_ == DigiKeySetupStep::ClientSecret) {
    if (key.type == KeyType::Character) {
      inputBuffer_.push_back(key.ch);
      dirty_ = true;
      return;
    }
    if (key.type == KeyType::Backspace) {
      if (!inputBuffer_.empty()) inputBuffer_.pop_back();
      dirty_ = true;
      return;
    }
  }

  if (key.type != KeyType::Enter) return;

  switch (digiKeySetupStep_) {
    case DigiKeySetupStep::Introduction:
      inputBuffer_ = settingsDraft_.digiKeyClientId;
      digiKeySetupStep_ = DigiKeySetupStep::ClientId;
      break;
    case DigiKeySetupStep::ClientId: {
      const auto clientId = trim(inputBuffer_);
      if (clientId.empty()) {
        setMessage("Enter a DigiKey Client ID", 3);
        return;
      }
      settingsDraft_.digiKeyClientId = clientId;
      inputBuffer_.clear();
      settingsDirty_ = true;
      digiKeySetupStep_ = DigiKeySetupStep::ClientSecret;
      break;
    }
    case DigiKeySetupStep::ClientSecret:
      if (trim(inputBuffer_).empty()) {
        setMessage("Enter a DigiKey Client secret", 3);
        return;
      }
      stagedDigiKeySecret_ = inputBuffer_;
      stagedDigiKeySecretChanged_ = true;
      inputBuffer_.clear();
      settingsDirty_ = true;
      digiKeySetupStep_ = DigiKeySetupStep::Review;
      break;
    case DigiKeySetupStep::Review:
      if (!saveSettingsDraft()) return;
      settingsCategory_ = SettingsCategory::DigiKey;
      changePage(Page::Settings);
      setMessage("DigiKey setup saved", 4);
      break;
  }
  dirty_ = true;
}

}  // namespace inventatory
