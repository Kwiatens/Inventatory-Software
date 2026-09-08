// Inventatory - Settings page-local input routing.

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "platform/security/CredentialStore.h"
#include "platform/digikey/DigiKeyApi.h"
#include "platform/system/StartupRegistration.h"
#include "core/storage/InventorySqlite.h"
#include "ui/shared/AppUiShared.h"

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

void App::stageSelectedPrinterQueue() {
  if (stagePrinterQueueSelection(printerQueues_, printerSelection_, settingsDraft_.printerQueue, settingsDirty_)) {
    dirty_ = true;
  }
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
      const auto previousSelection = printerSelection_;
      if (ch == 'j' && printerSelection_ + 1 < printerQueues_.size()) ++printerSelection_;
      if (ch == 'k' && printerSelection_ > 0) --printerSelection_;
      if (printerSelection_ != previousSelection) stageSelectedPrinterQueue();
    }
    return;
  }

  if (key.type == KeyType::Left) {
    settingsCategory_ = static_cast<SettingsCategory>(max(0, static_cast<int>(settingsCategory_) - 1));
    settingsField_ = 0;
    appearancePickerOpen_ = false;
    dirty_ = true;
  } else if (key.type == KeyType::Right) {
    settingsCategory_ = static_cast<SettingsCategory>(min(6, static_cast<int>(settingsCategory_) + 1));
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
    settingsCategory_ = static_cast<SettingsCategory>(min(6, static_cast<int>(settingsCategory_) + 1));
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
