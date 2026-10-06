#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/system/Console.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Propsys.lib")

namespace inventatory {

using namespace std;

namespace {

string ipv4ToString(const sockaddr_in& address) {
  char buffer[INET_ADDRSTRLEN] = {};
  if (InetNtopA(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) {
    return {};
  }
  return buffer;
}

bool privateIpv4(uint32_t hostOrder) {
  const auto first = (hostOrder >> 24U) & 0xffU;
  const auto second = (hostOrder >> 16U) & 0xffU;
  return first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
         (first == 192U && second == 168U) || (first == 169U && second == 254U);
}

// UTF-8 to UTF-16. An empty result for a non-empty input means the text was not valid UTF-8.
wstring widenUtf8(const string& value) {
  if (value.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) return {};
  wstring result(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count) <= 0) {
    return {};
  }
  return result;
}

// Dialogs such as SHBrowseForFolder need COM on the calling thread. A thread that already runs in a
// different apartment keeps working (CoInitializeEx then fails and nothing is uninitialized).
struct ComApartment {
  ComApartment() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), shouldUninitialize(SUCCEEDED(result)) {}
  ~ComApartment() {
    if (shouldUninitialize) CoUninitialize();
  }
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  HRESULT result;
  bool shouldUninitialize;
};

// Places a DWORD on the open clipboard under a registered format name.
bool setClipboardDword(const wchar_t* formatName, DWORD value) {
  const UINT format = RegisterClipboardFormatW(formatName);
  if (format == 0) return false;
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  if (handle == nullptr) return false;
  auto* buffer = static_cast<DWORD*>(GlobalLock(handle));
  if (buffer == nullptr) {
    GlobalFree(handle);
    return false;
  }
  *buffer = value;
  GlobalUnlock(handle);
  if (SetClipboardData(format, handle) == nullptr) {
    GlobalFree(handle);
    return false;
  }
  return true;
}

}  // namespace

bool controlModifierPressed() {
  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
         (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
         (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0 ||
         (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
         (GetKeyState(VK_LCONTROL) & 0x8000) != 0 ||
         (GetKeyState(VK_RCONTROL) & 0x8000) != 0;
}

bool openUrl(const string& url) {
  constexpr size_t kMaximumUrlLength = 2048;
  if (url.size() > kMaximumUrlLength || url.rfind("https://", 0) != 0) {
    return false;
  }
  for (const unsigned char character : url) {
    if (character < 0x20U || character == 0x7fU) {
      return false;
    }
  }
  const auto hostBegin = sizeof("https://") - 1U;
  const auto hostEnd = url.find_first_of("/?#", hostBegin);
  if (hostEnd == hostBegin || url.find_first_of("\\\"<>|", hostBegin) != string::npos) {
    return false;
  }
  const auto result = reinterpret_cast<intptr_t>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
}

bool copyToClipboard(const string& text) {
  // The text is UTF-16 (CF_UNICODETEXT) so non-ASCII text is not mangled by the ANSI code page.
  const wstring wideText = widenUtf8(text);
  if (wideText.empty() && !text.empty()) {
    return false;
  }

  if (!OpenClipboard(nullptr)) {
    return false;
  }

  struct ClipboardGuard {
    ~ClipboardGuard() {
      CloseClipboard();
    }
  } guard;

  if (!EmptyClipboard()) {
    return false;
  }

  const size_t byteCount = (wideText.size() + 1) * sizeof(wchar_t);
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, byteCount);
  if (handle == nullptr) {
    return false;
  }

  auto* buffer = static_cast<wchar_t*>(GlobalLock(handle));
  if (buffer == nullptr) {
    GlobalFree(handle);
    return false;
  }

  memcpy(buffer, wideText.c_str(), byteCount);
  GlobalUnlock(handle);

  if (SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
    GlobalFree(handle);
    return false;
  }

  // Best effort: the copied text can be a pairing secret, so keep it out of Windows clipboard
  // history and cloud clipboard sync. A failure here does not fail the copy itself.
  setClipboardDword(L"ExcludeClipboardContentFromMonitorProcessing", 1U);
  setClipboardDword(L"CanIncludeInClipboardHistory", 0U);
  setClipboardDword(L"CanUploadToCloudClipboard", 0U);

  return true;
}

bool openCsvFileDialog(filesystem::path& selectedPath) {
  const wchar_t filter[] = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
  wstring fileName(32768U, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = nullptr;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = fileName.data();
  dialog.nMaxFile = static_cast<DWORD>(fileName.size());
  dialog.lpstrTitle = L"Select a DigiKey order CSV or KiCad BOM";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  dialog.lpstrDefExt = L"csv";

  if (GetOpenFileNameW(&dialog) == 0) {
    return false;
  }

  selectedPath = filesystem::path(fileName.c_str());
  return true;
}

bool saveFileDialog(filesystem::path& selectedPath, const string& title, const string& filter,
                    const string& defaultExtension) {
  wstring fileName(32768U, L'\0');
  const wstring& initialPath = selectedPath.native();
  if (!initialPath.empty() && initialPath.size() < fileName.size()) {
    copy(initialPath.begin(), initialPath.end(), fileName.begin());
  }
  // The filter is a double-NUL-terminated list; the caller's string already ends with one NUL and
  // wstring::c_str() supplies the second.
  const wstring wideFilter = widenUtf8(filter);
  const wstring wideTitle = widenUtf8(title);
  const wstring wideExtension = widenUtf8(defaultExtension);
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = nullptr;
  dialog.lpstrFilter = wideFilter.empty() ? nullptr : wideFilter.c_str();
  dialog.lpstrFile = fileName.data();
  dialog.nMaxFile = static_cast<DWORD>(fileName.size());
  dialog.lpstrTitle = wideTitle.c_str();
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  dialog.lpstrDefExt = wideExtension.c_str();

  if (GetSaveFileNameW(&dialog) == 0) {
    return false;
  }

  selectedPath = filesystem::path(fileName.c_str());
  return true;
}

bool openFolderDialog(filesystem::path& selectedPath, const string& title) {
  const ComApartment apartment;
  const wstring wideTitle = widenUtf8(title);
  BROWSEINFOW browse{};
  browse.hwndOwner = nullptr;
  browse.lpszTitle = wideTitle.c_str();
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

  LPITEMIDLIST itemIdList = SHBrowseForFolderW(&browse);
  if (itemIdList == nullptr) {
    return false;
  }

  wchar_t pathBuffer[MAX_PATH] = {};
  const bool ok = SHGetPathFromIDListW(itemIdList, pathBuffer) != 0;
  CoTaskMemFree(itemIdList);
  if (!ok) {
    return false;
  }

  selectedPath = filesystem::path(pathBuffer);
  return true;
}

vector<string> localAddresses() {
  vector<string> addresses;

  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return addresses;
  }

  char hostName[256] = {};
  if (gethostname(hostName, sizeof(hostName)) == SOCKET_ERROR) {
    WSACleanup();
    return addresses;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  if (getaddrinfo(hostName, nullptr, &hints, &result) == 0) {
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
      const auto* address = reinterpret_cast<sockaddr_in*>(current->ai_addr);
      const auto ip = ipv4ToString(*address);
      if (!ip.empty() && ip != "127.0.0.1" &&
          find(addresses.begin(), addresses.end(), ip) == addresses.end()) {
        addresses.push_back(ip);
      }
    }
    freeaddrinfo(result);
  }

  WSACleanup();

  if (addresses.empty()) {
    addresses.push_back("127.0.0.1");
  }

  return addresses;
}

vector<string> privateLocalAddresses() {
  vector<string> addresses;

  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return addresses;

  char hostName[256] = {};
  if (gethostname(hostName, sizeof(hostName)) == SOCKET_ERROR) {
    WSACleanup();
    return addresses;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  if (getaddrinfo(hostName, nullptr, &hints, &result) == 0) {
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
      const auto* address = reinterpret_cast<const sockaddr_in*>(current->ai_addr);
      if (!privateIpv4(ntohl(address->sin_addr.s_addr))) continue;
      const auto ip = ipv4ToString(*address);
      if (!ip.empty() && find(addresses.begin(), addresses.end(), ip) == addresses.end()) addresses.push_back(ip);
    }
    freeaddrinfo(result);
  }

  WSACleanup();
  return addresses;
}

void setConsoleTitle(const string& title) {
  SetConsoleTitleA(title.c_str());
}

void requestTerminalAttention() {
  // The existing console window is restored by BackgroundController::requestOpenFromTray.
}

void waitForAcknowledgement(int timeoutMs) {
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  if (input == nullptr || input == INVALID_HANDLE_VALUE) return;
  DWORD mode = 0;
  if (GetConsoleMode(input, &mode) == 0) return;
  FlushConsoleInputBuffer(input);
  for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < static_cast<ULONGLONG>(timeoutMs);) {
    if (WaitForSingleObject(input, 100) != WAIT_OBJECT_0) continue;
    INPUT_RECORD record{};
    DWORD count = 0;
    if (ReadConsoleInputW(input, &record, 1, &count) != 0 && count == 1 && record.EventType == KEY_EVENT &&
        record.Event.KeyEvent.bKeyDown != 0 && record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
      return;
    }
  }
}

void initializeConsoleWindow() {
  SetCurrentProcessExplicitAppUserModelID(L"Kwiatens.Inventatory");
  setConsoleTitle("Inventatory");
  const HWND console = GetConsoleWindow();
  if (console == nullptr) return;

  IPropertyStore* windowStore = nullptr;
  if (SUCCEEDED(SHGetPropertyStoreForWindow(console, IID_PPV_ARGS(&windowStore))) && windowStore != nullptr) {
    PROPVARIANT pv;
    if (SUCCEEDED(InitPropVariantFromString(L"Kwiatens.Inventatory", &pv))) {
      windowStore->SetValue(PKEY_AppUserModel_ID, pv);
      PropVariantClear(&pv);
    }
    windowStore->Commit();
    windowStore->Release();
  }

  const auto moduleHandle = GetModuleHandleW(nullptr);
  const auto smallX = GetSystemMetrics(SM_CXSMICON);
  const auto smallY = GetSystemMetrics(SM_CYSMICON);
  auto* smallIcon = static_cast<HICON>(
      LoadImageW(moduleHandle, MAKEINTRESOURCEW(1), IMAGE_ICON, smallX, smallY, LR_SHARED));
  if (smallIcon == nullptr) {
    smallIcon = LoadIconW(moduleHandle, MAKEINTRESOURCEW(1));
  }
  if (smallIcon != nullptr) {
    SendMessageW(console, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
  }

  const auto bigX = GetSystemMetrics(SM_CXICON);
  const auto bigY = GetSystemMetrics(SM_CYICON);
  auto* bigIcon = static_cast<HICON>(
      LoadImageW(moduleHandle, MAKEINTRESOURCEW(1), IMAGE_ICON, bigX, bigY, LR_SHARED));
  if (bigIcon == nullptr) {
    bigIcon = LoadIconW(moduleHandle, MAKEINTRESOURCEW(1));
  }
  if (bigIcon != nullptr) {
    SendMessageW(console, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
  }
}

bool ensureTerminalAttached(int /*argc*/, char* /*argv*/[]) {
  return true;
}

}  // namespace inventatory

