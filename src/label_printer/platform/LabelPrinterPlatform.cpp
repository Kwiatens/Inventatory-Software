// Inventatory - Printer platform, configuration, and queue integration.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "core/storage/AtomicFile.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>

#pragma comment(lib, "Winspool.lib")
#endif

namespace inventatory {

using namespace std;
using namespace label_printer_detail;

namespace {
constexpr size_t kMaximumPrinterNameBytes = 1024;
constexpr uintmax_t kMaximumPrinterConfigBytes = 4096;

bool isValidPrinterName(const string& value) {
  if (value.size() > kMaximumPrinterNameBytes) {
    return false;
  }
  for (const unsigned char ch : value) {
    if (ch == '\0' || ch == '\r' || ch == '\n') {
      return false;
    }
  }
  return true;
}

}  // namespace

LabelPrinterService::LabelPrinterService(unique_ptr<PrinterBackend> backend)
    : backend_(move(backend)) {
#if defined(_WIN32) || defined(__linux__)
  if (backend_ == nullptr) {
    backend_ = createPlatformPrinterBackend();
  }
#endif
}

LabelPrinterService::~LabelPrinterService() = default;

vector<PrinterQueueInfo> LabelPrinterService::enumeratePrinters() const {
  if (backend_ == nullptr) {
    return {};
  }
  return backend_->enumeratePrinters();
}

bool LabelPrinterService::loadConfig(const filesystem::path& path) {
  persistedPath_ = path;
  persistedPrinter_.clear();
  printerChosenSinceLoad_ = false;
  // Anything but a clean read leaves the file as it is: a missing file is simply "no printer", while one
  // that exists but cannot be read or parsed is kept for the user to repair rather than overwritten.
  configFileState_ = ConfigFileState::Unreadable;
  try {
    error_code sizeError;
    const auto configSize = filesystem::file_size(path, sizeError);
    if (sizeError == make_error_code(errc::no_such_file_or_directory)) {
      configuredPrinter_.clear();
      configFileState_ = ConfigFileState::Missing;
      return false;
    }
    if (sizeError || configSize > kMaximumPrinterConfigBytes) {
      configuredPrinter_.clear();
      return false;
    }

    ifstream input(path, ios::binary);
    if (!input) {
      configuredPrinter_.clear();
      return false;
    }

    string value;
    if (!(input >> quoted(value))) {
      configuredPrinter_.clear();
      return false;
    }

    input >> ws;
    const string trimmedValue = trim(value);
    if (input.bad() || !input.eof() || trimmedValue.empty() || !isValidPrinterName(trimmedValue)) {
      configuredPrinter_.clear();
      return false;
    }

    configuredPrinter_ = trimmedValue;
    configPath_ = path;
    persistedPrinter_ = trimmedValue;
    configFileState_ = ConfigFileState::InSync;
    return true;
  } catch (...) {
    configuredPrinter_.clear();
    return false;
  }
}

bool LabelPrinterService::saveConfig(const filesystem::path& path) const {
  try {
    if (path.empty()) {
      return false;
    }
    const string printerName = trim(configuredPrinter_);
    if (!isValidPrinterName(printerName)) {
      return false;
    }

    ostringstream serialized;
    serialized << quoted(printerName) << '\n';
    if (!serialized) {
      return false;
    }
    const string contents = serialized.str();
    if (contents.size() > kMaximumPrinterConfigBytes) {
      return false;
    }

    if (persistedPath_ == path) {
      error_code existsError;
      switch (configFileState_) {
        case ConfigFileState::Unreadable:
          // Keep the original for the user to repair unless a printer was chosen since.
          if (!printerChosenSinceLoad_) return true;
          break;
        case ConfigFileState::Missing:
          if (printerName.empty()) return true;
          break;
        case ConfigFileState::InSync:
          if (printerName == persistedPrinter_ && filesystem::exists(path, existsError)) return true;
          break;
        case ConfigFileState::Unknown:
          break;
      }
    }

    string writeError;
    if (!writeFileAtomically(path, contents, &writeError)) {
      return false;
    }
    persistedPath_ = path;
    persistedPrinter_ = printerName;
    configFileState_ = ConfigFileState::InSync;
    printerChosenSinceLoad_ = false;
    return true;
  } catch (...) {
    return false;
  }
}

void LabelPrinterService::setConfiguredPrinter(string printerName) {
  configuredPrinter_ = trim(printerName);
  printerChosenSinceLoad_ = true;
}

const string& LabelPrinterService::configuredPrinter() const {
  return configuredPrinter_;
}

bool LabelPrinterService::hasConfiguredPrinter() const {
  return !trim(configuredPrinter_).empty();
}

optional<PrinterQueueInfo> LabelPrinterService::configuredPrinterInfo() const {
  if (!hasConfiguredPrinter()) {
    return nullopt;
  }

  const auto printers = enumeratePrinters();
  const auto needle = toLower(trim(configuredPrinter_));
  for (const auto& printer : printers) {
    if (toLower(trim(printer.name)) == needle) {
      return printer;
    }
  }
  return nullopt;
}

PrinterCheckResult LabelPrinterService::probeConfiguredPrinter() const {
  if (backend_ == nullptr) {
    return {false, "Printer backend unavailable"};
  }
  return backend_->probePrinter(configuredPrinter_);
}

}  // namespace inventatory
