// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Application shell rendering and foreground/background loops.

#include "App.h"

#include "platform/digikey/DigiKeyApi.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/StartupRegistration.h"
#include "platform/system/UpdateService.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <utility>
#include <thread>

namespace inventatory {

using namespace std;

namespace {


KeyEvent translateEvent(const ftxui::Event& event) {
  static const std::pair<ftxui::Event, KeyType> kKeys[] = {
      {ftxui::Event::Return, KeyType::Enter},
      {ftxui::Event::Escape, KeyType::Escape},
      {ftxui::Event::CtrlH, KeyType::CtrlBackspace},
      {ftxui::Event::CtrlZ, KeyType::CtrlZ},
      {ftxui::Event::Tab, KeyType::Tab},
      {ftxui::Event::TabReverse, KeyType::TabReverse},
      {ftxui::Event::ArrowUp, KeyType::Up},
      {ftxui::Event::ArrowDown, KeyType::Down},
      {ftxui::Event::ArrowLeft, KeyType::Left},
      {ftxui::Event::ArrowRight, KeyType::Right},
      {ftxui::Event::Home, KeyType::Home},
      {ftxui::Event::End, KeyType::End},
      {ftxui::Event::PageUp, KeyType::PageUp},
      {ftxui::Event::PageDown, KeyType::PageDown},
      {ftxui::Event::Delete, KeyType::Delete},
  };
  for (const auto& [key, type] : kKeys) {
    if (event == key) return {type, '\0', {}};
  }
  if (event == ftxui::Event::Backspace) {
    return {controlModifierPressed() ? KeyType::CtrlBackspace : KeyType::Backspace, '\0', {}};
  }
  if (event.is_character() && !event.character().empty()) {
    const auto& text = event.character();
    return {KeyType::Character, text.size() == 1 ? text[0] : '\0', text};
  }
  return {KeyType::Unknown, '\0', {}};
}

}  // namespace


int App::run() {
#if defined(_WIN32)
  // Windows Terminal does not advertise truecolor, so set it before any RGB color is built.
  // Otherwise FTXUI falls back to ANSI and collapses the palette.
  ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
#endif
  if (!startInBackground_) {
    initializeConsoleWindow();
#ifndef _WIN32
    if (settings_.completedOnboardingVersion >= 1) {
      string shortcutError;
      createDesktopShortcut(shortcutError);
    }
#endif
  }
  running_ = true;
  if (startInBackground_ && backgroundController_.interactiveInstanceRunning()) {
    running_ = false;
  }
  // Rewrite the startup entry only once a background choice is recorded, so a fresh
  // install never removes an existing one.
  if (settings_.backgroundConsentAsked || settings_.backgroundServiceEnabled) {
    string startupError;
    if (!setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, startupError)) {
      setMessage("Unable to update the background startup service: " + startupError, 5);
    }
  }
  // Windows sign-in launches with --background: no FTXUI, only the Scan R1 bridge and tray.
  const bool backgroundStarted = backgroundController_.start(
      startInBackground_ || settings_.backgroundServiceEnabled, startInBackground_, [this] {
        backgroundQuitRequested_.store(true);
      }, [this] { foregroundRequested_.store(true); });
  bool backgroundServiceUnavailable = false;
  if (!backgroundStarted && startInBackground_) {
    // A background process its controller cannot reach can never be told to quit, so it exits.
    // The saved preference stays: the failure may be transient (e.g. not ready at sign-in).
    cerr << "Inventatory background service could not start its controller; exiting.\n";
    running_ = false;
    backgroundServiceUnavailable = true;
  } else if (!backgroundStarted && settings_.backgroundServiceEnabled) {
    // The saved setting must not claim a background bridge that failed to start.
    settings_.backgroundServiceEnabled = false;
    settingsDraft_.backgroundServiceEnabled = false;
    string startupError;
    setBackgroundStartupEnabled(false, startupError);
    if (!saveAppSettings(settingsPath_, settings_)) {
      appSettingsSavePending_ = true;
    }
    setMessage("Background service unavailable; it was disabled", 7);
  }

  if (backgroundServiceUnavailable) {
    // Skip the loop; shutdown below still stops services and saves.
  } else if (startInBackground_) {
    runBackgroundLoop();
  } else {
    runInteractiveLoop();
  }

  // Stop producers before the final save, so no late callback mutates the store after the snapshot.
  // Drop parked device reports first.
  cancelPendingDeviceRequests();
  mdnsService_.stop();
  server_.stop();
  // Update work is cancelled rather than waited for, within the same deadline.
  const auto shutdownDeadline = chrono::steady_clock::now() + chrono::seconds(3);
  stopUpdateWorkUntil(shutdownDeadline);
  stopWorkspaceBoundWorkUntil(shutdownDeadline);
  const bool finalSaveSucceeded = saveState();
  if (!finalSaveSucceeded) {
    // No frame is left to show this, so report on stderr and keep the state until App destruction.
    cerr << "Inventatory shutdown save failed: "
         << (persistenceError_.empty() ? "unknown persistence error" : persistenceError_) << '\n';
  }
  backgroundController_.stop();
  if (finalSaveSucceeded && !updateInstallerLaunched_ && !startInBackground_ && settings_.backgroundServiceEnabled) {
    if (!backgroundController_.restartAsBackgroundService()) {
      settings_.backgroundServiceEnabled = false;
      settingsDraft_.backgroundServiceEnabled = false;
      string startupError;
      setBackgroundStartupEnabled(false, startupError);
      if (!saveAppSettings(settingsPath_, settings_)) {
        cerr << "Inventatory could not disable the unavailable background service preference: "
             << (startupError.empty() ? "settings save failed" : startupError) << '\n';
      }
      cerr << "Inventatory background service failed to start after shutdown" << '\n';
      return 1;
    }
  }
  return finalSaveSucceeded && !backgroundServiceUnavailable ? 0 : 1;
}

void App::processBackgroundWork() {
  if (inventoryRecoveryRequired_) {
    clearMessageIfExpired();
    return;
  }
  processDeviceRequests();
  processDeviceSyncEvents();
  clearMessageIfExpired();
  clearDeleteConfirmationIfExpired();
  processUpdateCheck();
  processSoftwareUpdate();
  processScanFirmwareCheck();
  processBleProvisioning();
  finishSupplierTest(digiKeyTestFuture_, "DigiKey");
  finishSupplierTest(tmeTestFuture_, "TME");
  finishSupplierTest(mouserTestFuture_, "Mouser");
  processScanDigiKeyEnrichment();
  processDigiKeyRefresh();
  processTmeRefresh();
  processMouserRefresh();
  processImportSync();
  processVendorLookup();
  processBomEnrichment();
  processPrinterWork();
}

void App::runBackgroundLoop() {
  while (running_) {
    if (backgroundController_.interactiveInstanceRunning()) {
      running_ = false;
      break;
    }
    if (backgroundQuitRequested_.exchange(false)) {
      running_ = false;
      break;
    }
    if (foregroundRequested_.exchange(false)) {
      runInteractiveLoop();
      continue;
    }
    processBackgroundWork();
    this_thread::sleep_for(chrono::milliseconds(100));
  }
}

void App::runInteractiveLoop() {
  initializeConsoleWindow();
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  screen.ForceHandleCtrlZ(false);
  screen.TrackMouse();
  auto renderer = ftxui::Renderer([this] { return renderUi(); });
  auto component = ftxui::CatchEvent(renderer, [this, &screen](ftxui::Event event) {
    if (event == ftxui::Event::Custom) {
      if (foregroundRequested_.exchange(false)) {
        // Another launch found this window open; ask the desktop to draw attention.
        requestTerminalAttention();
      }
      if (backgroundQuitRequested_.exchange(false)) {
        running_ = false;
        screen.ExitLoopClosure()();
        return true;
      }
      updateWizardTransition();
      processBackgroundWork();
      if (!running_) {
        screen.ExitLoopClosure()();
      }
      return true;
    }

    if (event.is_mouse()) {
      return handleMouse(event.mouse());
    }

    const auto key = translateEvent(event);
    if (key.type == KeyType::Unknown && event != ftxui::Event::Escape) {
      return false;
    }

    handleKey(key);
    if (!running_) {
      screen.ExitLoopClosure()();
    }
    return true;
  });

  thread ticker([this, &screen] {
    while (running_) {
      screen.PostEvent(ftxui::Event::Custom);
      // Tick faster only while the wordmark reveal animates.
      this_thread::sleep_for(chrono::milliseconds(wordmarkRevealRunning() ? 33 : 100));
    }
  });

  screen.Loop(component);
  running_ = false;
  if (ticker.joinable()) {
    ticker.join();
  }
}

}  // namespace inventatory
