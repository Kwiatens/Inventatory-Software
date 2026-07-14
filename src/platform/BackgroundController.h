// Inventatory - Notification-area lifetime control for the background scanner service.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

struct HWND__;
using HWND = HWND__*;

namespace inventatory {

class BackgroundController {
 public:
  using Callback = std::function<void()>;

  BackgroundController() = default;
  ~BackgroundController();

  bool acquireSingleInstance();
  bool signalExistingInstance() const;
  bool start(bool enabled, bool hideInitially, Callback onQuit);
  void stop();
  bool enabled() const;
  void hideConsole(bool notifyUser);
  void showConsole();
  void requestQuitFromTray();

  BackgroundController(const BackgroundController&) = delete;
  BackgroundController& operator=(const BackgroundController&) = delete;

 private:
  void trayThreadMain();
  std::atomic<bool> enabled_{false};
  std::atomic<HWND> trayWindow_{nullptr};
  void* instanceMutex_ = nullptr;
  std::thread trayThread_;
  mutable std::mutex callbackMutex_;
  Callback onQuit_;
};

}  // namespace inventatory
