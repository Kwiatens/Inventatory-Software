// Inventatory - Linux desktop and terminal integration.

#include "platform/system/Console.h"

#include "platform/system/ChildProcess.h"
#include "platform/system/Environment.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
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

// Runs a desktop helper (the zenity dialogs) and captures its standard output. The helper's own
// warnings (GTK, dconf, a missing display) are discarded instead of being printed over the terminal
// interface. A dialog waits for the user, so the deadline is only a backstop for a helper that never
// returns.
bool runCaptured(const string& executable, const vector<string>& arguments, string& output) {
  vector<string> command{executable};
  command.insert(command.end(), arguments.begin(), arguments.end());
  ChildProcessOptions options;
  options.timeout = std::chrono::hours(24);
  options.maxOutputBytes = 32768U;
  auto result = runChildProcess(command, options);
  output = result.succeeded() ? std::move(result.output) : string();
  return result.succeeded();
}

// Closes every inherited descriptor above stderr in a forked child just before exec.  Only raw
// system calls are used, so it is async-signal-safe and does not allocate.  Everything the app
// creates is already close-on-exec; this is defence in depth against descriptors from libraries.
void closeInheritedDescriptors() {
#ifdef SYS_close_range
  if (syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif
  for (int descriptor = 3; descriptor < 4096; ++descriptor) close(descriptor);
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
    closeInheritedDescriptors();
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
  // Close-on-exec: the child only needs its dup2()ed copy, and a concurrently forked helper must
  // not keep a write end open (it would stop the reader from seeing EOF).
  if (pipe2(descriptors, O_CLOEXEC) != 0) return false;
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
  fputs("\033]2;", stdout);
  fwrite(safeTitle.data(), 1U, safeTitle.size(), stdout);
  fputs("\007", stdout);
  fflush(stdout);
}

void requestTerminalAttention() {
  fputs("\a", stdout);
  fflush(stdout);
}

void waitForAcknowledgement(int timeoutMs) {
  if (isatty(STDIN_FILENO) == 0) return;
  pollfd input{STDIN_FILENO, POLLIN, 0};
  int ready = 0;
  do {
    ready = poll(&input, 1, timeoutMs);
  } while (ready < 0 && errno == EINTR);
  if (ready > 0 && (input.revents & POLLIN) != 0) {
    char discard[256];
    if (read(STDIN_FILENO, discard, sizeof(discard)) < 0) return;
  }
}

void initializeConsoleWindow() {
  setConsoleTitle("Inventatory");
}

vector<string> buildTerminalCandidateArgs(const string& terminalBinary, const string& executable,
                                          const vector<string>& extraArgs) {
  if (terminalBinary == "konsole") {
    vector<string> args = {
        "konsole",
        "--separate",
        "--hide-menubar",
        "--hide-tabbar",
        "--hide-toolbars",
        "--desktopfile", "inventatory",
        "-qwindowtitle", "Inventatory",
        "-qwindowicon", "inventatory",
        "-p", "Icon=inventatory",
        "-p", "tabtitle=Inventatory",
        "-p", "LocalTabTitleFormat=%w",
        "-p", "RemoteTabTitleFormat=%w",
        "-p", "ShowTerminalSizeHint=false",
        "-p", "ScrollBarPosition=2",
        "-e", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "gnome-terminal") {
    vector<string> args = {"gnome-terminal", "--hide-menubar", "--class=inventatory", "--title=Inventatory", "--", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "ptyxis") {
    vector<string> args = {"ptyxis", "--standalone", "--app-id=inventatory", "--title=Inventatory", "--", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "alacritty") {
    vector<string> args = {"alacritty", "--class", "inventatory,inventatory", "--title", "Inventatory", "-e", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "kitty") {
    vector<string> args = {"kitty", "--class", "inventatory", "-T", "Inventatory", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "foot") {
    vector<string> args = {"foot", "--app-id", "inventatory", "-T", "Inventatory", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "wezterm") {
    vector<string> args = {"wezterm", "start", "--class", "inventatory", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "xterm") {
    vector<string> args = {"xterm", "+sb", "-class", "inventatory", "-title", "Inventatory", "-e", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  if (terminalBinary == "xdg-terminal-exec") {
    vector<string> args = {"xdg-terminal-exec", "--app-id=inventatory", executable};
    args.insert(args.end(), extraArgs.begin(), extraArgs.end());
    return args;
  }
  vector<string> args = {"x-terminal-emulator", "-e", executable};
  args.insert(args.end(), extraArgs.begin(), extraArgs.end());
  return args;
}

bool ensureTerminalAttached(int argc, char* argv[]) {
  if (isatty(STDIN_FILENO) != 0) {
    return true;
  }

  string executable = currentExecutablePath().string();
  if (executable.empty() && argc > 0 && argv[0] != nullptr) {
    executable = argv[0];
  }
  if (executable.empty()) return false;

  vector<string> extraArgs;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr) extraArgs.emplace_back(argv[i]);
  }

  const char* desktopEnv = getenv("XDG_CURRENT_DESKTOP");
  string desktopLower = desktopEnv ? desktopEnv : "";
  for (auto& ch : desktopLower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
  const bool prefersKde = desktopLower.find("kde") != string::npos || desktopLower.find("plasma") != string::npos;
  const bool prefersGnome = desktopLower.find("gnome") != string::npos || desktopLower.find("unity") != string::npos;

  struct TerminalCandidate {
    const char* binary;
    int priority;
  };

  vector<TerminalCandidate> candidates = {
      {"konsole", prefersKde ? 0 : 10},
      {"gnome-terminal", prefersGnome ? 0 : 11},
      {"ptyxis", prefersGnome ? 1 : 12},
      {"alacritty", 20},
      {"kitty", 21},
      {"foot", 22},
      {"wezterm", 23},
      {"xterm", 30},
      {"xdg-terminal-exec", 40},
      {"x-terminal-emulator", 50},
  };

  std::sort(candidates.begin(), candidates.end(), [](const TerminalCandidate& a, const TerminalCandidate& b) {
    return a.priority < b.priority;
  });

  for (const auto& candidate : candidates) {
    if (!executableAvailable(candidate.binary)) continue;
    const auto cmdArgs = buildTerminalCandidateArgs(candidate.binary, executable, extraArgs);
    vector<char*> execArgs;
    execArgs.reserve(cmdArgs.size() + 1U);
    for (const auto& arg : cmdArgs) {
      execArgs.push_back(const_cast<char*>(arg.c_str()));
    }
    execArgs.push_back(nullptr);
    execvp(execArgs[0], execArgs.data());
  }

  std::fprintf(stderr, "Inventatory requires a terminal emulator to run interactively.\n");
  return false;
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
