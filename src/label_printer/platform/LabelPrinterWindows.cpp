// Inventatory - Windows printer backend.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>

#pragma comment(lib, "Winspool.lib")
#endif

namespace inventatory {

using namespace std;
using namespace label_printer_detail;

namespace {

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

unique_ptr<PrinterBackend> createPlatformPrinterBackend() {
#ifdef _WIN32
  return make_unique<WindowsPrinterBackend>();
#else
  return nullptr;
#endif
}

}  // namespace inventatory
