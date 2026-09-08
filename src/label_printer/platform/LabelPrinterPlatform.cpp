// Inventatory - Printer platform, configuration, and queue integration.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
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

filesystem::path temporaryConfigPath(const filesystem::path& path, uint64_t sequence) {
  static const auto processSeed =
      static_cast<uint64_t>(chrono::steady_clock::now().time_since_epoch().count());
  static atomic<uint64_t> nextSequence{0};

  filesystem::path candidate = path;
  candidate += ".tmp-" + to_string(processSeed) + "-" + to_string(sequence) + "-" +
               to_string(nextSequence.fetch_add(1, memory_order_relaxed));
  return candidate;
}

#ifdef _WIN32
void closeConfigHandle(void* rawHandle) noexcept {
  if (rawHandle != nullptr) {
    CloseHandle(static_cast<HANDLE>(rawHandle));
  }
}

bool writeTemporaryConfig(const filesystem::path& path, const string& contents, bool& created) {
  created = false;
  HANDLE rawHandle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (rawHandle == INVALID_HANDLE_VALUE) {
    return false;
  }

  unique_ptr<void, decltype(&closeConfigHandle)> handle(reinterpret_cast<void*>(rawHandle), closeConfigHandle);
  created = true;
  size_t written = 0;
  while (written < contents.size()) {
    const DWORD requested = static_cast<DWORD>(min<size_t>(contents.size() - written, numeric_limits<DWORD>::max()));
    DWORD actual = 0;
    if (!WriteFile(static_cast<HANDLE>(handle.get()), contents.data() + written, requested, &actual, nullptr) ||
        actual == 0 || actual != requested) {
      return false;
    }
    written += actual;
  }

  if (!FlushFileBuffers(static_cast<HANDLE>(handle.get()))) {
    return false;
  }

  HANDLE handleToClose = static_cast<HANDLE>(handle.release());
  return CloseHandle(handleToClose) != 0;
}
#else
bool writeTemporaryConfig(const filesystem::path& path, const string& contents, bool& created) {
  created = false;
  ofstream output(path, ios::binary | ios::out | ios::trunc);
  if (!output) {
    return false;
  }
  created = true;
  output.write(contents.data(), static_cast<streamsize>(contents.size()));
  if (!output) {
    return false;
  }
  output.flush();
  if (!output) {
    return false;
  }
  output.close();
  return static_cast<bool>(output);
}
#endif

}  // namespace

LabelPrinterService::LabelPrinterService(unique_ptr<PrinterBackend> backend)
    : backend_(move(backend)) {
#ifdef _WIN32
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
  try {
    error_code sizeError;
    const auto configSize = filesystem::file_size(path, sizeError);
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
    return true;
  } catch (...) {
    configuredPrinter_.clear();
    return false;
  }
}

bool LabelPrinterService::saveConfig(const filesystem::path& path) const {
  filesystem::path temporary;
  bool temporaryOwned = false;
  const auto removeTemporary = [&] {
    if (temporaryOwned) {
      error_code cleanupError;
      filesystem::remove(temporary, cleanupError);
    }
  };

  try {
    if (path.empty()) {
      return false;
    }
    const string printerName = trim(configuredPrinter_);
    if (!isValidPrinterName(printerName)) {
      return false;
    }

    const auto parent = path.parent_path();
    error_code filesystemError;
    if (!parent.empty()) {
      filesystem::create_directories(parent, filesystemError);
      if (filesystemError) {
        return false;
      }
      if (!filesystem::is_directory(parent, filesystemError) || filesystemError) {
        return false;
      }
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

    for (uint64_t attempt = 0; attempt < 32; ++attempt) {
      temporary = temporaryConfigPath(path, attempt);
      filesystemError.clear();
      if (!filesystem::exists(temporary, filesystemError)) {
        if (filesystemError) {
          return false;
        }
        break;
      }
      if (filesystemError) {
        return false;
      }
      temporary.clear();
    }
    if (temporary.empty()) {
      return false;
    }

    if (!writeTemporaryConfig(temporary, contents, temporaryOwned)) {
      removeTemporary();
      return false;
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      removeTemporary();
      return false;
    }
#else
    filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
      removeTemporary();
      return false;
    }
#endif
    temporaryOwned = false;
    return true;
  } catch (...) {
    removeTemporary();
    return false;
  }
}

void LabelPrinterService::setConfiguredPrinter(string printerName) {
  configuredPrinter_ = trim(printerName);
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
  const auto needle = lowerAscii(trim(configuredPrinter_));
  for (const auto& printer : printers) {
    if (lowerAscii(trim(printer.name)) == needle) {
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
