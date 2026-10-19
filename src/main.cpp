// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "App.h"
#include "platform/system/UpdateService.h"
#include "platform/system/Console.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <iostream>
#ifndef _WIN32
#include <clocale>
#include <langinfo.h>
#endif

namespace {

// A failed launch runs in a window that closes on exit, so the reason is held on screen briefly.
void reportLaunchProblem(const std::string& message, int holdMs) {
  std::cerr << message << '\n';
  if (holdMs <= 0) return;
  std::cerr << "Press Enter to close this window.\n";
  inventatory::waitForAcknowledgement(holdMs);
}

// Takes the workspace from a running background service. Returns false if it still runs afterwards.
bool takeOverFromBackgroundService(inventatory::BackgroundController& controller) {
  // The long grace period matters only when busy. A service that never answers is terminated,
  // which leaves SQLite and the state files consistent.
  constexpr int kGracefulMs = 12000;
  constexpr int kForcedMs = 3000;
  bool announced = false;
  const auto result = controller.stopBackgroundService(kGracefulMs, kForcedMs, [&announced](int seconds) {
    if (!announced) {
      std::cout << "Stopping the Inventatory background service" << std::flush;
      announced = true;
    }
    std::cout << '.' << std::flush;
    (void)seconds;
  });
  if (announced) std::cout << '\n';
  switch (result) {
    case inventatory::BackgroundStopResult::NotRunning:
    case inventatory::BackgroundStopResult::Stopped:
      return true;
    case inventatory::BackgroundStopResult::Forced:
      std::cerr << "The Inventatory background service did not respond and was terminated.\n";
      return true;
    case inventatory::BackgroundStopResult::Failed:
      break;
  }
  reportLaunchProblem(
      "Inventatory could not stop its background service, so the workspace was not opened to avoid two "
      "processes using it.\nEnd the 'inventatory' background process (for example with the system monitor "
      "or `pkill -x inventatory`) and start Inventatory again.",
      60000);
  return false;
}

int runInventatory(bool startInBackground, std::filesystem::path& updateMarkerPath) {
  inventatory::BackgroundController backgroundController;
#ifndef _WIN32
  // A background service waits while a terminal session owns the workspace, then takes over.
  if (startInBackground) backgroundController.waitForWorkspaceRelease();
#else
  if (startInBackground && backgroundController.interactiveInstanceRunning()) return 0;
#endif
  if (!backgroundController.acquireSingleInstance(startInBackground)) {
    if (startInBackground) return 0;
    const bool existingWindowNotified = backgroundController.signalExistingInstance();
#ifdef _WIN32
    // Windows restores the existing console window, so this one has nothing left to say.
    if (existingWindowNotified) return 0;
#endif
    reportLaunchProblem(existingWindowNotified
                            ? "Inventatory is already open in another window; it was asked to come forward."
                            : "Inventatory is already running but could not be reached.",
                        existingWindowNotified ? 4000 : 15000);
    return 0;
  }
  if (!startInBackground && !takeOverFromBackgroundService(backgroundController)) return 1;
  inventatory::App app(startInBackground, backgroundController);
  const int exitCode = app.run();
  if (app.updateInstallerLaunched()) updateMarkerPath = app.updateMarkerPath();
  if (app.workAbandoned() && updateMarkerPath.empty()) {
    // A network worker outlived the shutdown deadline. Everything is saved, and destroying App would
    // block on that worker.
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(exitCode);
  }
  return exitCode;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
#ifdef Inventatory_VERSION_STRING
    std::cout << Inventatory_VERSION_STRING << '\n';
#else
    std::cout << "dev\n";
#endif
    return 0;
  }
  const bool startInBackground = argc == 2 && std::string(argv[1]) == "--background";
#ifndef _WIN32
  if (!startInBackground && !inventatory::ensureTerminalAttached(argc, argv)) {
    return 1;
  }
#endif
#ifndef _WIN32
  if (std::setlocale(LC_ALL, "") == nullptr || std::string(nl_langinfo(CODESET)) != "UTF-8") {
    if (std::setlocale(LC_ALL, "C.UTF-8") == nullptr || std::string(nl_langinfo(CODESET)) != "UTF-8") {
      // The background service draws no UI, so a missing UTF-8 locale must not stop it.
      // A restart loop would silently disable Scan R1 bridging.
      if (!startInBackground) {
        std::cerr << "Inventatory requires a UTF-8 terminal locale. Set LANG to an installed UTF-8 locale and retry.\n";
        return 1;
      }
    }
  }
#endif
  std::filesystem::path updateMarkerPath;
  const int exitCode = runInventatory(startInBackground, updateMarkerPath);
  // Relaunch only after the app and single-instance lock are fully torn down.
  if (!updateMarkerPath.empty()) inventatory::relaunchAfterUpdate(updateMarkerPath);
  return exitCode;
}
