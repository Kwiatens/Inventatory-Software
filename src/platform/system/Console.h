// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::filesystem::path;
using std::string;
using std::vector;

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
  // Complete UTF-8 sequence of a typed character; `ch` is set only for a single ASCII byte.
  string text;
};

// Appends a typed character to an edit buffer without splitting UTF-8.
inline void appendKeyText(string& buffer, const KeyEvent& key) {
  if (!key.text.empty()) {
    buffer += key.text;
  } else if (key.ch != '\0') {
    buffer.push_back(key.ch);
  }
}

// Removes the last UTF-8 encoded character, never leaving a partial sequence.
inline void eraseLastCharacter(string& buffer) {
  while (!buffer.empty() && (static_cast<unsigned char>(buffer.back()) & 0xC0U) == 0x80U) buffer.pop_back();
  if (!buffer.empty()) buffer.pop_back();
}

void setConsoleTitle(const string& title);
void initializeConsoleWindow();
bool ensureTerminalAttached(int argc, char* argv[]);
// Asks the desktop to draw attention to this terminal window (bell / taskbar urgency).
void requestTerminalAttention();
// Returns on Enter or after timeoutMs, so a short-lived terminal window stays readable.
void waitForAcknowledgement(int timeoutMs);
#ifndef _WIN32
vector<string> buildTerminalCandidateArgs(const string& terminalBinary, const string& executable,
                                          const vector<string>& extraArgs = {});
#endif
// An https:// link with a host, no control characters and nothing a shell or launcher could
// misread. openUrl refuses anything else.
inline bool openableUrl(const string& url) {
  constexpr size_t kMaximumUrlLength = 2048;
  if (url.size() > kMaximumUrlLength || url.rfind("https://", 0) != 0) return false;
  for (const unsigned char character : url) {
    if (character < 0x20U || character == 0x7fU) return false;
  }
  const auto hostBegin = sizeof("https://") - 1U;
  return url.find_first_of("/?#", hostBegin) != hostBegin &&
         url.find_first_of("\\\"<>|", hostBegin) == string::npos;
}
bool openUrl(const string& url);
bool copyToClipboard(const string& text);
bool openCsvFileDialog(filesystem::path& selectedPath);
bool saveFileDialog(filesystem::path& selectedPath, const string& title, const string& filter,
                    const string& defaultExtension);
bool openFolderDialog(filesystem::path& selectedPath, const string& title);
vector<string> privateLocalAddresses();
bool controlModifierPressed();

}  // namespace inventatory

