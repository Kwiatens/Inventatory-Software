#include "App.h"

#include <string>
#include <iostream>

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
  inventatory::BackgroundController backgroundController;
  if (startInBackground && backgroundController.interactiveInstanceRunning()) return 0;
  if (!backgroundController.acquireSingleInstance(startInBackground)) {
    if (!startInBackground) backgroundController.signalExistingInstance();
    return 0;
  }
  if (!startInBackground && backgroundController.backgroundServiceRunning()) {
    backgroundController.requestBackgroundServiceQuit();
    backgroundController.waitForBackgroundServiceToStop(5000);
  }
  inventatory::App app(startInBackground, backgroundController);
  return app.run();
}
