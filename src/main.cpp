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

// A launch that cannot continue usually runs in a terminal window that closes the moment the
// process exits, so the reason is shown and held on screen briefly instead of vanishing.
void reportLaunchProblem(const std::string& message, int holdMs) {
  std::cerr << message << '\n';
  if (holdMs <= 0) return;
  std::cerr << "Press Enter to close this window.\n";
  inventatory::waitForAcknowledgement(holdMs);
}

// Hands the workspace over from a running background service. Returns false when the service is
// still running afterwards, in which case the workspace must not be opened.
bool takeOverFromBackgroundService(inventatory::BackgroundController& controller) {
  // The service saves and releases its lock within a few seconds; the long grace period only
  // matters when it is busy. A service that never answers is terminated rather than left to block
  // every later launch (a killed process leaves SQLite and the atomic state files consistent).
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
  if (startInBackground && backgroundController.interactiveInstanceRunning()) return 0;
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
    // A network worker outlived the shutdown deadline. Everything is saved and the single-instance
    // lock is released; destroying the App would only block on that worker, so leave without it.
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
      std::cerr << "Inventatory requires a UTF-8 terminal locale. Set LANG to an installed UTF-8 locale and retry.\n";
      return 1;
    }
  }
#endif
  std::filesystem::path updateMarkerPath;
  const int exitCode = runInventatory(startInBackground, updateMarkerPath);
  // Relaunch only after the app and single-instance lock are fully torn down.
  if (!updateMarkerPath.empty()) inventatory::relaunchAfterUpdate(updateMarkerPath);
  return exitCode;
}
