// Inventatory - Linux single-instance and background-service control.

#include "platform/system/BackgroundController.h"

#include "platform/system/Environment.h"

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
struct sigaction gPreviousTermAction{};
struct sigaction gPreviousInterruptAction{};
struct sigaction gPreviousUserAction{};
bool gSignalHandlersInstalled = false;

void controllerSignalHandler(int signalNumber) {
  if (signalNumber == SIGUSR1) gOpenSignal = 1;
  else gQuitSignal = 1;
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

bool sendProcessSignal(pid_t process, int signalNumber) {
  if (process <= 1) return false;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  const int processDescriptor = static_cast<int>(syscall(SYS_pidfd_open, process, 0));
  if (processDescriptor >= 0) {
    const bool sent = syscall(SYS_pidfd_send_signal, processDescriptor, signalNumber, nullptr, 0) == 0;
    close(processDescriptor);
    return sent;
  }
  if (errno != ENOSYS && errno != EINVAL) return false;
#endif
  // Older kernels without pidfds still get a PID-reuse check before delivery.
  std::vector<char> targetBuffer(4096U);
  std::vector<char> ownBuffer(4096U);
  const auto targetPath = std::string("/proc/") + std::to_string(process) + "/exe";
  const auto targetLength = readlink(targetPath.c_str(), targetBuffer.data(), targetBuffer.size());
  const auto ownLength = readlink("/proc/self/exe", ownBuffer.data(), ownBuffer.size());
  if (targetLength <= 0 || ownLength <= 0 || static_cast<size_t>(targetLength) == targetBuffer.size() ||
      static_cast<size_t>(ownLength) == ownBuffer.size()) return false;
  const auto target = std::filesystem::path(std::string(targetBuffer.data(), targetLength));
  const auto own = std::filesystem::path(std::string(ownBuffer.data(), ownLength));
  if (target != own) return false;
  return kill(process, signalNumber) == 0;
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
  struct sigaction action{};
  action.sa_handler = controllerSignalHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (sigaction(SIGTERM, &action, &gPreviousTermAction) != 0 ||
      sigaction(SIGINT, &action, &gPreviousInterruptAction) != 0 ||
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

bool BackgroundController::restartAsBackgroundService() {
  if (backgroundMode_) return false;
  const auto executable = executablePath();
  if (executable.empty()) return false;
  if (instanceLockFd_ >= 0) {
    flock(instanceLockFd_, LOCK_UN);
    close(instanceLockFd_);
    instanceLockFd_ = -1;
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
    onQuit_ = std::move(onQuit);
    onOpen_ = std::move(onOpen);
  }
  if (!installSignalHandlers()) return false;
  if (signalThread_.joinable()) return true;
  trayStartupCancelled_.store(false);
  signalThread_ = std::thread([this] {
    while (!trayStartupCancelled_.load()) {
      if (gQuitSignal != 0) {
        gQuitSignal = 0;
        requestQuitFromTray();
      }
      if (gOpenSignal != 0) {
        gOpenSignal = 0;
        requestOpenFromTray();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });
  return true;
}

void BackgroundController::stop() {
  enabled_.store(false);
  trayStartupCancelled_.store(true);
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
