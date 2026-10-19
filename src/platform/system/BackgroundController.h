// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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
// Directory for the single-instance locks: absolute XDG_RUNTIME_DIR, else <userRunRoot>/<uid>, else
// <fallbackRoot>/inventatory-<uid>. Must be private to the user; empty if none qualifies.
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
#ifndef _WIN32
  // Blocks while a terminal session or another process holds the workspace, without taking it. Waits
  // instead of exiting: systemd restarts the service after any exit except `systemctl stop`.
  void waitForWorkspaceRelease() const;
#endif
  bool requestBackgroundServiceQuit() const;
  bool waitForBackgroundServiceToStop(int timeoutMs) const;
  // Terminates a background service that ignored the quit request. Only a process verified to be
  // this application is signalled.
  bool forceStopBackgroundService() const;
  // Asks the service to quit and waits gracefulTimeoutMs for its lock; then terminates it and waits
  // forcedTimeoutMs. onWaiting is called about once a second with the seconds waited.
  BackgroundStopResult stopBackgroundService(int gracefulTimeoutMs, int forcedTimeoutMs,
                                             const std::function<void(int)>& onWaiting = {}) const;
  bool restartAsBackgroundService();
  // Linux keeps the existing callbacks when an empty one is passed, so enabling background mode later
  // does not drop the callbacks installed at startup.
  bool start(bool enabled, bool hideInitially, Callback onQuit, Callback onOpen = {});
  // Ends background mode only, while the process keeps running. Windows removes the tray icon. Linux
  // keeps the lock, signal handling and callbacks, because the process still owns the workspace.
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
