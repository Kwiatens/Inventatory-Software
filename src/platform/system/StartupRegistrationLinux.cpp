// Inventatory - per-user Linux launcher and systemd service registration.

#include "platform/system/StartupRegistration.h"

#include "core/storage/AtomicFile.h"
#include "platform/system/Environment.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace inventatory {
namespace filesystem = std::filesystem;
namespace {

filesystem::path homeDirectory() {
  if (const auto home = environmentValue("HOME"); home.has_value() && !home->empty()) return filesystem::path(*home);
  return filesystem::current_path();
}

filesystem::path configHome() {
  if (const auto xdg = environmentValue("XDG_CONFIG_HOME"); xdg.has_value() && !xdg->empty() &&
      filesystem::path(*xdg).is_absolute()) return filesystem::path(*xdg);
  return homeDirectory() / ".config";
}

filesystem::path dataHome() {
  if (const auto xdg = environmentValue("XDG_DATA_HOME"); xdg.has_value() && !xdg->empty() &&
      filesystem::path(*xdg).is_absolute()) return filesystem::path(*xdg);
  return homeDirectory() / ".local" / "share";
}

filesystem::path currentExecutablePath() {
  std::vector<char> buffer(4096U);
  for (;;) {
    const auto count = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count < 0) return {};
    if (static_cast<size_t>(count) < buffer.size()) return filesystem::u8path(std::string(buffer.data(), count));
    if (buffer.size() >= 1024U * 1024U) return {};
    buffer.resize(buffer.size() * 2U);
  }
}

std::string desktopQuoted(const std::string& value) {
  std::string encoded = "\"";
  for (const char ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20U || byte == 0x7fU) return {};
    if (ch == '%') encoded += "%%";
    else {
      if (ch == '\\' || ch == '"') encoded.push_back('\\');
      encoded.push_back(ch);
    }
  }
  encoded.push_back('"');
  return encoded;
}

std::string systemdQuoted(const std::string& value) {
  std::string encoded = "\"";
  for (const char ch : value) {
    if (ch == ' ') encoded += "\\x20";
    else if (static_cast<unsigned char>(ch) < 0x20U || static_cast<unsigned char>(ch) == 0x7fU) {
      static constexpr char kHex[] = "0123456789abcdef";
      const auto byte = static_cast<unsigned char>(ch);
      encoded += "\\x";
      encoded.push_back(kHex[(byte >> 4U) & 0x0fU]);
      encoded.push_back(kHex[byte & 0x0fU]);
    }
    else if (ch == '\\') encoded += "\\\\";
    else if (ch == '"') encoded += "\\\"";
    else if (ch == '%') encoded += "%%";
    else if (ch == '$') encoded += "$$";
    else encoded.push_back(ch);
  }
  encoded.push_back('"');
  return encoded;
}

bool runSystemctl(const std::vector<std::string>& arguments, std::string& error) {
  int outputPipe[2]{};
  if (pipe(outputPipe) != 0) {
    error = "Unable to create a system service request";
    return false;
  }
  const auto child = fork();
  if (child < 0) {
    close(outputPipe[0]);
    close(outputPipe[1]);
    error = "Unable to start systemctl";
    return false;
  }
  if (child == 0) {
    close(outputPipe[0]);
    dup2(outputPipe[1], STDOUT_FILENO);
    dup2(outputPipe[1], STDERR_FILENO);
    close(outputPipe[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>("systemctl"));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp("systemctl", argv.data());
    _exit(127);
  }
  close(outputPipe[1]);
  std::string output;
  char buffer[1024];
  for (;;) {
    const auto count = read(outputPipe[0], buffer, sizeof(buffer));
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      close(outputPipe[0]);
      waitpid(child, nullptr, 0);
      error = "Unable to read systemctl's response";
      return false;
    }
    if (output.size() < 4096U) output.append(buffer, std::min<size_t>(static_cast<size_t>(count), 4096U - output.size()));
  }
  close(outputPipe[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
  error = output.empty() ? "systemctl --user is unavailable" : output;
  return false;
}

constexpr const char* kServiceName = "inventatory-background.service";

filesystem::path servicePath() { return configHome() / "systemd" / "user" / kServiceName; }

}  // namespace

std::wstring buildBackgroundStartupLauncherPath(const std::wstring& executablePath) {
  const auto separator = executablePath.find_last_of(L"\\/");
  const auto directory = separator == std::wstring::npos ? std::wstring() : executablePath.substr(0, separator + 1U);
  return directory + L"inventatory-background.exe";
}

std::wstring buildBackgroundStartupCommand(const std::wstring& executablePath) {
  return L"\"" + executablePath + L"\" --background";
}

std::wstring buildDesktopShortcutPath(const std::wstring& desktopDirectory) {
  if (desktopDirectory.empty()) return L"Inventatory.lnk";
  const wchar_t last = desktopDirectory.back();
  if (last == L'\\' || last == L'/') return desktopDirectory + L"Inventatory.lnk";
  return desktopDirectory + L"\\Inventatory.lnk";
}

bool createDesktopShortcut(std::string& error) {
  const auto executable = currentExecutablePath();
  if (executable.empty()) {
    error = "Unable to find the Inventatory executable";
    return false;
  }
  const auto launcherDirectory = dataHome() / "applications";
  std::error_code filesystemError;
  filesystem::create_directories(launcherDirectory, filesystemError);
  if (filesystemError) {
    error = "Unable to create the Inventatory application launcher: " + filesystemError.message();
    return false;
  }
  const auto launcher = launcherDirectory / "inventatory.desktop";
  const auto executableUtf8 = executable.u8string();
  const auto quotedExecutable = desktopQuoted(executableUtf8);
  if (quotedExecutable.empty()) {
    error = "The Inventatory executable path contains characters unsupported by application launchers";
    return false;
  }
  const std::string contents = "[Desktop Entry]\nType=Application\nName=Inventatory\nComment=Hardware inventory\n" +
                               std::string("Exec=") + quotedExecutable +
                               "\nTerminal=true\nCategories=Utility;\n";
  if (!writeFileAtomically(launcher, contents, &error)) return false;
  filesystem::permissions(launcher, filesystem::perms::owner_read | filesystem::perms::owner_write |
                                        filesystem::perms::owner_exec | filesystem::perms::group_read |
                                        filesystem::perms::group_exec | filesystem::perms::others_read |
                                        filesystem::perms::others_exec,
                          filesystem::perm_options::replace, filesystemError);
  if (filesystemError) {
    error = "Unable to make the Inventatory application launcher executable: " + filesystemError.message();
    return false;
  }
  return true;
}

bool setBackgroundStartupEnabled(bool enabled, std::string& error) {
  error.clear();
  const auto unit = servicePath();
  if (!enabled) {
    std::error_code filesystemError;
    const bool exists = filesystem::exists(unit, filesystemError);
    if (filesystemError) {
      error = "Unable to inspect the Inventatory background service file: " + filesystemError.message();
      return false;
    }
    if (!exists) return true;
    if (!runSystemctl({"--user", "disable", "--now", kServiceName}, error)) return false;
    filesystem::remove(unit, filesystemError);
    if (filesystemError) {
      error = "Unable to remove the Inventatory background service file: " + filesystemError.message();
      return false;
    }
    return runSystemctl({"--user", "daemon-reload"}, error);
  }

  const auto executable = currentExecutablePath();
  if (executable.empty()) {
    error = "Unable to find the Inventatory executable for background startup";
    return false;
  }
  std::error_code filesystemError;
  filesystem::create_directories(unit.parent_path(), filesystemError);
  if (filesystemError) {
    error = "Unable to create the user service directory: " + filesystemError.message();
    return false;
  }
  const std::string contents = "[Unit]\nDescription=Inventatory Scan R1 background service\nAfter=network-online.target\n\n" +
                               std::string("[Service]\nType=simple\nExecStart=") +
                               systemdQuoted(executable.u8string()) +
                               " --background\nRestart=on-failure\n\n[Install]\nWantedBy=default.target\n";
  if (!writeFileAtomically(unit, contents, &error)) return false;
  filesystem::permissions(unit, filesystem::perms::owner_read | filesystem::perms::owner_write,
                          filesystem::perm_options::replace, filesystemError);
  if (filesystemError) {
    error = "Unable to protect the Inventatory background service file: " + filesystemError.message();
    return false;
  }
  if (!runSystemctl({"--user", "daemon-reload"}, error) ||
      !runSystemctl({"--user", "enable", kServiceName}, error)) {
    std::error_code ignoredFilesystemError;
    std::string ignoredServiceError;
    filesystem::remove(unit, ignoredFilesystemError);
    runSystemctl({"--user", "daemon-reload"}, ignoredServiceError);
    return false;
  }
  return true;
}

}  // namespace inventatory
