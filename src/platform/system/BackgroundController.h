// Inventatory - Notification-area lifetime control for the background scanner service.

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
struct HWND__;
using HWND = HWND__*;
#endif

namespace inventatory {

enum class BackgroundStopResult {
  NotRunning,  // no background service held its lock
  Stopped,     // it quit after the polite request
  Forced,      // it ignored the request and was terminated
  Failed,      // it is still running
};

#ifndef _WIN32
// Directory that holds the single-instance locks. Prefers an absolute XDG_RUNTIME_DIR, then the
// per-user <userRunRoot>/<uid> directory (so a launch without the variable, such as from cron or a
// plain su, still shares the locks of the systemd unit), then <fallbackRoot>/inventatory-<uid>. Only a
// directory owned by the user and closed to others is used; empty when none qualifies.
std::filesystem::path backgroundRuntimeDirectory(const std::optional<std::string>& xdgRuntimeDir,
                                                 const std::filesystem::path& userRunRoot,
                                                 const std::filesystem::path& fallbackRoot);
#endif

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
  // Terminates a background service that ignored the quit request. Only a process verified to be
  // this application is signalled.
  bool forceStopBackgroundService() const;
  // Asks the background service to quit and waits up to gracefulTimeoutMs for it to release its
  // lock; if it does not, terminates it and waits up to forcedTimeoutMs. onWaiting (optional) is
  // called about once a second with the seconds waited so far.
  BackgroundStopResult stopBackgroundService(int gracefulTimeoutMs, int forcedTimeoutMs,
                                             const std::function<void(int)>& onWaiting = {}) const;
  bool restartAsBackgroundService();
  // Linux keeps any callback it already holds when an empty one is passed, so enabling background
  // mode later in a session does not drop the callbacks the application installed at startup.
  bool start(bool enabled, bool hideInitially, Callback onQuit, Callback onOpen = {});
  // Ends background mode only (the "run in background" setting turned off while the process keeps
  // running). Windows removes the notification-area icon. Linux keeps the single-instance lock, the
  // signal handling and the callbacks, because the interactive process still owns the workspace.
  void disableBackgroundMode();
  // Full teardown at process shutdown; releases the single-instance ownership.
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
  std::atomic<bool> trayStartupCancelled_{false};
#ifdef _WIN32
  std::atomic<HWND> trayWindow_{nullptr};
#else
  std::atomic<void*> trayWindow_{nullptr};
#endif
  void* instanceMutex_ = nullptr;
  int instanceLockFd_ = -1;
  bool backgroundMode_ = false;
  std::thread trayThread_;
  std::thread signalThread_;
  mutable std::mutex callbackMutex_;
  mutable std::mutex trayReadyMutex_;
  std::condition_variable trayReadyChanged_;
  bool trayReady_ = false;
  Callback onQuit_;
  Callback onOpen_;
};

}  // namespace inventatory
