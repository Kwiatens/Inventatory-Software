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
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <thread>
#include <future>

namespace inventatory {

using namespace std;

namespace {


KeyEvent translateEvent(const ftxui::Event& event) {
  if (event == ftxui::Event::Return) {
    return {KeyType::Enter, '\0'};
  }
  if (event == ftxui::Event::Escape) {
    return {KeyType::Escape, '\0'};
  }
  if (event == ftxui::Event::CtrlH) {
    return {KeyType::CtrlBackspace, '\0'};
  }
  if (event == ftxui::Event::CtrlZ) {
    return {KeyType::CtrlZ, '\0'};
  }
  if (event == ftxui::Event::Backspace) {
    if (controlModifierPressed()) {
      return {KeyType::CtrlBackspace, '\0'};
    }
    return {KeyType::Backspace, '\0'};
  }
  if (event == ftxui::Event::Tab) {
    return {KeyType::Tab, '\0'};
  }
  if (event == ftxui::Event::TabReverse) {
    return {KeyType::TabReverse, '\0'};
  }
  if (event == ftxui::Event::ArrowUp) {
    return {KeyType::Up, '\0'};
  }
  if (event == ftxui::Event::ArrowDown) {
    return {KeyType::Down, '\0'};
  }
  if (event == ftxui::Event::ArrowLeft) {
    return {KeyType::Left, '\0'};
  }
  if (event == ftxui::Event::ArrowRight) {
    return {KeyType::Right, '\0'};
  }
  if (event == ftxui::Event::Home) {
    return {KeyType::Home, '\0'};
  }
  if (event == ftxui::Event::End) {
    return {KeyType::End, '\0'};
  }
  if (event == ftxui::Event::PageUp) {
    return {KeyType::PageUp, '\0'};
  }
  if (event == ftxui::Event::PageDown) {
    return {KeyType::PageDown, '\0'};
  }
  if (event == ftxui::Event::Delete) {
    return {KeyType::Delete, '\0'};
  }
  if (event.is_character() && !event.character().empty()) {
    const auto& text = event.character();
    return {KeyType::Character, text.size() == 1 ? text[0] : '\0', text};
  }
  return {KeyType::Unknown, '\0'};
}

}  // namespace


int App::run() {
#if defined(_WIN32)
  // Windows Terminal does not advertise truecolor through TERM/COLORTERM.
  // Select it before any semantic palette helper constructs an RGB color;
  // otherwise FTXUI collapses the graphite/cyan palette to ANSI colors.
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
  // Rewrite the per-user startup entry after setup has recorded a background
  // choice, so a fresh install never removes an existing entry prematurely.
  if (settings_.backgroundConsentAsked || settings_.backgroundServiceEnabled) {
    string startupError;
    if (!setBackgroundStartupEnabled(settings_.backgroundServiceEnabled, startupError)) {
      setMessage("Unable to update the background startup service: " + startupError, 5);
    }
  }
  // Windows sign-in launches this process with --background. Keep that path
  // free of FTXUI so only the Scan R1 bridge and notification-area handler run.
  const bool backgroundStarted = backgroundController_.start(
      startInBackground_ || settings_.backgroundServiceEnabled, startInBackground_, [this] {
        backgroundQuitRequested_.store(true);
      }, [this] { foregroundRequested_.store(true); });
  bool backgroundServiceUnavailable = false;
  if (!backgroundStarted && startInBackground_) {
    // A background process that cannot be reached through its controller can never be asked to
    // quit by a later launch, so it must not keep running. The saved preference is left alone: the
    // failure may be transient (for example the shell was not ready yet at sign-in) and the next
    // sign-in should try again.
    cerr << "Inventatory background service could not start its controller; exiting.\n";
    running_ = false;
    backgroundServiceUnavailable = true;
  } else if (!backgroundStarted && settings_.backgroundServiceEnabled) {
    // A tray/controller startup failure must not leave the persisted setting
    // claiming that the background bridge is available on the next launch.
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
    // Skip the loop; the shutdown below still stops the services and saves.
  } else if (startInBackground_) {
    runBackgroundLoop();
  } else {
    runInteractiveLoop();
  }

  // Stop producers before the final save.  LocalHttpServer joins all request
  // workers, and the workspace-bound futures are joined here as well, so no
  // late callback can mutate the store after the final snapshot was written.
  //
  // Requests parked for the (now stopped) application loop are failed first. Otherwise
  // server_.stop() would wait for in-flight workers, which in turn wait for this thread, and a
  // launcher waiting for this process to release its lock would time out.
  cancelPendingDeviceRequests("Inventatory is shutting down", true);
  mdnsService_.stop();
  server_.stop();
  stopWorkspaceBoundWorkUntil(chrono::steady_clock::now() + chrono::seconds(3));
  const bool finalSaveSucceeded = saveState();
  if (!finalSaveSucceeded) {
    // There is no interactive frame left to display this error.  Keep the
    // process result and stderr actionable for launchers, and retain the
    // in-memory state until App destruction for the caller's recovery path.
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
  processScans();
  processDeviceRequests();
  processDeviceSyncEvents();
  clearMessageIfExpired();
  clearDeleteConfirmationIfExpired();
  processUpdateCheck();
  processSoftwareUpdate();
  processScanFirmwareCheck();
  processScanDigiKeyEnrichment();
  processDigiKeyRefresh();
  processImportSync();
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
        // Another launch found this window already open; flag it so the desktop can draw attention.
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
      // Tick faster only while the post-update wordmark colour reveal is animating.
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
