// Inventatory - Notification-area lifetime control for the background scanner service.

#define WIN32_LEAN_AND_MEAN
#include "platform/system/BackgroundController.h"
#include "platform/system/StartupRegistration.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

namespace inventatory {

namespace {

constexpr wchar_t kWindowClass[] = L"InventatoryBackgroundControllerWindow";
constexpr wchar_t kWindowTitle[] = L"Inventatory Background Controller";
constexpr wchar_t kInteractiveMutexName[] = L"Local\\InventatorySoftware.InteractiveInstance";
constexpr wchar_t kBackgroundMutexName[] = L"Local\\InventatorySoftware.BackgroundService";
constexpr UINT kTrayMessage = WM_APP + 41;
constexpr UINT kHideMessage = WM_APP + 42;
constexpr UINT kStopMessage = WM_APP + 43;
constexpr UINT kRestoreMessage = WM_APP + 44;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT kOpenCommand = 1001;
constexpr UINT kQuitCommand = 1002;

BackgroundController* controllerFrom(HWND window) {
  return reinterpret_cast<BackgroundController*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

bool mutexExists(const wchar_t* name) {
  const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
  if (mutex == nullptr) return false;
  CloseHandle(mutex);
  return true;
}

std::wstring currentExecutablePath() {
  std::wstring path(MAX_PATH, L'\0');
  DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  while (length == path.size()) {
    path.resize(path.size() * 2);
    length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  }
  if (length == 0) return {};
  path.resize(length);
  return path;
}

bool consoleIsAvailable() {
  if (GetConsoleWindow() != nullptr) return true;
  const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  return output != nullptr && output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode) != 0;
}

bool ensureConsole() {
  if (consoleIsAvailable()) return true;
  const bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
  if (!attached && !AllocConsole()) return false;
  if (!attached) SetConsoleTitleW(L"Inventatory");

  FILE* stream = nullptr;
  freopen_s(&stream, "CONIN$", "r", stdin);
  freopen_s(&stream, "CONOUT$", "w", stdout);
  freopen_s(&stream, "CONOUT$", "w", stderr);
  return consoleIsAvailable();
}

void restoreConsole() {
  if (!ensureConsole()) return;
  if (const HWND console = GetConsoleWindow(); console != nullptr) {
    ShowWindow(console, SW_RESTORE);
    SetForegroundWindow(console);
  }
}

LRESULT CALLBACK controllerWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* controller = controllerFrom(window);
  switch (message) {
    case kTrayMessage:
      if (lParam == WM_LBUTTONUP) {
        if (controller != nullptr) controller->requestOpenFromTray();
        return 0;
      }
      if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kOpenCommand, L"Open Inventatory");
        AppendMenuW(menu, MF_STRING, kQuitCommand, L"Quit Inventatory");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(window);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        return 0;
      }
      break;
    case kHideMessage:
      if (controller != nullptr) controller->hideConsole(true);
      return 0;
    case kRestoreMessage:
      if (controller != nullptr) controller->requestOpenFromTray();
      return 0;
    case kStopMessage:
      if (controller != nullptr) controller->requestQuitFromTray();
      DestroyWindow(window);
      return 0;
    case WM_COMMAND:
      if (LOWORD(wParam) == kOpenCommand) {
        restoreConsole();
        return 0;
      }
      if (LOWORD(wParam) == kQuitCommand && controller != nullptr) {
        controller->requestQuitFromTray();
        return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

BOOL WINAPI consoleControlHandler(DWORD type) {
  if (type != CTRL_CLOSE_EVENT) return FALSE;
  const HWND window = FindWindowW(kWindowClass, kWindowTitle);
  if (window == nullptr) return FALSE;
  PostMessageW(window, kHideMessage, 0, 0);
  return TRUE;
}

}  // namespace

BackgroundController::~BackgroundController() {
  stop();
  if (instanceMutex_ != nullptr) CloseHandle(static_cast<HANDLE>(instanceMutex_));
}

bool BackgroundController::acquireSingleInstance(bool backgroundMode) {
  const auto* mutexName = backgroundMode ? kBackgroundMutexName : kInteractiveMutexName;
  const HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName);
  if (mutex == nullptr) return false;
  backgroundMode_ = backgroundMode;
  instanceMutex_ = mutex;
  return GetLastError() != ERROR_ALREADY_EXISTS;
}

bool BackgroundController::signalExistingInstance() const {
  const HWND window = FindWindowW(kWindowClass, kWindowTitle);
  return window != nullptr && PostMessageW(window, kRestoreMessage, 0, 0) != 0;
}

bool BackgroundController::backgroundServiceRunning() const {
  return mutexExists(kBackgroundMutexName);
}

bool BackgroundController::interactiveInstanceRunning() const {
  return mutexExists(kInteractiveMutexName);
}

bool BackgroundController::requestBackgroundServiceQuit() const {
  const HWND window = FindWindowW(kWindowClass, kWindowTitle);
  return window != nullptr && PostMessageW(window, kStopMessage, 0, 0) != 0;
}

bool BackgroundController::waitForBackgroundServiceToStop(int timeoutMs) const {
  const auto start = GetTickCount64();
  const auto timeout = timeoutMs < 0 ? 0ULL : static_cast<ULONGLONG>(timeoutMs);
  while (backgroundServiceRunning()) {
    if (GetTickCount64() - start >= timeout) return false;
    Sleep(10);
  }
  return true;
}

bool BackgroundController::restartAsBackgroundService() {
  if (backgroundMode_) return false;
  const auto executablePath = currentExecutablePath();
  if (executablePath.empty()) return false;
  const auto launcherPath = buildBackgroundStartupLauncherPath(executablePath);
  std::wstring commandLine = buildBackgroundStartupCommand(launcherPath);
  std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back(L'\0');

  if (instanceMutex_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(instanceMutex_));
    instanceMutex_ = nullptr;
  }

  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};
  if (!CreateProcessW(launcherPath.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startupInfo, &processInfo)) {
    return false;
  }
  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);
  return true;
}

bool BackgroundController::start(bool enabled, bool hideInitially, Callback onQuit, Callback onOpen) {
  if (!enabled) return true;
  if (!backgroundMode_ && backgroundServiceRunning()) return true;
  if (enabled_.exchange(true)) return true;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    onQuit_ = std::move(onQuit);
    onOpen_ = std::move(onOpen);
  }
  {
    std::lock_guard<std::mutex> lock(trayReadyMutex_);
    trayReady_ = false;
  }
  trayStartupCancelled_.store(false);
  trayThread_ = std::thread(&BackgroundController::trayThreadMain, this);
  {
    std::unique_lock<std::mutex> lock(trayReadyMutex_);
    if (!trayReadyChanged_.wait_for(lock, std::chrono::seconds(2), [this] { return trayReady_; })) {
      enabled_.store(false);
      trayStartupCancelled_.store(true);
      lock.unlock();
      if (const HWND window = trayWindow_.load(); window != nullptr) {
        PostMessageW(window, kStopMessage, 0, 0);
      }
      if (trayThread_.joinable()) trayThread_.join();
      trayWindow_.store(nullptr);
      std::lock_guard<std::mutex> callbackLock(callbackMutex_);
      onQuit_ = {};
      onOpen_ = {};
      return false;
    }
  }
  if (trayWindow_.load() == nullptr) {
    enabled_.store(false);
    trayStartupCancelled_.store(true);
    if (trayThread_.joinable()) trayThread_.join();
    trayWindow_.store(nullptr);
    std::lock_guard<std::mutex> callbackLock(callbackMutex_);
    onQuit_ = {};
    onOpen_ = {};
    return false;
  }
  SetConsoleCtrlHandler(consoleControlHandler, TRUE);
  if (hideInitially) hideConsole(false);
  return true;
}

void BackgroundController::stop() {
  const bool wasEnabled = enabled_.exchange(false);
  if (wasEnabled) SetConsoleCtrlHandler(consoleControlHandler, FALSE);
  if (const HWND window = trayWindow_.load(); window != nullptr) PostMessageW(window, kStopMessage, 0, 0);
  if (trayThread_.joinable()) trayThread_.join();
  trayWindow_.store(nullptr);
  std::lock_guard<std::mutex> lock(callbackMutex_);
  onQuit_ = {};
  onOpen_ = {};
}

bool BackgroundController::enabled() const {
  return enabled_.load();
}

void BackgroundController::hideConsole(bool notifyUser) {
  if (!enabled()) return;
  if (const HWND console = GetConsoleWindow(); console != nullptr) ShowWindow(console, SW_HIDE);
  if (notifyUser) {
    const HWND window = trayWindow_.load();
    if (window != nullptr) {
      NOTIFYICONDATAW icon{};
      icon.cbSize = sizeof(icon);
      icon.hWnd = window;
      icon.uID = kTrayIconId;
      icon.uFlags = NIF_INFO;
      wcscpy_s(icon.szInfoTitle, L"Inventatory is still running");
      wcscpy_s(icon.szInfo, L"Inventatory remains available for Scan R1 in the notification area.");
      icon.dwInfoFlags = NIIF_INFO;
      Shell_NotifyIconW(NIM_MODIFY, &icon);
    }
  }
}

void BackgroundController::showConsole() {
  restoreConsole();
}

void BackgroundController::requestQuitFromTray() {
  Callback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = onQuit_;
  }
  if (callback) callback();
}

void BackgroundController::requestOpenFromTray() {
  Callback callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = onOpen_;
  }
  if (callback) callback();
  restoreConsole();
}

void BackgroundController::trayThreadMain() {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW windowClass{};
  windowClass.hInstance = instance;
  windowClass.lpfnWndProc = controllerWindowProc;
  windowClass.lpszClassName = kWindowClass;
  RegisterClassW(&windowClass);
  HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                instance, this);
  trayWindow_.store(window);
  {
    std::lock_guard<std::mutex> lock(trayReadyMutex_);
    trayReady_ = true;
  }
  trayReadyChanged_.notify_all();
  if (window == nullptr || trayStartupCancelled_.load()) {
    if (window != nullptr) DestroyWindow(window);
    return;
  }

  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = window;
  icon.uID = kTrayIconId;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = kTrayMessage;
  icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
  wcscpy_s(icon.szTip, L"Inventatory Scan R1 service");
  Shell_NotifyIconW(NIM_ADD, &icon);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  Shell_NotifyIconW(NIM_DELETE, &icon);
}

}  // namespace inventatory
