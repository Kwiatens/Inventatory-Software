// Inventatory - Notification-area lifetime control for the background scanner service.

#pragma once

#include <atomic>
#include <condition_variable>
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

  bool acquireSingleInstance(bool backgroundMode);
  bool signalExistingInstance() const;
  bool backgroundServiceRunning() const;
  bool interactiveInstanceRunning() const;
  bool requestBackgroundServiceQuit() const;
  bool waitForBackgroundServiceToStop(int timeoutMs) const;
  bool restartAsBackgroundService();
  bool start(bool enabled, bool hideInitially, Callback onQuit, Callback onOpen = {});
  void stop();
  bool enabled() const;
  void hideConsole(bool notifyUser);
  void showConsole();
  void requestQuitFromTray();
  void requestOpenFromTray();

  BackgroundController(const BackgroundController&) = delete;
  BackgroundController& operator=(const BackgroundController&) = delete;

 private:
  void trayThreadMain();
  std::atomic<bool> enabled_{false};
  std::atomic<HWND> trayWindow_{nullptr};
  void* instanceMutex_ = nullptr;
  bool backgroundMode_ = false;
  std::thread trayThread_;
  mutable std::mutex callbackMutex_;
  mutable std::mutex trayReadyMutex_;
  std::condition_variable trayReadyChanged_;
  bool trayReady_ = false;
  Callback onQuit_;
  Callback onOpen_;
};

}  // namespace inventatory
