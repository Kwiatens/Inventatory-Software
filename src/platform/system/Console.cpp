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
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shell32.lib")

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

  const size_t byteCount = (text.size() + 1) * sizeof(char);
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, byteCount);
  if (handle == nullptr) {
    return false;
  }

  auto* buffer = static_cast<char*>(GlobalLock(handle));
  if (buffer == nullptr) {
    GlobalFree(handle);
    return false;
  }

  memcpy(buffer, text.c_str(), byteCount);
  GlobalUnlock(handle);

  if (SetClipboardData(CF_TEXT, handle) == nullptr) {
    GlobalFree(handle);
    return false;
  }

  return true;
}

bool openCsvFileDialog(filesystem::path& selectedPath) {
  char fileName[MAX_PATH] = {};
  OPENFILENAMEA dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = nullptr;
  dialog.lpstrFilter = "CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
  dialog.lpstrFile = fileName;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrTitle = "Select a DigiKey order CSV or KiCad BOM";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  dialog.lpstrDefExt = "csv";

  if (GetOpenFileNameA(&dialog) == 0) {
    return false;
  }

  selectedPath = filesystem::path(fileName);
  return true;
}

bool saveFileDialog(filesystem::path& selectedPath, const string& title, const string& filter,
                    const string& defaultExtension) {
  char fileName[MAX_PATH] = {};
  OPENFILENAMEA dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = nullptr;
  dialog.lpstrFilter = filter.c_str();
  dialog.lpstrFile = fileName;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrTitle = title.c_str();
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  dialog.lpstrDefExt = defaultExtension.c_str();

  if (GetSaveFileNameA(&dialog) == 0) {
    return false;
  }

  selectedPath = filesystem::path(fileName);
  return true;
}

bool openFolderDialog(filesystem::path& selectedPath, const string& title) {
  BROWSEINFOA browse{};
  browse.hwndOwner = nullptr;
  browse.lpszTitle = title.c_str();
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

  LPITEMIDLIST itemIdList = SHBrowseForFolderA(&browse);
  if (itemIdList == nullptr) {
    return false;
  }

  char pathBuffer[MAX_PATH] = {};
  const bool ok = SHGetPathFromIDListA(itemIdList, pathBuffer) != 0;
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

}  // namespace inventatory

