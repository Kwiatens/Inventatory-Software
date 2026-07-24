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
bool openUrl(const string& url);
bool copyToClipboard(const string& text);
bool openCsvFileDialog(filesystem::path& selectedPath);
bool openCatalogueFileDialog(filesystem::path& selectedPath);
bool openFolderDialog(filesystem::path& selectedPath, const string& title);
vector<string> localAddresses();
bool controlModifierPressed();
bool controlModifierPressed();
vector<KeyEvent> pollKeys();

}  // namespace inventatory

