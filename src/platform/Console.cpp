#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/Console.h"

#include <algorithm>
#include <cstdint>
#include <conio.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
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

namespace hims {

using namespace std;

namespace {

bool ensureVirtualTerminal() {
  HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
    return false;
  }

  DWORD mode = 0;
  if (!GetConsoleMode(handle, &mode)) {
    return false;
  }

  mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  return SetConsoleMode(handle, mode) != 0;
}

void writeEscapeSequence(const char* sequence) {
  cout << sequence;
}

void setCursorVisibility(bool visible) {
  HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
    return;
  }

  CONSOLE_CURSOR_INFO info{};
  if (!GetConsoleCursorInfo(handle, &info)) {
    return;
  }

  info.bVisible = visible ? TRUE : FALSE;
  info.dwSize = 25;
  SetConsoleCursorInfo(handle, &info);
}

KeyEvent translateSpecialKey(int code) {
  switch (code) {
    case 72:
      return {KeyType::Up, '\0'};
    case 80:
      return {KeyType::Down, '\0'};
    case 75:
      return {KeyType::Left, '\0'};
    case 77:
      return {KeyType::Right, '\0'};
    case 71:
      return {KeyType::Home, '\0'};
    case 79:
      return {KeyType::End, '\0'};
    case 73:
      return {KeyType::PageUp, '\0'};
    case 81:
      return {KeyType::PageDown, '\0'};
    case 83:
      return {KeyType::Delete, '\0'};
    default:
      return {KeyType::Unknown, '\0'};
  }
}

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

ConsoleSession::ConsoleSession() {
  const bool vtEnabled = ensureVirtualTerminal();
  if (vtEnabled) {
    writeEscapeSequence("\x1b[?1049h");
    writeEscapeSequence("\x1b[2J\x1b[H");
    alternateScreen_ = true;
  } else {
    clearConsole();
  }
  hideCursor();
  setConsoleTitle("HIMS Terminal");
  active_ = true;
}

ConsoleSession::~ConsoleSession() {
  restore();
}

void ConsoleSession::restore() {
  if (!active_) {
    return;
  }

  showCursor();
  if (alternateScreen_) {
    writeEscapeSequence("\x1b[?1049l");
    cout << flush;
    alternateScreen_ = false;
  }
  active_ = false;
}

ConsoleSize consoleSize() {
  ConsoleSize result;
  CONSOLE_SCREEN_BUFFER_INFO info{};
  const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
    return result;
  }

  if (GetConsoleScreenBufferInfo(handle, &info)) {
    result.columns = info.srWindow.Right - info.srWindow.Left + 1;
    result.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  }

  return result;
}

void clearConsole() {
  cout << "\x1b[2J\x1b[3J\x1b[H";
}

void hideCursor() {
  setCursorVisibility(false);
}

void showCursor() {
  setCursorVisibility(true);
}

void setConsoleTitle(const string& title) {
  SetConsoleTitleA(title.c_str());
}

bool openUrl(const string& url) {
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
  dialog.lpstrTitle = "Select DigiKey order CSV";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  dialog.lpstrDefExt = "csv";

  if (GetOpenFileNameA(&dialog) == 0) {
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

vector<KeyEvent> pollKeys() {
  vector<KeyEvent> keys;
  while (_kbhit()) {
    const int first = _getch();
    if (first == 0 || first == 224) {
      const int second = _getch();
      keys.push_back(translateSpecialKey(second));
      continue;
    }

    switch (first) {
      case 13:
        keys.push_back({KeyType::Enter, '\0'});
        break;
      case 27:
        keys.push_back({KeyType::Escape, '\0'});
        break;
      case 8:
        keys.push_back({controlModifierPressed() ? KeyType::CtrlBackspace : KeyType::Backspace, '\0'});
        break;
      case 26:
        keys.push_back({KeyType::CtrlZ, '\0'});
        break;
      case 9:
        keys.push_back({KeyType::Tab, '\0'});
        break;
      default:
        if (first >= 32 && first <= 126) {
          keys.push_back({KeyType::Character, static_cast<char>(first)});
        } else {
          keys.push_back({KeyType::Unknown, '\0'});
        }
        break;
    }
  }

  return keys;
}

}  // namespace hims

