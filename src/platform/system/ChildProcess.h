// Inventatory - bounded helper processes on Linux.

#pragma once

#ifndef _WIN32

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <time.h>

namespace inventatory {

// Writing to a helper that already exited must fail with EPIPE instead of raising SIGPIPE in the
// application. A SIGPIPE this guard provoked is consumed again; one that was already pending stays.
class SigpipeGuard final {
 public:
  SigpipeGuard() {
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    valid_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    if (valid_) {
      sigset_t pending{};
      if (sigpending(&pending) == 0) hadPending_ = sigismember(&pending, SIGPIPE) == 1;
    }
  }

  ~SigpipeGuard() {
    if (!valid_) return;
    if (!hadPending_) {
      sigset_t pending{};
      if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        timespec noWait{};
        sigtimedwait(&blocked_, nullptr, &noWait);
      }
    }
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }

  SigpipeGuard(const SigpipeGuard&) = delete;
  SigpipeGuard& operator=(const SigpipeGuard&) = delete;

 private:
  sigset_t blocked_{};
  sigset_t previous_{};
  bool valid_ = false;
  bool hadPending_ = false;
};

struct ChildProcessOptions {
  // The whole run, including a helper that closes its output but never exits. The helper and its
  // process group are killed when it expires.
  std::chrono::milliseconds timeout{10000};
  // Captured output beyond this ends the run.
  std::size_t maxOutputBytes = 64U * 1024U;
  // Merge the helper's stderr into the captured output. Otherwise stderr is discarded, so a helper's
  // warnings never reach the terminal user interface.
  bool mergeStderr = false;
  // Written to the helper's standard input and then closed; standard input is /dev/null when unset.
  bool hasInput = false;
  std::string input;
};

struct ChildProcessResult {
  bool started = false;          // the helper process was created (it may still have failed to exec)
  bool timedOut = false;
  bool outputTooLarge = false;
  bool inputFailed = false;      // the helper stopped reading before all input was written
  int exitCode = -1;             // -1 when it was killed or never ran
  std::string output;

  bool succeeded() const { return started && !timedOut && !outputTooLarge && !inputFailed && exitCode == 0; }
};

// Runs argv[0] (searched in PATH) with a bounded lifetime. Descriptors are close-on-exec, the argument
// vector is built before fork() so the child only makes async-signal-safe calls, and the helper leads
// its own process group so a timeout also stops anything it started.
ChildProcessResult runChildProcess(const std::vector<std::string>& argv, const ChildProcessOptions& options = {});

}  // namespace inventatory

#endif
