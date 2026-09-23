// Inventatory - CUPS queue integration for Linux label printers.

#include "label_printer/core/LabelPrinterPrivate.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace inventatory {
namespace {

bool runCommand(const std::vector<std::string>& arguments, std::string& output) {
  if (arguments.empty()) return false;
  int descriptors[2]{};
  if (pipe(descriptors) != 0) return false;
  const auto child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  if (child == 0) {
    close(descriptors[0]);
    if (dup2(descriptors[1], STDOUT_FILENO) < 0 || dup2(descriptors[1], STDERR_FILENO) < 0) _exit(127);
    close(descriptors[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(argv.front(), argv.data());
    _exit(127);
  }
  close(descriptors[1]);
  output.clear();
  char buffer[2048];
  for (;;) {
    const auto count = read(descriptors[0], buffer, sizeof(buffer));
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      close(descriptors[0]);
      waitpid(child, nullptr, 0);
      return false;
    }
    if (output.size() + static_cast<size_t>(count) > 64U * 1024U) {
      close(descriptors[0]);
      waitpid(child, nullptr, 0);
      output.clear();
      return false;
    }
    output.append(buffer, static_cast<size_t>(count));
  }
  close(descriptors[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class SigpipeBlock final {
 public:
  SigpipeBlock() {
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    valid_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    if (valid_) {
      sigset_t pending{};
      if (sigpending(&pending) == 0) hadPending_ = sigismember(&pending, SIGPIPE) == 1;
    }
  }

  ~SigpipeBlock() {
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

 private:
  sigset_t blocked_{};
  sigset_t previous_{};
  bool valid_ = false;
  bool hadPending_ = false;
};

bool writeAll(int descriptor, const std::string& contents) {
  SigpipeBlock block;
  size_t offset = 0;
  while (offset < contents.size()) {
    const auto written = write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (written == 0) return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

bool validQueueName(const std::string& name) {
  if (name.empty() || name.size() > 1024U) return false;
  return std::none_of(name.begin(), name.end(), [](unsigned char value) {
    return value == 0 || value == '\r' || value == '\n';
  });
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

class CupsPrinterBackend final : public PrinterBackend {
 public:
  std::vector<PrinterQueueInfo> enumeratePrinters() const override {
    std::string output;
    if (!runCommand({"lpstat", "-p", "-d"}, output)) return {};
    std::string defaultName;
    std::vector<PrinterQueueInfo> printers;
    size_t begin = 0;
    while (begin < output.size()) {
      const auto end = output.find('\n', begin);
      const auto line = output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
      if (line.rfind("system default destination: ", 0) == 0) {
        defaultName = line.substr(std::string("system default destination: ").size());
      } else if (line.rfind("printer ", 0) == 0) {
        const auto nameBegin = std::string("printer ").size();
        const auto nameEnd = line.find(' ', nameBegin);
        if (nameEnd != std::string::npos) {
          PrinterQueueInfo printer;
          printer.name = line.substr(nameBegin, nameEnd - nameBegin);
          printer.driverName = "CUPS";
          const auto state = line.find(" is ", nameEnd);
          printer.statusText = state == std::string::npos ? line.substr(nameEnd + 1U) : line.substr(state + 4U);
          const auto status = lower(printer.statusText);
          printer.isReady = status.find("disabled") == std::string::npos &&
                            status.find("stopped") == std::string::npos;
          printers.push_back(std::move(printer));
        }
      }
      if (end == std::string::npos) break;
      begin = end + 1U;
    }
    for (auto& printer : printers) printer.isDefault = printer.name == defaultName;
    std::sort(printers.begin(), printers.end(), [](const auto& first, const auto& second) {
      if (first.isDefault != second.isDefault) return first.isDefault > second.isDefault;
      return lower(first.name) < lower(second.name);
    });
    return printers;
  }

  PrinterCheckResult probePrinter(const std::string& printerName) const override {
    if (!validQueueName(printerName)) return {false, "No printer configured"};
    std::string output;
    if (!runCommand({"lpstat", "-p", printerName}, output)) {
      return {false, output.empty() ? "Unable to query the CUPS printer queue" : output};
    }
    const auto statusBegin = output.find(" is ");
    if (statusBegin == std::string::npos) return {true, "Printer queue is available"};
    auto status = output.substr(statusBegin + 4U);
    while (!status.empty() && (status.back() == '\r' || status.back() == '\n')) status.pop_back();
    const auto normalized = lower(status);
    const bool ready = normalized.find("disabled") == std::string::npos &&
                       normalized.find("stopped") == std::string::npos;
    return {ready, status.empty() ? (ready ? "Printer queue is available" : "Printer queue is stopped") : status};
  }

  bool sendRawJob(const std::string& printerName, const std::string& jobName, const std::string& zpl,
                  std::string* error) const override {
    if (!validQueueName(printerName) || jobName.size() > 1024U || zpl.empty()) {
      if (error != nullptr) *error = "Printer job details are invalid";
      return false;
    }
    int inputPipe[2]{};
    if (pipe(inputPipe) != 0) {
      if (error != nullptr) *error = "Unable to create a CUPS printer job";
      return false;
    }
    const auto child = fork();
    if (child < 0) {
      close(inputPipe[0]);
      close(inputPipe[1]);
      if (error != nullptr) *error = "Unable to start the CUPS printer job";
      return false;
    }
    if (child == 0) {
      close(inputPipe[1]);
      if (dup2(inputPipe[0], STDIN_FILENO) < 0) _exit(127);
      close(inputPipe[0]);
      const int nullDevice = open("/dev/null", O_WRONLY);
      if (nullDevice >= 0) {
        dup2(nullDevice, STDOUT_FILENO);
        dup2(nullDevice, STDERR_FILENO);
        close(nullDevice);
      }
      execlp("lp", "lp", "-d", printerName.c_str(), "-o", "raw", "-t",
             (jobName.empty() ? "Inventatory label" : jobName.c_str()), static_cast<char*>(nullptr));
      _exit(127);
    }
    close(inputPipe[0]);
    const bool wrote = writeAll(inputPipe[1], zpl);
    close(inputPipe[1]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    const bool accepted = wrote && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!accepted && error != nullptr) {
      *error = wrote ? "CUPS did not accept the raw label job" : "The CUPS printer queue closed the label job early";
    }
    return accepted;
  }
};

}  // namespace

std::unique_ptr<PrinterBackend> createPlatformPrinterBackend() {
  return std::make_unique<CupsPrinterBackend>();
}

}  // namespace inventatory
