// Inventatory - bounded helper processes on Linux.

#pragma once

#ifndef _WIN32

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace inventatory {

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
