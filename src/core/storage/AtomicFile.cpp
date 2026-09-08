// Inventatory - Hardware Inventory Management System
// Crash-resistant replacement for small local text files.

#include "core/storage/AtomicFile.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace inventatory {

using namespace std;

namespace {

atomic<uint64_t> temporarySequence{0};

void setError(string* error, string message) {
  if (error != nullptr) *error = move(message);
}

string systemErrorText(const string& operation, const error_code& error) {
  ostringstream output;
  output << operation;
  if (error) output << " (" << error.message() << ')';
  return output.str();
}

#ifdef _WIN32

string win32ErrorText(const string& operation, DWORD errorCode) {
  ostringstream output;
  output << operation << " (Win32 error " << errorCode << ')';
  return output.str();
}

filesystem::path uniqueTemporaryPath(const filesystem::path& destination, uint64_t sequence) {
  const auto name = destination.filename().wstring() + L".tmp-" +
                    to_wstring(static_cast<unsigned long long>(GetCurrentProcessId())) + L"-" +
                    to_wstring(static_cast<unsigned long long>(sequence));
  return destination.parent_path() / filesystem::path(name);
}

bool writeWindowsFile(const filesystem::path& temporary, string_view contents, string* error, bool& collision,
                      bool& owned) {
  owned = false;
  HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD errorCode = GetLastError();
    collision = errorCode == ERROR_FILE_EXISTS || errorCode == ERROR_ALREADY_EXISTS;
    if (!collision) setError(error, win32ErrorText("Unable to create temporary file", errorCode));
    return false;
  }
  collision = false;
  owned = true;

  bool success = true;
  size_t offset = 0;
  while (offset < contents.size()) {
    const auto remaining = contents.size() - offset;
    const DWORD requested = static_cast<DWORD>(min<size_t>(remaining, numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (WriteFile(handle, contents.data() + offset, requested, &written, nullptr) == 0 || written != requested) {
      setError(error, win32ErrorText("Unable to write temporary file", GetLastError()));
      success = false;
      break;
    }
    offset += written;
  }
  if (success && FlushFileBuffers(handle) == 0) {
    setError(error, win32ErrorText("Unable to flush temporary file", GetLastError()));
    success = false;
  }
  if (CloseHandle(handle) == 0 && success) {
    setError(error, win32ErrorText("Unable to close temporary file", GetLastError()));
    success = false;
  }
  return success;
}

#else

filesystem::path uniqueTemporaryPath(const filesystem::path& destination, uint64_t sequence) {
  const auto name = destination.filename().string() + ".tmp-" + to_string(static_cast<unsigned long long>(getpid())) +
                    "-" + to_string(static_cast<unsigned long long>(sequence));
  return destination.parent_path() / filesystem::path(name);
}

bool writePosixFile(const filesystem::path& temporary, string_view contents, string* error, bool& collision,
                    bool& owned) {
  owned = false;
  const int descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    collision = errno == EEXIST;
    if (!collision) {
      setError(error, systemErrorText("Unable to create temporary file", error_code(errno, generic_category())));
    }
    return false;
  }
  collision = false;
  owned = true;

  bool success = true;
  size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written = write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written <= 0) {
      setError(error, systemErrorText("Unable to write temporary file", error_code(errno, generic_category())));
      success = false;
      break;
    }
    offset += static_cast<size_t>(written);
  }
  if (success && fsync(descriptor) != 0) {
    setError(error, systemErrorText("Unable to flush temporary file", error_code(errno, generic_category())));
    success = false;
  }
  if (close(descriptor) != 0 && success) {
    setError(error, systemErrorText("Unable to close temporary file", error_code(errno, generic_category())));
    success = false;
  }
  return success;
}

#endif

bool removeOwnedTemporary(const filesystem::path& temporary) {
  error_code ignored;
  return filesystem::remove(temporary, ignored) || !filesystem::exists(temporary, ignored);
}

}  // namespace

bool writeFileAtomically(const filesystem::path& destination, string_view contents, string* error) {
  setError(error, {});

  try {
    if (destination.filename().empty()) {
      setError(error, "Unable to write file: destination has no filename");
      return false;
    }

    auto parent = destination.parent_path();
    if (parent.empty()) parent = filesystem::current_path();

    error_code filesystemError;
    filesystem::create_directories(parent, filesystemError);
    if (filesystemError) {
      setError(error, systemErrorText("Unable to create destination directory", filesystemError));
      return false;
    }
    if (!filesystem::is_directory(parent, filesystemError) || filesystemError) {
      setError(error, systemErrorText("Destination parent is not a directory", filesystemError));
      return false;
    }

    filesystem::path temporary;
    bool written = false;
    for (int attempt = 0; attempt < 32 && !written; ++attempt) {
      const uint64_t sequence = temporarySequence.fetch_add(1, memory_order_relaxed);
      temporary = uniqueTemporaryPath(destination, sequence);
      bool collision = false;
      bool owned = false;
#ifdef _WIN32
      written = writeWindowsFile(temporary, contents, error, collision, owned);
#else
      written = writePosixFile(temporary, contents, error, collision, owned);
#endif
      if (!written && !collision) {
        if (owned) removeOwnedTemporary(temporary);
        return false;
      }
    }
    if (!written) {
      setError(error, "Unable to reserve and write a unique temporary file");
      return false;
    }

#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
      setError(error, win32ErrorText("Unable to replace destination file", GetLastError()));
      removeOwnedTemporary(temporary);
      return false;
    }
#else
    filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
      setError(error, systemErrorText("Unable to replace destination file", filesystemError));
      removeOwnedTemporary(temporary);
      return false;
    }
#endif
    return true;
  } catch (const filesystem::filesystem_error& exception) {
    setError(error, string("Unable to write file: ") + exception.what());
    return false;
  } catch (const exception& exception) {
    setError(error, string("Unable to write file: ") + exception.what());
    return false;
  }
}

}  // namespace inventatory
