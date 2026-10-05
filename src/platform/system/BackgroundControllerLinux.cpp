// Inventatory - Linux single-instance and background-service control.

#include "platform/system/BackgroundController.h"

#include "platform/system/Environment.h"
#include "platform/system/StartupRegistration.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace inventatory {
namespace filesystem = std::filesystem;
namespace {

volatile sig_atomic_t gQuitSignal = 0;
volatile sig_atomic_t gOpenSignal = 0;
// Self-pipe: the handler writes a byte so the signal thread sleeps in poll() instead of polling flags.
// Both ends stay open for the life of the process so a late signal can never write to a closed
// (or reused) descriptor.
int gWakeRead = -1;
int gWakeWrite = -1;
struct sigaction gPreviousTermAction{};
struct sigaction gPreviousHangupAction{};
struct sigaction gPreviousInterruptAction{};
struct sigaction gPreviousUserAction{};
bool gSignalHandlersInstalled = false;

void wakeSignalThread() {
  if (gWakeWrite < 0) return;
  const int savedErrno = errno;
  const char byte = 1;
  const auto ignored = write(gWakeWrite, &byte, 1);
  (void)ignored;
  errno = savedErrno;
}

void controllerSignalHandler(int signalNumber) {
  if (signalNumber == SIGUSR1) gOpenSignal = 1;
  else gQuitSignal = 1;
  wakeSignalThread();
}

bool createWakePipe() {
  if (gWakeRead >= 0) return true;
  int descriptors[2]{};
  if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) return false;
  gWakeRead = descriptors[0];
  gWakeWrite = descriptors[1];
  return true;
}

filesystem::path runtimeDirectory() {
  const auto validPrivateDirectory = [](const filesystem::path& path) {
    struct stat status{};
    return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == geteuid() &&
           (status.st_mode & 0077) == 0;
  };
  if (const auto xdg = environmentValue("XDG_RUNTIME_DIR"); xdg.has_value() && !xdg->empty() &&
      filesystem::path(*xdg).is_absolute()) {
    const auto directory = filesystem::path(*xdg) / "inventatory";
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return {};
    if (validPrivateDirectory(directory)) return directory;
  }
  const auto fallback = filesystem::path("/tmp") / ("inventatory-" + std::to_string(geteuid()));
  if (mkdir(fallback.c_str(), 0700) != 0 && errno != EEXIST) return {};
  return validPrivateDirectory(fallback) ? fallback : filesystem::path();
}

filesystem::path lockPath(bool backgroundMode) {
  const auto directory = runtimeDirectory();
  if (directory.empty()) return {};
  return directory / (backgroundMode ? "background.lock" : "interactive.lock");
}

int acquireLock(bool backgroundMode) {
  const auto path = lockPath(backgroundMode);
  if (path.empty()) return -1;
  const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (descriptor < 0) return -1;
  if (fchmod(descriptor, 0600) != 0 || flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    close(descriptor);
    return -1;
  }
  const auto pid = std::to_string(getpid()) + "\n";
  if (ftruncate(descriptor, 0) != 0 || pwrite(descriptor, pid.data(), pid.size(), 0) != static_cast<ssize_t>(pid.size()) ||
      fsync(descriptor) != 0) {
    flock(descriptor, LOCK_UN);
    close(descriptor);
    return -1;
  }
  return descriptor;
}

pid_t lockedProcessId(bool backgroundMode) {
  const auto path = lockPath(backgroundMode);
  if (path.empty()) return -1;
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return -1;
  if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
    flock(descriptor, LOCK_UN);
    close(descriptor);
    return -1;
  }
  char text[32]{};
  const auto count = pread(descriptor, text, sizeof(text) - 1U, 0);
  close(descriptor);
  if (count <= 0) return -1;
  char* end = nullptr;
  errno = 0;
  const auto value = strtol(text, &end, 10);
  if (errno != 0 || end == text || value <= 1 || value > std::numeric_limits<pid_t>::max()) return -1;
  return static_cast<pid_t>(value);
}

std::string executableName(std::string target) {
  constexpr char kDeleted[] = " (deleted)";
  const auto suffix = std::string(kDeleted);
  // A binary replaced on disk (an update) is reported with this suffix while it keeps running.
  if (target.size() > suffix.size() && target.compare(target.size() - suffix.size(), suffix.size(), suffix) == 0) {
    target.resize(target.size() - suffix.size());
  }
  return filesystem::path(target).filename().string();
}

std::string readExecutableLink(const std::string& path) {
  std::vector<char> buffer(4096U);
  const auto length = readlink(path.c_str(), buffer.data(), buffer.size());
  if (length <= 0 || static_cast<size_t>(length) == buffer.size()) return {};
  return std::string(buffer.data(), static_cast<size_t>(length));
}

// The lock file only records a PID, so before signalling it make sure that PID still belongs to a
// process of this user running this application (and not an unrelated process that reused it).
bool processLooksLikeInventatory(pid_t process) {
  const auto base = std::string("/proc/") + std::to_string(process);
  struct stat status{};
  if (stat(base.c_str(), &status) != 0 || status.st_uid != geteuid()) return false;
  const auto target = executableName(readExecutableLink(base + "/exe"));
  const auto own = executableName(readExecutableLink("/proc/self/exe"));
  return !target.empty() && target == own;
}

bool sendProcessSignal(pid_t process, int signalNumber) {
  if (process <= 1) return false;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  const int processDescriptor = static_cast<int>(syscall(SYS_pidfd_open, process, 0));
  if (processDescriptor >= 0) {
    // The descriptor pins the process, so the check below cannot be defeated by PID reuse.
    const bool sent = processLooksLikeInventatory(process) &&
                      syscall(SYS_pidfd_send_signal, processDescriptor, signalNumber, nullptr, 0) == 0;
    close(processDescriptor);
    return sent;
  }
  if (errno != ENOSYS && errno != EINVAL) return false;
#endif
  return processLooksLikeInventatory(process) && kill(process, signalNumber) == 0;
}

filesystem::path executablePath() {
  std::vector<char> buffer(4096U);
  for (;;) {
    const auto count = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count < 0) return {};
    if (static_cast<size_t>(count) < buffer.size()) return filesystem::path(std::string(buffer.data(), count));
    if (buffer.size() >= 1024U * 1024U) return {};
    buffer.resize(buffer.size() * 2U);
  }
}

bool installSignalHandlers() {
  if (gSignalHandlersInstalled) return true;
  if (!createWakePipe()) return false;
  struct sigaction action{};
  action.sa_handler = controllerSignalHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (sigaction(SIGTERM, &action, &gPreviousTermAction) != 0 ||
      sigaction(SIGINT, &action, &gPreviousInterruptAction) != 0 ||
      // Closing the terminal window sends SIGHUP. FTXUI re-raises the signal it received once its
      // loop ends, so without a handler here the process would die before saving or handing the
      // workspace back to the background service.
      sigaction(SIGHUP, &action, &gPreviousHangupAction) != 0 ||
      sigaction(SIGUSR1, &action, &gPreviousUserAction) != 0) {
    return false;
  }
  gSignalHandlersInstalled = true;
  return true;
}

void restoreSignalHandlers() {
  if (!gSignalHandlersInstalled) return;
  sigaction(SIGTERM, &gPreviousTermAction, nullptr);
  sigaction(SIGINT, &gPreviousInterruptAction, nullptr);
  sigaction(SIGHUP, &gPreviousHangupAction, nullptr);
  sigaction(SIGUSR1, &gPreviousUserAction, nullptr);
  gSignalHandlersInstalled = false;
}

}  // namespace

BackgroundController::~BackgroundController() { stop(); }

bool BackgroundController::acquireSingleInstance(bool backgroundMode) {
  if (instanceLockFd_ >= 0) return false;
  const int descriptor = acquireLock(backgroundMode);
  if (descriptor < 0) return false;
  backgroundMode_ = backgroundMode;
  instanceLockFd_ = descriptor;
  installSignalHandlers();
  return true;
}

bool BackgroundController::signalExistingInstance() const {
  return sendProcessSignal(lockedProcessId(false), SIGUSR1);
}

bool BackgroundController::backgroundServiceRunning() const {
  const auto path = lockPath(true);
  if (path.empty()) return false;
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return false;
  const bool running = flock(descriptor, LOCK_EX | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
  if (!running) flock(descriptor, LOCK_UN);
  close(descriptor);
  return running;
}

bool BackgroundController::interactiveInstanceRunning() const {
  const auto path = lockPath(false);
  if (path.empty()) return false;
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) return false;
  const bool running = flock(descriptor, LOCK_EX | LOCK_NB) != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
  if (!running) flock(descriptor, LOCK_UN);
  close(descriptor);
  return running;
}

bool BackgroundController::requestBackgroundServiceQuit() const {
  return sendProcessSignal(lockedProcessId(true), SIGTERM);
}

bool BackgroundController::waitForBackgroundServiceToStop(int timeoutMs) const {
  const auto timeout = std::chrono::milliseconds(std::max(0, timeoutMs));
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (backgroundServiceRunning()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

bool BackgroundController::forceStopBackgroundService() const {
  return sendProcessSignal(lockedProcessId(true), SIGKILL);
}

BackgroundStopResult BackgroundController::stopBackgroundService(int gracefulTimeoutMs, int forcedTimeoutMs,
                                                                 const std::function<void(int)>& onWaiting) const {
  if (!backgroundServiceRunning()) return BackgroundStopResult::NotRunning;
  const auto begin = std::chrono::steady_clock::now();
  const auto elapsed = [&] { return std::chrono::steady_clock::now() - begin; };
  const auto graceful = std::chrono::milliseconds(std::max(0, gracefulTimeoutMs));
  bool requested = false;
  int reportedSeconds = 0;
  while (backgroundServiceRunning()) {
    // The PID is written just after the lock is taken, so the request is retried until it lands.
    if (!requested) requested = requestBackgroundServiceQuit();
    // A request that cannot be delivered (no PID to signal) will not start working by waiting.
    if (elapsed() >= graceful || (!requested && elapsed() >= std::chrono::seconds(3))) break;
    const int seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(elapsed()).count());
    if (seconds > reportedSeconds) {
      reportedSeconds = seconds;
      if (onWaiting) onWaiting(seconds);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!backgroundServiceRunning()) return BackgroundStopResult::Stopped;
  if (!forceStopBackgroundService()) return BackgroundStopResult::Failed;
  return waitForBackgroundServiceToStop(forcedTimeoutMs) ? BackgroundStopResult::Forced
                                                          : BackgroundStopResult::Failed;
}

bool BackgroundController::restartAsBackgroundService() {
  if (backgroundMode_) return false;
  const auto executable = executablePath();
  if (executable.empty()) return false;
  if (instanceLockFd_ >= 0) {
    flock(instanceLockFd_, LOCK_UN);
    close(instanceLockFd_);
    instanceLockFd_ = -1;
  }
  // Prefer the per-user systemd unit so the service is supervised, logged and visible to
  // `systemctl --user status`; fall back to a detached process when systemd is unavailable.
  std::string unitError;
  if (startBackgroundServiceUnit(unitError)) {
    const auto unitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!backgroundServiceRunning() && std::chrono::steady_clock::now() < unitDeadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (backgroundServiceRunning()) return true;
  }
  const auto child = fork();
  if (child < 0) return false;
  if (child == 0) {
    if (setsid() < 0) _exit(127);
    const auto daemon = fork();
    if (daemon < 0) _exit(127);
    if (daemon > 0) _exit(0);
    const int nullDevice = open("/dev/null", O_RDWR);
    if (nullDevice >= 0) {
      dup2(nullDevice, STDIN_FILENO);
      dup2(nullDevice, STDOUT_FILENO);
      dup2(nullDevice, STDERR_FILENO);
      if (nullDevice > STDERR_FILENO) close(nullDevice);
    }
    execl(executable.c_str(), executable.c_str(), "--background", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!backgroundServiceRunning() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return backgroundServiceRunning();
}

bool BackgroundController::start(bool enabled, bool hideInitially, Callback onQuit, Callback onOpen) {
  (void)hideInitially;
  enabled_.store(enabled);
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    // An empty callback keeps the one installed earlier (run() passes both; the in-session enable
    // from Settings passes only onQuit).
    if (onQuit) onQuit_ = std::move(onQuit);
    if (onOpen) onOpen_ = std::move(onOpen);
  }
  if (!installSignalHandlers()) return false;
  if (signalThread_.joinable()) return true;
  trayStartupCancelled_.store(false);
  signalThread_ = std::thread([this] {
    while (!trayStartupCancelled_.load()) {
      pollfd wake{gWakeRead, POLLIN, 0};
      const int ready = poll(&wake, 1, -1);
      if (ready < 0 && errno != EINTR) break;
      char drain[64];
      while (read(gWakeRead, drain, sizeof(drain)) > 0) {}
      if (gQuitSignal != 0) {
        gQuitSignal = 0;
        requestQuitFromTray();
      }
      if (gOpenSignal != 0) {
        gOpenSignal = 0;
        requestOpenFromTray();
      }
    }
  });
  return true;
}

// Linux has no tray: background mode is only the enabled flag. The instance lock, signal thread and
// signal handlers belong to the running process and are released by stop() at shutdown.
void BackgroundController::disableBackgroundMode() { enabled_.store(false); }

void BackgroundController::stop() {
  enabled_.store(false);
  trayStartupCancelled_.store(true);
  wakeSignalThread();
  if (signalThread_.joinable()) signalThread_.join();
  restoreSignalHandlers();
  if (instanceLockFd_ >= 0) {
    flock(instanceLockFd_, LOCK_UN);
    close(instanceLockFd_);
    instanceLockFd_ = -1;
  }
  std::lock_guard<std::mutex> lock(callbackMutex_);
  onQuit_ = {};
  onOpen_ = {};
}

bool BackgroundController::enabled() const { return enabled_.load(); }
void BackgroundController::hideConsole(bool notifyUser) { (void)notifyUser; }
void BackgroundController::showConsole() {}

void BackgroundController::requestQuitFromTray() {
  Callback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = onQuit_;
  }
  if (callback) callback();
}

void BackgroundController::requestOpenFromTray() {
  Callback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = onOpen_;
  }
  if (callback) callback();
}

}  // namespace inventatory
