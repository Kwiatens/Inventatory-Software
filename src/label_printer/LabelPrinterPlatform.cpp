// Inventatory - Printer platform, configuration, and queue integration.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "label_printer/LabelPrinterPrivate.h"

#include "core/InventoryInternals.h"
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

string windowsErrorText(unsigned long code) {
#ifdef _WIN32
  if (code == 0) {
    return "Unknown error";
  }

  LPWSTR buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "Windows error " + to_string(code);
  }

  wstring wide(buffer, length);
  LocalFree(buffer);
  string result;
  result.reserve(wide.size());
  for (wchar_t ch : wide) {
    if (ch == L'\r' || ch == L'\n') {
      continue;
    }
    if (ch <= 0x7f) {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('?');
    }
  }
  return trim(result);
#else
  (void)code;
  return "Windows error";
#endif
}

string narrowFromWide(const wstring& value) {
#ifdef _WIN32
  if (value.empty()) {
    return {};
  }
  const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (sizeNeeded <= 0) {
    return {};
  }
  string result(static_cast<size_t>(sizeNeeded), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), sizeNeeded, nullptr, nullptr);
  return result;
#else
  return {};
#endif
}

wstring widenFromUtf8(const string& value) {
#ifdef _WIN32
  if (value.empty()) {
    return {};
  }
  const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
  if (sizeNeeded <= 0) {
    return {};
  }
  wstring result(static_cast<size_t>(sizeNeeded), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), sizeNeeded);
  return result;
#else
  return {};
#endif
}

#ifdef _WIN32
PrinterQueueInfo makePrinterInfo(const wstring& name, const wstring& driver, const wstring& port, DWORD status,
                                 bool isDefault) {
  PrinterQueueInfo info;
  info.name = narrowFromWide(name);
  info.driverName = narrowFromWide(driver);
  info.portName = narrowFromWide(port);
  info.isDefault = isDefault;

  vector<string> statusParts;
  if (status == 0) {
    info.isReady = true;
    statusParts.push_back("Ready");
  } else {
    if (status & PRINTER_STATUS_PAUSED) {
      statusParts.push_back("Paused");
    }
    if (status & PRINTER_STATUS_OFFLINE) {
      statusParts.push_back("Offline");
    }
    if (status & PRINTER_STATUS_ERROR) {
      statusParts.push_back("Error");
    }
    if (status & PRINTER_STATUS_PAPER_OUT) {
      statusParts.push_back("Label stock empty");
    }
    if (status & PRINTER_STATUS_NOT_AVAILABLE) {
      statusParts.push_back("Not available");
    }
    if (status & PRINTER_STATUS_NO_TONER) {
      statusParts.push_back("Media error");
    }
    if (status & PRINTER_STATUS_DOOR_OPEN) {
      statusParts.push_back("Door open");
    }
    info.isReady = statusParts.empty();
  }
  if (statusParts.empty()) {
    statusParts.push_back("Ready");
  }
  info.statusText = join(statusParts, ';');
  return info;
}

class WindowsPrinterBackend final : public PrinterBackend {
 public:
  vector<PrinterQueueInfo> enumeratePrinters() const override {
    vector<PrinterQueueInfo> printers;
    DWORD bytesNeeded = 0;
    DWORD count = 0;
    const DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    EnumPrintersW(flags, nullptr, 2, nullptr, 0, &bytesNeeded, &count);
    if (bytesNeeded == 0) {
      return printers;
    }

    vector<BYTE> buffer(bytesNeeded);
    if (!EnumPrintersW(flags, nullptr, 2, buffer.data(), bytesNeeded, &bytesNeeded, &count)) {
      return printers;
    }

    const auto* entries = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
    printers.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
      const auto& entry = entries[index];
      printers.push_back(makePrinterInfo(entry.pPrinterName ? entry.pPrinterName : L"",
                                        entry.pDriverName ? entry.pDriverName : L"",
                                        entry.pPortName ? entry.pPortName : L"", entry.Status,
                                        (entry.Attributes & PRINTER_ATTRIBUTE_DEFAULT) != 0));
    }

    sort(printers.begin(), printers.end(), [](const PrinterQueueInfo& lhs, const PrinterQueueInfo& rhs) {
      if (lhs.isDefault != rhs.isDefault) {
        return lhs.isDefault > rhs.isDefault;
      }
      return lowerAscii(lhs.name) < lowerAscii(rhs.name);
    });
    return printers;
  }

  PrinterCheckResult probePrinter(const string& printerName) const override {
    PrinterCheckResult result;
    if (trim(printerName).empty()) {
      result.message = "No printer configured";
      return result;
    }

    const auto wideName = widenFromUtf8(printerName);
    if (wideName.empty()) {
      result.message = "Printer name could not be converted";
      return result;
    }

    PRINTER_DEFAULTSW defaults{};
    defaults.DesiredAccess = PRINTER_ACCESS_USE;
    HANDLE handle = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(wideName.c_str()), &handle, &defaults)) {
      result.message = "Unable to open printer queue: " + windowsErrorText(GetLastError());
      return result;
    }

    unique_ptr<void, decltype(&ClosePrinter)> printerHandle(handle, ClosePrinter);
    DWORD bytesNeeded = 0;
    GetPrinterW(handle, 2, nullptr, 0, &bytesNeeded);
    if (bytesNeeded == 0) {
      result.ok = true;
      result.message = "Printer queue opened";
      return result;
    }

    vector<BYTE> buffer(bytesNeeded);
    if (!GetPrinterW(handle, 2, buffer.data(), bytesNeeded, &bytesNeeded)) {
      result.message = "Unable to query printer status: " + windowsErrorText(GetLastError());
      return result;
    }

    const auto* info = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
    const auto queue = makePrinterInfo(info->pPrinterName ? info->pPrinterName : L"",
                                       info->pDriverName ? info->pDriverName : L"",
                                       info->pPortName ? info->pPortName : L"", info->Status,
                                       (info->Attributes & PRINTER_ATTRIBUTE_DEFAULT) != 0);
    result.ok = queue.isReady;
    result.message = queue.statusText;
    return result;
  }

  bool sendRawJob(const string& printerName, const string& jobName, const string& zpl, string* error) const override {
    const auto wideName = widenFromUtf8(printerName);
    if (wideName.empty()) {
      if (error != nullptr) {
        *error = "Printer name could not be converted";
      }
      return false;
    }

    PRINTER_DEFAULTSW defaults{};
    defaults.DesiredAccess = PRINTER_ACCESS_USE;
    HANDLE handle = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(wideName.c_str()), &handle, &defaults)) {
      if (error != nullptr) {
        *error = "Unable to open printer queue: " + windowsErrorText(GetLastError());
      }
      return false;
    }

    unique_ptr<void, decltype(&ClosePrinter)> printerHandle(handle, ClosePrinter);

    DOC_INFO_1W doc{};
    const auto jobWide = widenFromUtf8(jobName);
    const auto rawWide = widenFromUtf8("RAW");
    doc.pDocName = const_cast<LPWSTR>(jobWide.empty() ? L"Inventatory Label" : jobWide.c_str());
    doc.pOutputFile = nullptr;
    doc.pDatatype = const_cast<LPWSTR>(rawWide.empty() ? L"RAW" : rawWide.c_str());

    if (StartDocPrinterW(handle, 1, reinterpret_cast<LPBYTE>(&doc)) == 0) {
      if (error != nullptr) {
        *error = "Unable to start printer job: " + windowsErrorText(GetLastError());
      }
      return false;
    }

    bool success = false;
    const auto endDoc = [&]() {
      EndDocPrinter(handle);
    };

    do {
      if (!StartPagePrinter(handle)) {
        if (error != nullptr) {
          *error = "Unable to start printer page: " + windowsErrorText(GetLastError());
        }
        break;
      }

      const auto body = zpl;
      DWORD bytesWritten = 0;
      if (!WritePrinter(handle, const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), &bytesWritten) ||
          bytesWritten != body.size()) {
        if (error != nullptr) {
          *error = "Unable to write print data: " + windowsErrorText(GetLastError());
        }
        break;
      }

      if (!EndPagePrinter(handle)) {
        if (error != nullptr) {
          *error = "Unable to finish printer page: " + windowsErrorText(GetLastError());
        }
        break;
      }

      success = true;
    } while (false);

    endDoc();
    return success;
  }
};
#endif
}  // namespace

LabelPrinterService::LabelPrinterService(unique_ptr<PrinterBackend> backend)
    : backend_(move(backend)) {
#ifdef _WIN32
  if (backend_ == nullptr) {
    backend_ = make_unique<WindowsPrinterBackend>();
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
