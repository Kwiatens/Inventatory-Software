// Inventatory - Linux desktop and terminal integration.

#include "platform/system/Console.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace inventatory {
using namespace std;

namespace {

bool executableAvailable(const char* name) {
  const char* path = getenv("PATH");
  if (path == nullptr) return false;
  string search(path);
  size_t begin = 0;
  while (begin <= search.size()) {
    const auto end = search.find(':', begin);
    const auto directory = search.substr(begin, end == string::npos ? string::npos : end - begin);
    const auto candidate = (directory.empty() ? string(".") : directory) + "/" + name;
    if (access(candidate.c_str(), X_OK) == 0) return true;
    if (end == string::npos) break;
    begin = end + 1U;
  }
  return false;
}

bool runCaptured(const string& executable, const vector<string>& arguments, string& output) {
  int descriptors[2]{};
  if (pipe(descriptors) != 0) return false;
  const pid_t child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  if (child == 0) {
    close(descriptors[0]);
    if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(127);
    close(descriptors[1]);
    vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(executable.c_str(), argv.data());
    _exit(127);
  }
  close(descriptors[1]);
  output.clear();
  char buffer[4096];
  for (;;) {
    const auto count = read(descriptors[0], buffer, sizeof(buffer));
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      close(descriptors[0]);
      waitpid(child, nullptr, 0);
      return false;
    }
    output.append(buffer, static_cast<size_t>(count));
    if (output.size() > 32768U) {
      close(descriptors[0]);
      waitpid(child, nullptr, 0);
      output.clear();
      return false;
    }
  }
  close(descriptors[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool spawnDetached(const string& executable, const string& argument) {
  if (!executableAvailable(executable.c_str())) return false;
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    const pid_t detached = fork();
    if (detached < 0) _exit(127);
    if (detached > 0) _exit(0);
    setsid();
    const int nullDevice = open("/dev/null", O_RDWR);
    if (nullDevice >= 0) {
      dup2(nullDevice, STDIN_FILENO);
      dup2(nullDevice, STDOUT_FILENO);
      dup2(nullDevice, STDERR_FILENO);
      if (nullDevice > STDERR_FILENO) close(nullDevice);
    }
    execlp(executable.c_str(), executable.c_str(), argument.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class SigpipeBlock final {
 public:
  SigpipeBlock() {
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    valid_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    if (valid_) {
      sigset_t pending{};
      if (sigpending(&pending) == 0) hadPending_ = sigismember(&pending, SIGPIPE) == 1;
    }
  }

  ~SigpipeBlock() {
    if (!valid_) return;
    if (!hadPending_) {
      sigset_t pending{};
      if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        timespec noWait{};
        sigtimedwait(&blocked_, nullptr, &noWait);
      }
    }
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }

 private:
  sigset_t blocked_{};
  sigset_t previous_{};
  bool valid_ = false;
  bool hadPending_ = false;
};

bool runClipboard(const char* executable, const vector<string>& arguments, const string& text) {
  int descriptors[2]{};
  if (pipe(descriptors) != 0) return false;
  const pid_t child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  if (child == 0) {
    close(descriptors[1]);
    if (dup2(descriptors[0], STDIN_FILENO) < 0) _exit(127);
    close(descriptors[0]);
    int nullDevice = open("/dev/null", O_WRONLY);
    if (nullDevice >= 0) {
      dup2(nullDevice, STDOUT_FILENO);
      dup2(nullDevice, STDERR_FILENO);
      close(nullDevice);
    }
    vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>(executable));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(executable, argv.data());
    _exit(127);
  }
  close(descriptors[0]);
  SigpipeBlock sigpipe;
  size_t offset = 0;
  while (offset < text.size()) {
    const auto count = write(descriptors[1], text.data() + offset, text.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      close(descriptors[1]);
      waitpid(child, nullptr, 0);
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  close(descriptors[1]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool runFileChooser(const vector<string>& arguments, filesystem::path& selectedPath) {
  string output;
  if (!executableAvailable("zenity") || !runCaptured("zenity", arguments, output)) return false;
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
  if (output.empty()) return false;
  selectedPath = filesystem::u8path(output);
  return true;
}

}  // namespace

ConsoleSession::ConsoleSession() : active_(true) {}
ConsoleSession::~ConsoleSession() { restore(); }

void ConsoleSession::restore() {
  if (!active_) return;
  showCursor();
  if (alternateScreen_) fputs("\033[?1049l", stdout);
  fflush(stdout);
  active_ = false;
  alternateScreen_ = false;
}

ConsoleSize consoleSize() {
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
    return {size.ws_col, size.ws_row};
  }
  return {};
}

void clearConsole() { fputs("\033[2J\033[H", stdout); fflush(stdout); }
void hideCursor() { fputs("\033[?25l", stdout); fflush(stdout); }
void showCursor() { fputs("\033[?25h", stdout); fflush(stdout); }

void setConsoleTitle(const string& title) {
  string safeTitle;
  safeTitle.reserve(min<size_t>(title.size(), 256U));
  for (const unsigned char ch : title) {
    if (ch >= 0x20U && ch != 0x7fU && safeTitle.size() < 256U) safeTitle.push_back(static_cast<char>(ch));
  }
  fputs("\033]0;", stdout);
  fwrite(safeTitle.data(), 1U, safeTitle.size(), stdout);
  fputs("\007", stdout);
  fflush(stdout);
}

bool openUrl(const string& url) {
  constexpr size_t kMaximumUrlLength = 2048;
  if (url.size() > kMaximumUrlLength || url.rfind("https://", 0) != 0) return false;
  for (const unsigned char character : url) {
    if (character < 0x20U || character == 0x7fU) return false;
  }
  const auto hostBegin = sizeof("https://") - 1U;
  const auto hostEnd = url.find_first_of("/?#", hostBegin);
  if (hostEnd == hostBegin || url.find_first_of("\\\"<>|", hostBegin) != string::npos) return false;
  return spawnDetached("xdg-open", url);
}

bool copyToClipboard(const string& text) {
  if (executableAvailable("wl-copy") && runClipboard("wl-copy", {}, text)) return true;
  if (executableAvailable("xclip") && runClipboard("xclip", {"-selection", "clipboard", "-in"}, text)) return true;
  return executableAvailable("xsel") && runClipboard("xsel", {"--clipboard", "--input"}, text);
}

bool openCsvFileDialog(filesystem::path& selectedPath) {
  return runFileChooser({"--file-selection", "--title=Select a DigiKey order CSV or KiCad BOM",
                         "--file-filter=CSV and KiCad files | *.csv *.xml *.kicad_pcb *.pos"}, selectedPath);
}

bool saveFileDialog(filesystem::path& selectedPath, const string& title, const string& filter,
                    const string& defaultExtension) {
  (void)filter;
  const string initial = selectedPath.empty()
                             ? (defaultExtension.empty() ? string() : "Inventatory." + defaultExtension)
                             : selectedPath.string();
  vector<string> arguments{"--file-selection", "--save", "--confirm-overwrite", "--title=" + title};
  if (!initial.empty()) arguments.push_back("--filename=" + initial);
  if (!runFileChooser(arguments, selectedPath)) return false;
  if (!defaultExtension.empty() && selectedPath.extension().empty()) {
    selectedPath += filesystem::u8path("." + defaultExtension);
  }
  return true;
}

bool openFolderDialog(filesystem::path& selectedPath, const string& title) {
  return runFileChooser({"--file-selection", "--directory", "--title=" + title}, selectedPath);
}

vector<string> localAddresses() {
  vector<string> addresses;
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return addresses;
  for (auto* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
        entry->ifa_addr->sa_family != AF_INET) continue;
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
    char buffer[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)) == nullptr) continue;
    const string address(buffer);
    if (find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.push_back(address);
  }
  freeifaddrs(interfaces);
  if (addresses.empty()) addresses.push_back("127.0.0.1");
  return addresses;
}

vector<string> privateLocalAddresses() {
  vector<string> addresses;
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return addresses;
  for (auto* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || (entry->ifa_flags & IFF_UP) == 0 ||
        (entry->ifa_flags & IFF_LOOPBACK) != 0 || entry->ifa_addr->sa_family != AF_INET) continue;
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
    const auto hostOrder = ntohl(ipv4->sin_addr.s_addr);
    const auto first = (hostOrder >> 24U) & 0xffU;
    const auto second = (hostOrder >> 16U) & 0xffU;
    const bool isPrivate = first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
                           (first == 192U && second == 168U) || (first == 169U && second == 254U);
    if (!isPrivate) continue;
    char buffer[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)) == nullptr) continue;
    const string address(buffer);
    if (find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.push_back(address);
  }
  freeifaddrs(interfaces);
  return addresses;
}

bool controlModifierPressed() { return false; }
vector<KeyEvent> pollKeys() { return {}; }

}  // namespace inventatory
