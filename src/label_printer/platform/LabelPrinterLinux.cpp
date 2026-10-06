// Inventatory - CUPS queue integration for Linux label printers.

#include "label_printer/core/LabelPrinterPrivate.h"
#include "label_printer/platform/CupsStatus.h"
#include "platform/system/ChildProcess.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace inventatory {
namespace {

// CUPS clients wait on cupsd or a remote server; a deadline keeps the single printer worker from
// stalling forever behind an unreachable queue.
constexpr std::chrono::seconds kQueryTimeout{10};
constexpr std::chrono::seconds kPrintTimeout{30};

bool runCommand(const std::vector<std::string>& arguments, std::string& output) {
  if (arguments.empty()) return false;
  ChildProcessOptions options;
  options.timeout = kQueryTimeout;
  options.mergeStderr = true;
  auto result = runChildProcess(arguments, options);
  output = std::move(result.output);
  if (result.outputTooLarge) output.clear();
  return result.succeeded();
}

bool validQueueName(const std::string& name) {
  if (name.empty() || name.size() > 1024U) return false;
  return std::none_of(name.begin(), name.end(), [](unsigned char value) {
    return value == 0 || value == '\r' || value == '\n';
  });
}

class CupsPrinterBackend final : public PrinterBackend {
 public:
  std::vector<PrinterQueueInfo> enumeratePrinters() const override {
    std::string output;
    if (!runCommand({"env", "LC_ALL=C", "lpstat", "-p", "-d"}, output)) return {};
    std::string defaultName;
    std::vector<PrinterQueueInfo> printers;
    size_t begin = 0;
    while (begin < output.size()) {
      const auto end = output.find('\n', begin);
      const auto line = output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
      if (line.rfind("system default destination: ", 0) == 0) {
        defaultName = line.substr(std::string("system default destination: ").size());
      } else if (const auto queue = parseCupsQueueLine(line)) {
        PrinterQueueInfo printer;
        printer.name = queue->name;
        printer.driverName = "CUPS";
        printer.statusText = queue->status;
        printer.isReady = queue->ready;
        printers.push_back(std::move(printer));
      }
      if (end == std::string::npos) break;
      begin = end + 1U;
    }
    for (auto& printer : printers) printer.isDefault = printer.name == defaultName;
    std::sort(printers.begin(), printers.end(), [](const auto& first, const auto& second) {
      if (first.isDefault != second.isDefault) return first.isDefault > second.isDefault;
      return toLower(first.name) < toLower(second.name);
    });
    return printers;
  }

  PrinterCheckResult probePrinter(const std::string& printerName) const override {
    if (!validQueueName(printerName)) return {false, "No printer configured"};
    std::string output;
    if (!runCommand({"env", "LC_ALL=C", "lpstat", "-p", printerName}, output)) {
      const auto reason = firstOutputLine(output);
      return {false, reason.empty() ? "Unable to query the CUPS printer queue" : reason};
    }
    size_t begin = 0;
    while (begin < output.size()) {
      const auto end = output.find('\n', begin);
      const auto line = output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
      if (const auto queue = parseCupsQueueLine(line)) {
        const std::string fallback = queue->ready ? "Printer queue is available" : "Printer queue is stopped";
        return {queue->ready, queue->status.empty() ? fallback : queue->status};
      }
      if (end == std::string::npos) break;
      begin = end + 1U;
    }
    return {true, "Printer queue is available"};
  }

  bool sendRawJob(const std::string& printerName, const std::string& jobName, const std::string& zpl,
                  std::string* error) const override {
    if (!validQueueName(printerName) || jobName.size() > 1024U || zpl.empty()) {
      if (error != nullptr) *error = "Printer job details are invalid";
      return false;
    }
    ChildProcessOptions options;
    options.timeout = kPrintTimeout;
    options.hasInput = true;
    options.input = zpl;
    const auto result = runChildProcess({"lp", "-d", printerName, "-o", "raw", "-t",
                                         jobName.empty() ? std::string("Inventatory label") : jobName},
                                        options);
    const bool wrote = result.started && !result.inputFailed;
    const bool accepted = result.succeeded();
    if (!accepted && error != nullptr) {
      if (result.timedOut) *error = "The CUPS printer queue did not accept the label job in time";
      else if (!result.started) *error = "Unable to start the CUPS printer job";
      else *error = wrote ? "CUPS did not accept the raw label job" : "The CUPS printer queue closed the label job early";
    }
    return accepted;
  }
};

}  // namespace

std::unique_ptr<PrinterBackend> createPlatformPrinterBackend() {
  return std::make_unique<CupsPrinterBackend>();
}

}  // namespace inventatory
