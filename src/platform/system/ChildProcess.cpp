// Inventatory - bounded helper processes on Linux.

#include "platform/system/ChildProcess.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace inventatory {
namespace {

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

void closeDescriptor(int& descriptor) {
  if (descriptor >= 0) close(descriptor);
  descriptor = -1;
}

void makeNonBlocking(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFL, 0);
  if (flags >= 0) fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

ChildProcessResult runChildProcess(const std::vector<std::string>& argv, const ChildProcessOptions& options) {
  ChildProcessResult result;
  if (argv.empty() || argv.front().empty()) return result;

  // Everything the child needs is prepared here: after fork() only async-signal-safe calls are made.
  std::vector<char*> arguments;
  arguments.reserve(argv.size() + 1U);
  for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
  arguments.push_back(nullptr);

  int outputPipe[2] = {-1, -1};
  int inputPipe[2] = {-1, -1};
  if (pipe2(outputPipe, O_CLOEXEC) != 0) return result;
  if (options.hasInput && pipe2(inputPipe, O_CLOEXEC) != 0) {
    closeDescriptor(outputPipe[0]);
    closeDescriptor(outputPipe[1]);
    return result;
  }

  const pid_t child = fork();
  if (child < 0) {
    closeDescriptor(outputPipe[0]);
    closeDescriptor(outputPipe[1]);
    closeDescriptor(inputPipe[0]);
    closeDescriptor(inputPipe[1]);
    return result;
  }
  if (child == 0) {
    setpgid(0, 0);
    if (options.hasInput) {
      if (dup2(inputPipe[0], STDIN_FILENO) < 0) _exit(127);
    } else {
      const int nullInput = open("/dev/null", O_RDONLY);
      if (nullInput < 0 || dup2(nullInput, STDIN_FILENO) < 0) _exit(127);
    }
    if (dup2(outputPipe[1], STDOUT_FILENO) < 0) _exit(127);
    if (options.mergeStderr) {
      if (dup2(outputPipe[1], STDERR_FILENO) < 0) _exit(127);
    } else {
      const int nullError = open("/dev/null", O_WRONLY);
      if (nullError < 0 || dup2(nullError, STDERR_FILENO) < 0) _exit(127);
    }
#ifdef SYS_close_range
    syscall(SYS_close_range, 3U, ~0U, 0U);
#endif
    execvp(arguments.front(), arguments.data());
    _exit(127);
  }

  // Both sides set the group so the first kill() cannot race the child's own setpgid().
  setpgid(child, child);
  closeDescriptor(outputPipe[1]);
  closeDescriptor(inputPipe[0]);
  result.started = true;

  SigpipeGuard sigpipeGuard;
  makeNonBlocking(outputPipe[0]);
  int inputDescriptor = inputPipe[1];
  if (inputDescriptor >= 0) makeNonBlocking(inputDescriptor);
  if (inputDescriptor >= 0 && options.input.empty()) closeDescriptor(inputDescriptor);

  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + options.timeout;
  bool killed = false;
  const auto killHelper = [&] {
    if (killed) return;
    killed = true;
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
  };

  int outputDescriptor = outputPipe[0];
  size_t inputOffset = 0;
  while (!killed && (outputDescriptor >= 0 || inputDescriptor >= 0)) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) {
      result.timedOut = true;
      killHelper();
      break;
    }
    pollfd descriptors[2]{};
    nfds_t count = 0;
    if (outputDescriptor >= 0) descriptors[count++] = pollfd{outputDescriptor, POLLIN, 0};
    if (inputDescriptor >= 0) descriptors[count++] = pollfd{inputDescriptor, POLLOUT, 0};
    const int ready = poll(descriptors, count, static_cast<int>(std::min<long long>(remaining, INT_MAX)));
    if (ready < 0) {
      if (errno == EINTR) continue;
      killHelper();
      break;
    }
    for (nfds_t index = 0; index < count && ready > 0; ++index) {
      const auto& entry = descriptors[index];
      if (entry.revents == 0) continue;
      if (entry.fd == outputDescriptor) {
        char buffer[4096];
        for (;;) {
          const auto received = read(outputDescriptor, buffer, sizeof(buffer));
          if (received > 0) {
            const auto room = options.maxOutputBytes > result.output.size() ? options.maxOutputBytes - result.output.size() : 0U;
            if (static_cast<size_t>(received) > room) {
              result.output.append(buffer, room);
              result.outputTooLarge = true;
              killHelper();
              closeDescriptor(outputDescriptor);
              break;
            }
            result.output.append(buffer, static_cast<size_t>(received));
            continue;
          }
          if (received < 0 && errno == EINTR) continue;
          if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          closeDescriptor(outputDescriptor);  // EOF or a read error
          break;
        }
      } else if (entry.fd == inputDescriptor) {
        while (inputDescriptor >= 0) {
          const auto written = write(inputDescriptor, options.input.data() + inputOffset, options.input.size() - inputOffset);
          if (written > 0) {
            inputOffset += static_cast<size_t>(written);
            if (inputOffset >= options.input.size()) closeDescriptor(inputDescriptor);
            continue;
          }
          if (written < 0 && errno == EINTR) continue;
          if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          result.inputFailed = true;  // the helper closed its input early (EPIPE) or the pipe failed
          closeDescriptor(inputDescriptor);
        }
      }
    }
  }
  closeDescriptor(outputDescriptor);
  closeDescriptor(inputDescriptor);

  int status = 0;
  bool reaped = false;
  bool statusKnown = false;
  auto pause = std::chrono::milliseconds(1);
  while (!killed) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      reaped = true;
      statusKnown = true;
      break;
    }
    if (waited < 0 && errno != EINTR) {
      reaped = true;  // already reaped elsewhere (SIGCHLD ignored): the exit status is unknown
      break;
    }
    if (Clock::now() >= deadline) {
      result.timedOut = true;
      killHelper();
      break;
    }
    std::this_thread::sleep_for(pause);
    pause = std::min(pause * 2, std::chrono::milliseconds(20));
  }
  if (!reaped) {
    pid_t waited;
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    statusKnown = waited == child;
  }
  result.exitCode = statusKnown && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

}  // namespace inventatory
