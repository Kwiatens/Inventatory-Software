// Inventatory - Hardware Inventory Management System
// Crash-resistant replacement for small local text files.

#include "core/storage/AtomicFile.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
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

bool fsyncDescriptor(int descriptor) {
  while (fsync(descriptor) != 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

// A symbolic link at the destination is followed to the file it names, so replacing the content does
// not replace the link. Returns the destination itself when it is not a link or the chain is cyclic.
filesystem::path resolveLinkedDestination(const filesystem::path& destination) {
  filesystem::path current = destination;
  for (int depth = 0; depth < 16; ++depth) {
    error_code ignored;
    const auto status = filesystem::symlink_status(current, ignored);
    if (ignored || status.type() != filesystem::file_type::symlink) return current;
    const auto target = filesystem::read_symlink(current, ignored);
    if (ignored || target.empty()) return destination;
    current = target.is_absolute() ? target : current.parent_path() / target;
  }
  return destination;
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
    // A signal (the background service uses several) can interrupt a write; that is not a failure.
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      setError(error, systemErrorText("Unable to write temporary file",
                                      error_code(written < 0 ? errno : ENOSPC, generic_category())));
      success = false;
      break;
    }
    offset += static_cast<size_t>(written);
  }
  if (success && !fsyncDescriptor(descriptor)) {
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

#ifdef _WIN32
bool moveFileReplacing(const filesystem::path& source, const filesystem::path& destination,
                       unsigned long& errorCode) {
  constexpr int kAttempts = 5;
  for (int attempt = 1;; ++attempt) {
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
      errorCode = 0;
      return true;
    }
    errorCode = GetLastError();
    const bool transient = errorCode == ERROR_ACCESS_DENIED || errorCode == ERROR_SHARING_VIOLATION ||
                           errorCode == ERROR_LOCK_VIOLATION;
    if (!transient || attempt >= kAttempts) return false;
    Sleep(static_cast<DWORD>(50 * attempt));
  }
}
#endif

bool writeFileAtomically(const filesystem::path& destination, string_view contents, string* error) {
  setError(error, {});

  try {
    if (destination.filename().empty()) {
      setError(error, "Unable to write file: destination has no filename");
      return false;
    }
#ifndef _WIN32
    if (const auto resolved = resolveLinkedDestination(destination); resolved != destination) {
      return writeFileAtomically(resolved, contents, error);
    }
#endif

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
    unsigned long moveError = 0;
    if (!moveFileReplacing(temporary, destination, moveError)) {
      setError(error, win32ErrorText("Unable to replace destination file", static_cast<DWORD>(moveError)));
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
    // The rename itself is only durable once the directory entry has reached the disk.
    string syncError;
    if (!syncDirectory(parent, &syncError)) {
      setError(error, syncError);
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

bool syncFile(const filesystem::path& file, string* error) {
  setError(error, {});
#ifdef _WIN32
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    setError(error, win32ErrorText("Unable to open file for flushing", GetLastError()));
    return false;
  }
  const bool flushed = FlushFileBuffers(handle) != 0;
  const DWORD flushError = flushed ? 0 : GetLastError();
  CloseHandle(handle);
  if (!flushed) {
    setError(error, win32ErrorText("Unable to flush file", flushError));
    return false;
  }
  return true;
#else
  int descriptor;
  do {
    descriptor = open(file.c_str(), O_RDONLY | O_CLOEXEC);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    setError(error, systemErrorText("Unable to open file for flushing", error_code(errno, generic_category())));
    return false;
  }
  const bool flushed = fsyncDescriptor(descriptor);
  const int flushError = errno;
  close(descriptor);
  if (!flushed) {
    setError(error, systemErrorText("Unable to flush file", error_code(flushError, generic_category())));
    return false;
  }
  return true;
#endif
}

bool syncDirectory(const filesystem::path& directory, string* error) {
  setError(error, {});
#ifdef _WIN32
  (void)directory;
  return true;
#else
  const auto target = directory.empty() ? filesystem::path(".") : directory;
  int descriptor;
  do {
    descriptor = open(target.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    setError(error, systemErrorText("Unable to open directory for flushing", error_code(errno, generic_category())));
    return false;
  }
  const bool flushed = fsyncDescriptor(descriptor);
  const int flushError = errno;
  close(descriptor);
  // Some filesystems cannot flush a directory at all; there is nothing more to do for them.
  if (!flushed && flushError != EINVAL && flushError != ENOTSUP && flushError != EROFS) {
    setError(error, systemErrorText("Unable to flush directory", error_code(flushError, generic_category())));
    return false;
  }
  return true;
#endif
}

}  // namespace inventatory
