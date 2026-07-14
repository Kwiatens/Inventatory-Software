#include "App.h"

#include <string>

int main(int argc, char* argv[]) {
  const bool startInBackground = argc == 2 && std::string(argv[1]) == "--background";
  hims::BackgroundController backgroundController;
  if (!backgroundController.acquireSingleInstance()) {
    backgroundController.signalExistingInstance();
    return 0;
  }
  hims::App app(startInBackground, backgroundController);
  return app.run();
}
