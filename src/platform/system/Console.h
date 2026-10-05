#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::filesystem::path;
using std::string;
using std::vector;

struct ConsoleSize {
  int columns = 120;
  int rows = 40;
};

enum class KeyType {
  Character,
  Enter,
  Escape,
  Backspace,
  CtrlBackspace,
  CtrlZ,
  Tab,
  TabReverse,
  Up,
  Down,
  Left,
  Right,
  Home,
  End,
  PageUp,
  PageDown,
  Delete,
  Unknown,
};

struct KeyEvent {
  KeyType type = KeyType::Unknown;
  char ch = '\0';
};

class ConsoleSession {
 public:
  ConsoleSession();
  ~ConsoleSession();

  void restore();

  ConsoleSession(const ConsoleSession&) = delete;
  ConsoleSession& operator=(const ConsoleSession&) = delete;

 private:
  bool active_ = false;
  bool alternateScreen_ = false;
};

ConsoleSize consoleSize();
void clearConsole();
void hideCursor();
void showCursor();
void setConsoleTitle(const string& title);
void initializeConsoleWindow();
bool ensureTerminalAttached(int argc, char* argv[]);
// Asks the desktop to draw attention to this terminal window (bell / taskbar urgency).
void requestTerminalAttention();
// Keeps a short-lived terminal window readable: returns when Enter is pressed or after timeoutMs.
void waitForAcknowledgement(int timeoutMs);
#ifndef _WIN32
vector<string> buildTerminalCandidateArgs(const string& terminalBinary, const string& executable,
                                          const vector<string>& extraArgs = {});
#endif
bool openUrl(const string& url);
bool copyToClipboard(const string& text);
bool openCsvFileDialog(filesystem::path& selectedPath);
bool saveFileDialog(filesystem::path& selectedPath, const string& title, const string& filter,
                    const string& defaultExtension);
bool openFolderDialog(filesystem::path& selectedPath, const string& title);
vector<string> localAddresses();
vector<string> privateLocalAddresses();
bool controlModifierPressed();
vector<KeyEvent> pollKeys();

}  // namespace inventatory

