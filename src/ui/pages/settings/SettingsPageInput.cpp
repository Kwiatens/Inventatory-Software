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
#include <cstdint>
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

void App::toggleSymbolStandard() {
  settingsDraft_.symbolStandard =
      settingsDraft_.symbolStandard == SymbolStandard::Eu ? SymbolStandard::Us : SymbolStandard::Eu;
  settingsDirty_ = settingsDraftHasChanges();
  dirty_ = true;
}

void App::selectSettingsCategory(SettingsCategory category) {
  if (settingsEditingField_) {
    settingsEditingField_ = false;
    inputBuffer_.clear();
  }
  if (appearancePickerOpen_) closeAppearancePicker(false);
  settingsCategory_ = category;
  settingsRow_ = 0;
  settingsField_ = 0;
  if (category == SettingsCategory::Printer) refreshPrinterState();
  if (category == SettingsCategory::InventatoryScan) refreshDeviceEventRecords();
  // Land on the first row that can be acted on, and select what it stands for.
  moveSettingsRow(0);
  dirty_ = true;
}

void App::moveSettingsRow(int delta) {
  const auto model = settingsPageModel();
  if (model.rows.empty()) return;
  vector<size_t> focusable;
  for (size_t index = 0; index < model.rows.size(); ++index) {
    if (model.rows[index].focusable()) focusable.push_back(index);
  }
  if (focusable.empty()) return;
  auto current = find(focusable.begin(), focusable.end(), settingsRow_);
  long position = current == focusable.end() ? 0 : static_cast<long>(current - focusable.begin());
  position = clamp<long>(position + delta, 0, static_cast<long>(focusable.size()) - 1);
  settingsRow_ = focusable[static_cast<size_t>(position)];
  const auto& row = model.rows[settingsRow_];
  if (row.field >= 0) settingsField_ = row.field;
  dirty_ = true;
}

void App::activateSettingsRow() {
  const auto model = settingsPageModel();
  if (settingsRow_ >= model.rows.size()) return;
  const auto& row = model.rows[settingsRow_];
  if (row.field >= 0) settingsField_ = row.field;
  if (row.activate) {
    row.activate();
  } else {
    // A row without its own action runs its first available button.
    for (const auto& button : row.buttons) {
      if (button.enabled && button.run) {
        button.run();
        break;
      }
    }
  }
  dirty_ = true;
}

void App::requestSettingsDiscard() {
  if (!settingsDirty_) return;
  const auto now = time(nullptr);
  if (settingsConfirmAction_ != "discard-settings" || now > settingsConfirmUntil_) {
    settingsConfirmAction_ = "discard-settings";
    settingsConfirmUntil_ = now + 5;
    setMessage("Press Discard or Esc again to discard your unsaved settings changes", 5, UiMessageSeverity::Warning);
    return;
  }
  settingsConfirmAction_.clear();
  settingsConfirmUntil_ = 0;
  cancelSettingsDraft();
}

void App::cycleStagedPrinterQueue() {
  if (printerQueues_.empty()) {
    setMessage("No printers were found. Refresh after connecting one.", 4, UiMessageSeverity::Warning);
    return;
  }
  size_t next = 0;
  for (size_t index = 0; index < printerQueues_.size(); ++index) {
    if (printerQueues_[index].name == settingsDraft_.printerQueue) next = (index + 1) % printerQueues_.size();
  }
  printerSelection_ = next;
  stageSelectedPrinterQueue();
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
      appendKeyText(inputBuffer_, key);
      dirty_ = true;
    } else if (key.type == KeyType::Backspace) {
      if (!inputBuffer_.empty()) eraseLastCharacter(inputBuffer_);
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

  // Every accelerator is registered in currentActions(); only navigation lives here. Up/Down move the
  // row cursor, Left/Right move between categories, and Enter acts on the row. Moving never changes a
  // setting.
  const auto categoryCount = static_cast<int>(settingsCategoryEntries().size());
  if (key.type == KeyType::Up || (key.type == KeyType::Character && key.ch == 'k')) {
    moveSettingsRow(-1);
  } else if (key.type == KeyType::Down || (key.type == KeyType::Character && key.ch == 'j')) {
    moveSettingsRow(1);
  } else if (key.type == KeyType::PageUp || key.type == KeyType::Home) {
    moveSettingsRow(-1000);
  } else if (key.type == KeyType::PageDown || key.type == KeyType::End) {
    moveSettingsRow(1000);
  } else if (key.type == KeyType::Left) {
    selectSettingsCategory(static_cast<SettingsCategory>(max(0, static_cast<int>(settingsCategory_) - 1)));
  } else if (key.type == KeyType::Right) {
    selectSettingsCategory(static_cast<SettingsCategory>(min(categoryCount - 1, static_cast<int>(settingsCategory_) + 1)));
  } else if (key.type == KeyType::Enter) {
    activateSettingsRow();
  } else if (key.type == KeyType::Escape) {
    if (settingsDirty_) requestSettingsDiscard();
    else changePage(Page::Stock);
  }
}

}  // namespace inventatory
