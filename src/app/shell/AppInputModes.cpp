// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Modal input handlers.

#include "App.h"

#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/StartupRegistration.h"
#include "platform/system/UpdateService.h"
#include "ui/shared/AppUiShared.h"
#include "ui/shared/NumericPrompt.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <utility>

namespace inventatory {

using namespace std;

void App::handleSearchKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    appendKeyText(inputBuffer_, key);
    searchQuery_ = inputBuffer_;
    syncSelectionToFilter();
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      eraseLastCharacter(inputBuffer_);
      searchQuery_ = inputBuffer_;
      syncSelectionToFilter();
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    inputMode_ = InputMode::None;
    syncSelectionToFilter();
    setMessage(searchQuery_.empty() ? "Filter cleared" : "Filter kept", 2);
    return;
  }

  if (key.type == KeyType::Escape) {
    searchQuery_ = searchQueryBeforeEdit_;
    inputBuffer_ = searchQuery_;
    inputMode_ = InputMode::None;
    syncSelectionToFilter();
    setMessage("Search cancelled", 2);
  }
}

void App::handleRackValueKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    appendKeyText(inputBuffer_, key);
    dirty_ = true;
    return;
  }

  if (key.type == KeyType::Backspace) {
    if (!inputBuffer_.empty()) {
      eraseLastCharacter(inputBuffer_);
      dirty_ = true;
    }
    return;
  }

  if (key.type == KeyType::Enter) {
    const auto value = inputBuffer_;
    const auto mode = inputMode_;
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    switch (mode) {
      case InputMode::RackRename:
        renameSelectedRack(value);
        break;
      case InputMode::RackType:
        changeSelectedRackType(value);
        break;
      case InputMode::RackCreate:
        createRackWithType(value);
        break;
      case InputMode::OrganizerCreate:
        createOrganizerFromInput(value);
        break;
      case InputMode::OrganizerType:
        createPendingOrganizer(value);
        break;
      case InputMode::RackJump:
        jumpToRack(value);
        break;
      case InputMode::RackFilter:
        rackFilter_ = trim(value);
        syncRackSelection();
        setMessage(rackFilter_.empty() ? "Rack filter cleared" : "Rack filter applied", 2);
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Rack input cancelled", 2);
  }
}

void App::handleQuantityAdjustKey(const KeyEvent& key) {
  if (key.type == KeyType::Character) {
    if (numeric_prompt::applyDigit(inputBuffer_, key.ch, inputReplaceOnType_, 9U)) dirty_ = true;
    return;
  }
  if (key.type == KeyType::Backspace) {
    numeric_prompt::applyBackspace(inputBuffer_, inputReplaceOnType_);
    dirty_ = true;
    return;
  }
  if (key.type == KeyType::Enter) {
    const auto value = inputBuffer_;
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setSelectedQuantityFromInput(value);
    return;
  }
  if (key.type == KeyType::Escape) {
    inputBuffer_.clear();
    inputMode_ = InputMode::None;
    setMessage("Quantity change cancelled", 2);
  }
}

void App::handleExitConfirmationKey(const KeyEvent& key) {
  if (key.type == KeyType::Escape) {
    pendingPageAfterSettings_.reset();
    exitSavePending_ = false;
    inputMode_ = InputMode::None;
    setMessage("Stayed on the current screen", 2);
    return;
  }
  if (key.type != KeyType::Character) return;
  const auto ch = static_cast<char>(tolower(static_cast<unsigned char>(key.ch)));
  if (exitSavePending_) {
    if (ch == 's') {
      if (saveState()) {
        exitSavePending_ = false;
        inputMode_ = InputMode::None;
        requestUserExit();
      }
    } else if (ch == 'd') {
      exitSavePending_ = false;
      inputMode_ = InputMode::None;
      setMessage("Exiting with unsaved data; retry persistence from the previous session", 8);
      running_ = false;
    }
    return;
  }
  if (ch == 's') {
    completeSettingsExit(true);
  } else if (ch == 'd') {
    completeSettingsExit(false);
  }
}

}  // namespace inventatory
