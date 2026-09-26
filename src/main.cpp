#include "App.h"
#include "platform/system/UpdateService.h"

#include <filesystem>
#include <string>
#include <iostream>
#ifndef _WIN32
#include <clocale>
#include <langinfo.h>
#endif

namespace {

int runInventatory(bool startInBackground, std::filesystem::path& updateMarkerPath) {
  inventatory::BackgroundController backgroundController;
  if (startInBackground && backgroundController.interactiveInstanceRunning()) return 0;
  if (!backgroundController.acquireSingleInstance(startInBackground)) {
    if (!startInBackground) backgroundController.signalExistingInstance();
    return 0;
  }
  if (!startInBackground && backgroundController.backgroundServiceRunning()) {
    backgroundController.requestBackgroundServiceQuit();
    if (!backgroundController.waitForBackgroundServiceToStop(5000)) {
      // Never open the shared workspace while the background bridge may still
      // be using it. Releasing the interactive mutex on return lets the
      // background process remain the sole owner until the user retries.
      std::cerr << "Inventatory background service did not stop; refusing concurrent startup.\n";
      return 1;
    }
  }
  inventatory::App app(startInBackground, backgroundController);
  const int exitCode = app.run();
  if (app.updateInstallerLaunched()) updateMarkerPath = app.updateMarkerPath();
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
#ifndef _WIN32
  if (std::setlocale(LC_ALL, "") == nullptr || std::string(nl_langinfo(CODESET)) != "UTF-8") {
    if (std::setlocale(LC_ALL, "C.UTF-8") == nullptr || std::string(nl_langinfo(CODESET)) != "UTF-8") {
      std::cerr << "Inventatory requires a UTF-8 terminal locale. Set LANG to an installed UTF-8 locale and retry.\n";
      return 1;
    }
  }
#endif
  const bool startInBackground = argc == 2 && std::string(argv[1]) == "--background";
  std::filesystem::path updateMarkerPath;
  const int exitCode = runInventatory(startInBackground, updateMarkerPath);
  // Relaunch only after the app and single-instance lock are fully torn down.
  if (!updateMarkerPath.empty()) inventatory::relaunchAfterUpdate(updateMarkerPath);
  return exitCode;
}
